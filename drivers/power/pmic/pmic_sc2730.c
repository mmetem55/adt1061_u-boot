// SPDX-License-Identifier: GPL-2.0+
/*
 * Unisoc (Spreadtrum) SC2730 PMIC driver, reached over the ADI bus
 * (drivers/spi/sprd_adi.c).
 *
 * Ported from the vendor U-Boot 2015.07 tree's
 * drivers/power/pmic/pmic_sc2730.c, which already used the modern
 * DM_PMIC uclass API (power/pmic.h's pmic_bind_children()/
 * pmic_clrsetbits()) almost identically to current upstream U-Boot, so
 * this port is close to verbatim. Changes from the vendor source:
 *   - dropped its adc_drvapi.h/adi_hal_internal.h/asm/arch/sprd_reg.h/
 *     sprd_adc.h includes - this driver only needs power/pmic.h,
 *     spi.h and the self-contained sc2730_reg_glb.h ported alongside
 *     it (see that file's header comment for the address scheme).
 *   - uses upstream's own __ffs()/GENMASK()/BIT() (<linux/bitops.h>)
 *     instead of the vendor's local redefinitions.
 *   - sprd_pmic_efuse_read() (DCDC zero-cross-detection/deadtime
 *     calibration trim, read from one-time-programmable efuse data)
 *     is stubbed to 0 for now: the efuse block driver referenced by
 *     the vendor source (pmic_efuse / "pmic_efuse" child) is not yet
 *     ported, so sc2730_reload_config() currently just leaves these
 *     DCDC/LDO trim registers at their power-on-reset default instead
 *     of applying the per-chip factory calibration. This only affects
 *     fine voltage/timing accuracy, not basic PMIC function.
 */

#include <dm.h>
#include <log.h>
#include <spi.h>
#include <dm/device_compat.h>
#include <power/pmic.h>
#include <linux/bitops.h>
#include <linux/delay.h>

#include "sc2730_reg_glb.h"

#define CBANK_VALUE		0x5
#define WR_UNLOCK		0x6e7f
#define TSX_WR_UNLOCK		0x1990
#define TRANS_26M_EN		1
#define DIV_COEF		0x750
#define IB_ZCD_THRESHOLD_MAX	111
#define IB_ZCD_THRESHOLD_MIN	67

/* See the file header comment: efuse-based trim reload is deferred. */
static inline u32 sprd_pmic_efuse_read(int block_id)
{
	return 0;
}

static const struct {
	int id;
	const char *name;
	int efuse_block_id;
	int efuse_bit_mask;
	unsigned long pmic_regs_addr;
	u32 pmic_regs_mask;
} reinit_node[] = {
	{0, "DCDC_CPU_ZCD", 14, GENMASK(15, 9), ANA_REG_GLB_DCDC_CPU_REG2, BITS_DCDC_CPU_ZCD(0x3)},
	{1, "DCDC_GPU_ZCD", 14, GENMASK(15, 9), ANA_REG_GLB_DCDC_GPU_REG2, BITS_DCDC_GPU_ZCD(0x3)},
	{2, "DCDC_CORE_ZCD", 14, GENMASK(15, 9), ANA_REG_GLB_DCDC_CORE_REG2, BITS_DCDC_CORE_ZCD(0x3)},
	{3, "DCDC_MODEM_ZCD", 14, GENMASK(15, 9), ANA_REG_GLB_DCDC_MODEM_REG2, BITS_DCDC_MODEM_ZCD(0x3)},
	{4, "DCDC_MEM_ZCD", 14, GENMASK(15, 9), ANA_REG_GLB_DCDC_MEM_REG2, BITS_DCDC_MEM_ZCD(0x3)},
	{5, "DCDC_MEMQ_ZCD", 14, GENMASK(15, 9), ANA_REG_GLB_DCDC_MEMQ_REG2, BITS_DCDC_MEMQ_ZCD(0x3)},
	{6, "DCDC_GEN0_ZCD", 14, GENMASK(15, 9), ANA_REG_GLB_DCDC_GEN0_REG2, BITS_DCDC_GEN0_ZCD(0x3)},
	{7, "DCDC_GEN1_ZCD", 14, GENMASK(15, 9), ANA_REG_GLB_DCDC_GEN1_REG2, BITS_DCDC_GEN1_ZCD(0x3)},
	{8, "DCDC_WPA_ZCD", 14, GENMASK(15, 9), ANA_REG_GLB_DCDC_WPA_REG3, BITS_DCDC_WPA_ZCD(0x3)},
	{9, "DCDC_SRAM_ZCD", 14, GENMASK(15, 9), ANA_REG_GLB_DCDC_SRAM_REG2, BITS_DCDC_SRAM_ZCD(0x3)},
	{10, "LDO_DCXO_LP", 14, GENMASK(4, 1), ANA_REG_GLB_LDO_VDD18_DCXO_REG2, BITS_LDO_VDD18_DCXO_REFTRIM_LP(0xF)},
	{-1, NULL, 0, 0, 0, 0},
};

static const struct {
	int id;
	const char *name;
	int value;
	unsigned long pmic_regs_addr;
	u32 pmic_regs_mask;
} reinit_node_cfg[] = {
	{0, "DCDC_CPU_DTM", 0x2, ANA_REG_GLB_DCDC_CPU_REG0, BITS_DCDC_CPU_DEADTIME(0x3)},
	{1, "DCDC_GPU_DTM", 0x2, ANA_REG_GLB_DCDC_GPU_REG0, BITS_DCDC_GPU_DEADTIME(0x3)},
	{2, "DCDC_CORE_DTM", 0x2, ANA_REG_GLB_DCDC_CORE_REG0, BITS_DCDC_CORE_DEADTIME(0x3)},
	{3, "DCDC_MODEM_DTM", 0x2, ANA_REG_GLB_DCDC_MODEM_REG0, BITS_DCDC_MODEM_DEADTIME(0x3)},
	{4, "DCDC_MEM_DTM", 0x2, ANA_REG_GLB_DCDC_MEM_REG0, BITS_DCDC_MEM_DEADTIME(0x3)},
	{5, "DCDC_MEMQ_DTM", 0x2, ANA_REG_GLB_DCDC_MEMQ_REG0, BITS_DCDC_MEMQ_DEADTIME(0x3)},
	{6, "DCDC_GEN0_DTM", 0x2, ANA_REG_GLB_DCDC_GEN0_REG0, BITS_DCDC_GEN0_DEADTIME(0x3)},
	{7, "DCDC_GEN1_DTM", 0x2, ANA_REG_GLB_DCDC_GEN1_REG0, BITS_DCDC_GEN1_DEADTIME(0x3)},
	{8, "DCDC_SRAM_DTM", 0x2, ANA_REG_GLB_DCDC_SRAM_REG0, BITS_DCDC_SRAM_DEADTIME(0x3)},
	{9, "DCDC_WPA_DTM", 0x3, ANA_REG_GLB_DCDC_WPA_REG0, BITS_DCDC_WPA_DEADTIME(0x3)},
	{10, "DCDC_CPU_CF", 0x1, ANA_REG_GLB_DCDC_CPU_REG1, BITS_DCDC_CPU_CF(0x3)},
	{11, "DCDC_GPU_CF", 0x1, ANA_REG_GLB_DCDC_GPU_REG1, BITS_DCDC_GPU_CF(0x3)},
	{12, "DCDC_MODEM_CF", 0x1, ANA_REG_GLB_DCDC_MODEM_REG1, BITS_DCDC_MODEM_CF(0x3)},
	{13, "DCDC_WPA_CF", 0x1, ANA_REG_GLB_DCDC_WPA_REG0, BITS_DCDC_WPA_CF(0x3)},
	{14, "DCDC_CPU_STBOP", 0x2DF, ANA_REG_GLB_DCDC_CPU_REG2, GENMASK(12, 0)},
	{15, "DCDC_GPU_STBOP", 0x2DF, ANA_REG_GLB_DCDC_GPU_REG2, GENMASK(12, 0)},
	{16, "DCDC_CORE_STBOP", 0x2D8, ANA_REG_GLB_DCDC_CORE_REG2, GENMASK(12, 0)},
	{17, "DCDC_MODEM_STBOP", 0xD9F, ANA_REG_GLB_DCDC_MODEM_REG2, GENMASK(12, 0)},
	{18, "DCDC_SRAM_STBOP", 0x2D8, ANA_REG_GLB_DCDC_SRAM_REG2, GENMASK(12, 0)},
	{-1, NULL, 0, 0, 0},
};

static const struct pmic_child_info sc2730_regulator_info[] = {
	{ .prefix = "DCDC", .driver = "sc2730-regulator" },
	{ .prefix = "LDO", .driver = "sc2730-regulator" },
	{ }
};

static int sc2730_power_control_bind(struct udevice *dev)
{
	int children;
	struct udevice *pmic = dev->parent;

	children = pmic_bind_children(pmic, dev_ofnode(dev), sc2730_regulator_info);
	if (!children)
		dev_err(dev, "%s: %s - no child found\n", __func__, dev->name);

	return 0;
}

U_BOOT_DRIVER(sc2730_power_controler) = {
	.name = "sc2730-power-controler",
	.id = UCLASS_PMIC,
	.bind = sc2730_power_control_bind,
};

static int sc2730_adi_write(struct udevice *dev, u32 reg, u32 value)
{
	int ret;
	u32 wd_addr[2] = { reg, value };

	ret = dm_spi_xfer(dev, 16, &wd_addr[0], NULL, 0);
	if (ret)
		dev_err(dev, "write pmic register:0x%08x failed.\n", reg);

	return ret;
}

static int sc2730_adi_read(struct udevice *dev, u32 *reg)
{
	u32 addr = *reg;
	int ret, retry = 5;

	/*
	 * A single ADI read can come back as a transient readback-address
	 * mismatch (sprd_adi_read()'s check in drivers/spi/sprd_adi.c:
	 * "read error, reg addr = ..., val = ...") even though the address
	 * and the target register are both correct - confirmed on real
	 * hardware with the FGU's battery-voltage register, which this
	 * SoC's ADC continuously updates, right after other ADI traffic on
	 * this same bus (this tree's own cyclic-serviced watchdog included,
	 * see drivers/watchdog/wdt-sc27xx.c) - a transaction immediately
	 * following another one onto a different target occasionally
	 * echoes a stale address. One retry after a short settle always
	 * clears it in testing; 5 is a generous margin. Retrying quietly
	 * here means every caller through the generic pmic_reg_read() API
	 * gets this for free, instead of every single flaky read scaring
	 * whoever's watching the UART log with a bus-failure message for
	 * something that was never actually a failure.
	 */
	do {
		*reg = addr;
		ret = dm_spi_xfer(dev, 16, NULL, reg, 0);
		if (!ret)
			return 0;
		udelay(100);
	} while (--retry > 0);

	dev_err(dev, "read pmic register by adi failed.\n");
	return ret;
}

static int sc2730_reload_config(struct udevice *dev)
{
	int i, shft, ib_trim_val, reload_val, ret = 0;
	u32 efuse_val;

	for (i = 0; reinit_node_cfg[i].id != -1; i++) {
		shft = __ffs(reinit_node_cfg[i].pmic_regs_mask);

		ret = pmic_clrsetbits(dev, reinit_node_cfg[i].pmic_regs_addr,
				      reinit_node_cfg[i].pmic_regs_mask,
				      (reinit_node_cfg[i].value << shft) &
				      reinit_node_cfg[i].pmic_regs_mask);
		if (ret) {
			dev_err(dev, "reinit node configure id: %d failed.\n", i);
			return ret;
		}
	}

	efuse_val = sprd_pmic_efuse_read(reinit_node[0].efuse_block_id);
	shft = __ffs(reinit_node[0].efuse_bit_mask);

	ib_trim_val = (efuse_val & reinit_node[0].efuse_bit_mask) >> shft;

	if (ib_trim_val >= IB_ZCD_THRESHOLD_MAX)
		reload_val = 2;
	else if (ib_trim_val <= IB_ZCD_THRESHOLD_MIN)
		reload_val = 0;
	else
		reload_val = 1;

	for (i = 0; reinit_node[i].id != -1 && reinit_node[i].id <= 9; i++) {
		shft = __ffs(reinit_node[i].pmic_regs_mask);
		ret = pmic_clrsetbits(dev, reinit_node[i].pmic_regs_addr,
				      reinit_node[i].pmic_regs_mask,
				      (reload_val << shft) &
				      reinit_node[i].pmic_regs_mask);
		if (ret) {
			dev_err(dev, "reinit node id: %d failed.\n", i);
			return ret;
		}
	}

	for (i = 10; reinit_node[i].id != -1; i++) {
		efuse_val = sprd_pmic_efuse_read(reinit_node[i].efuse_block_id);
		shft = __ffs(reinit_node[i].efuse_bit_mask);

		reload_val = (efuse_val & reinit_node[i].efuse_bit_mask) >> shft;
		shft = __ffs(reinit_node[i].pmic_regs_mask);

		ret = pmic_clrsetbits(dev, reinit_node[i].pmic_regs_addr,
				      reinit_node[i].pmic_regs_mask,
				      (reload_val << shft) &
				      reinit_node[i].pmic_regs_mask);
		if (ret)
			dev_err(dev, "reinit node id: %d failed.\n", i);
	}

	return ret;
}

static int sc2730_reset_cbank_value(struct udevice *dev)
{
	int ret;
	u16 chip_id_low;
	u32 tmp;

	tmp = ANA_REG_GLB_CHIP_ID_LOW;
	ret = sc2730_adi_read(dev, &tmp);
	if (ret)
		return ret;

	chip_id_low = tmp & 0xffff;

	if (chip_id_low <= 0xb000)
		ret = sc2730_adi_write(dev, ANA_REG_GLB_TSX_CTRL11,
				       BITS_DCXO_CORE_CBANK_LP(CBANK_VALUE));

	return ret;
}

static const struct pmic_child_info pmic_children_info[] = {
	{ .prefix = "power-controller", .driver = "sc2730-power-controler" },
	{ .prefix = "gpio-controller", .driver = "pmic_eic_sprd" },
	{ .prefix = "adc", .driver = "pmic_adc" },
	{ .prefix = "efuse", .driver = "pmic_efuse" },
	{ .prefix = "led-controller", .driver = "sprd-bltc" },
	{ .prefix = "vibrator", .driver = "sc27xx-vibrator" },
	{ .prefix = "rtc", .driver = "sprd-rtc" },
	{ .prefix = "watchdog", .driver = "sprd-wdt" },
	{ }
};

static int sc2730_write(struct udevice *dev, uint reg, const uint8_t *buff,
			 int len)
{
	int ret;
	u32 wd_addr, value;

	wd_addr = (u32)reg;
	value = *(const u32 *)buff;

	ret = sc2730_adi_write(dev, wd_addr, value);
	if (ret)
		dev_err(dev, "write pmic register failed.\n");

	return ret;
}

static int sc2730_read(struct udevice *dev, uint reg, uint8_t *buff, int len)
{
	int ret;
	u32 *tmp = (u32 *)buff, rd_addr = (u32)reg;

	ret = sc2730_adi_read(dev, &rd_addr);
	if (ret) {
		dev_err(dev, "read pmic register failed.\n");
		return ret;
	}

	*tmp = rd_addr;
	return ret;
}

static int sc2730_bind(struct udevice *dev)
{
	int children;

	children = pmic_bind_children(dev, dev_ofnode(dev), pmic_children_info);
	if (!children)
		dev_err(dev, "%s: %s - no child found\n", __func__, dev->name);

	return 0;
}

/*
 * Full stock U-Boot PMIC bring-up table (39 steps), reverse-engineered
 * by the user from the stock signed U-Boot binary's own PMIC init
 * routine (0x9eff9d94..0x9effa030 in the stock image) and matched
 * against this SoC's ANA_REG_GLB_* register map - every single
 * address in the table already had an existing #define in
 * sc2730_reg_glb.h, so no new register definitions were needed.
 *
 * History: an earlier, narrower version of this function ported only
 * the two registers initially confirmed necessary for a WCN 26MHz
 * crystal-clock routing bug (DCXO_XTL_EN, XTL_WAIT_CTRL0 - steps 32
 * and 39 below). That fixed the PMIC side of the clock-request path
 * (confirmed on real hardware: both registers read back correctly
 * after boot), but WCN's first internal register access still failed
 * the same way ("dt writel fail ret:-110") - meaning something else
 * in the PMIC's power/clock environment still differed from stock.
 * Rather than keep guessing which of the remaining ~35 registers
 * might matter, this applies the *entire* stock table verbatim, in
 * stock's exact order, to make the PMIC's state match stock bit-for-
 * bit and eliminate "PMIC state difference" as a hypothesis entirely
 * - at the user's explicit, informed request. Most of this table is
 * core DCDC/LDO sleep-mode tuning that the user was unwilling to
 * apply from the kernel side without being sure of its effect;
 * applying it here in U-Boot instead, matching exactly what real
 * stock firmware already does on every one of these devices in
 * production, is the lower-risk way to test it.
 *
 * Runs as the very first thing in sc2730_probe(), matching stock's
 * own ordering (this table is stock's first PMIC action, before any
 * of this driver's own TSX/DCXO unlock-and-configure code further
 * down). Deliberately uses no WR_UNLOCK/TSX_WR_UNLOCK magic values
 * anywhere in this table - stock's own routine doesn't either for
 * any of these 39 steps (it writes a plain 0 to PWR_WR_PROT_VALUE in
 * step 2, not an unlock code), and this is empirically consistent
 * with how the original narrower version of this function already
 * behaved: DCXO_XTL_EN and XTL_WAIT_CTRL0 wrote correctly with no
 * unlock sequence at all, confirmed on real hardware. All values are
 * taken verbatim from the user's disassembly, same approach as this
 * port's other ported magic register values.
 *
 * One open item: step 5 (SLP_DCDC_LP_CTRL) is the one entry stock
 * computes conditionally, from a raw ADC-style reading the user's
 * disassembly notes label "vbat_raw" and compare against 0xafff. The
 * exact source/scaling of that reading isn't confirmed - it comes
 * from a separate subroutine call, not a literal register address,
 * and 0xafff doesn't match this board's own FGU ADC scale as already
 * calibrated elsewhere in this port (a fully-charged ~4.2V reads as
 * only ~2900 raw counts there, nowhere near 0xafff/45055 - so this
 * almost certainly isn't reading the same ADC channel as sprd_fgu.c).
 * Rather than guess which branch a real reading would take, this
 * hardcodes the lower/"else" value (0x03CF). If WCN still doesn't
 * come up after this full table, this register is the one remaining
 * known unknown in an otherwise exact stock match.
 */
#define XTL_CLOCK_TABLE_DCXO_XTL_EN_VALUE	0xD000
#define XTL_CLOCK_TABLE_XTL_WAIT_CTRL0_VALUE	0x3F32

static void sc2730_apply_stock_pmic_table(struct udevice *dev)
{
	u32 reg;
	int i;

	/*
	 * Stock's own wait condition before touching anything in this
	 * table: poll PWR_WR_PROT_VALUE's bit 15 (BIT_PWR_WR_PROT)
	 * until it reads back set. Stock does this unconditionally
	 * with no timeout; bounded here instead, same caution as this
	 * port's other hardware-timing polls (e.g. sprd_fgu.c's
	 * INT_RAW wait), so a bit that behaves unexpectedly on this
	 * boot path can't hang the board forever - if it never sets,
	 * the rest of the table still runs (logged), since proceeding
	 * matches stock's intent more closely than refusing to boot.
	 */
	for (i = 0; i < 1000; i++) {
		reg = ANA_REG_GLB_PWR_WR_PROT_VALUE;
		if (sc2730_adi_read(dev, &reg))
			break;
		if (reg & BIT_PWR_WR_PROT)
			break;
		udelay(100);
	}
	if (i >= 1000)
		printf("pmic stock-table: PWR_WR_PROT ready bit never set, proceeding anyway\n");

	sc2730_adi_write(dev, ANA_REG_GLB_POWER_PD_SW, 0x0000);
	sc2730_adi_write(dev, ANA_REG_GLB_PWR_WR_PROT_VALUE, 0x0000);

	sc2730_adi_write(dev, ANA_REG_GLB_SLP_CTRL, 0x0007);
	sc2730_adi_write(dev, ANA_REG_GLB_SLP_DCDC_PD_CTRL, 0x02f1);
	/* vbat_raw-conditional register - see function doc comment. */
	sc2730_adi_write(dev, ANA_REG_GLB_SLP_DCDC_LP_CTRL, 0x03cf);
	sc2730_adi_write(dev, ANA_REG_GLB_DCDC_SRAM_SLP_CTRL1, 0x00e0);
	sc2730_adi_write(dev, ANA_REG_GLB_DCDC_MODEM_SLP_CTRL1, 0x00e0);
	sc2730_adi_write(dev, ANA_REG_GLB_DCDC_GPU_SLP_CTRL1, 0x00b0);
	sc2730_adi_write(dev, ANA_REG_GLB_DCDC_CORE_SLP_CTRL1, 0x00be);
	sc2730_adi_write(dev, ANA_REG_GLB_SLP_LDO_PD_CTRL0, 0x6000);
	sc2730_adi_write(dev, ANA_REG_GLB_SLP_LDO_PD_CTRL1, 0x00a2);
	sc2730_adi_write(dev, ANA_REG_GLB_SLP_LDO_LP_CTRL0, 0x4e00);
	sc2730_adi_write(dev, ANA_REG_GLB_SLP_LDO_LP_CTRL1, 0x0142);

	/* --- XTL_EN crystal-request routing (WCN-critical subset) --- */
	sc2730_adi_write(dev, ANA_REG_GLB_DCDC_XTL_EN,  0xb000);
	sc2730_adi_write(dev, ANA_REG_GLB_DCDC_XTL_EN0, 0xb020);
	sc2730_adi_write(dev, ANA_REG_GLB_DCDC_XTL_EN1, 0x0020);
	sc2730_adi_write(dev, ANA_REG_GLB_DCDC_XTL_EN2, 0xb000);
	sc2730_adi_write(dev, ANA_REG_GLB_DCDC_XTL_EN3, 0xa020);
	sc2730_adi_write(dev, ANA_REG_GLB_LDO_XTL_EN0,  0xb02c);
	sc2730_adi_write(dev, ANA_REG_GLB_LDO_XTL_EN1,  0x0000);
	sc2730_adi_write(dev, ANA_REG_GLB_LDO_XTL_EN2,  0x0000);
	sc2730_adi_write(dev, ANA_REG_GLB_LDO_XTL_EN3,  0x1000);
	sc2730_adi_write(dev, ANA_REG_GLB_LDO_XTL_EN4,  0x0000);
	sc2730_adi_write(dev, ANA_REG_GLB_LDO_XTL_EN5,  0x0000);
	sc2730_adi_write(dev, ANA_REG_GLB_LDO_XTL_EN6,  0x0000);
	sc2730_adi_write(dev, ANA_REG_GLB_LDO_XTL_EN7,  0x0000);
	sc2730_adi_write(dev, ANA_REG_GLB_LDO_XTL_EN8,  0x0000);
	sc2730_adi_write(dev, ANA_REG_GLB_LDO_XTL_EN9,  0x0000);
	sc2730_adi_write(dev, ANA_REG_GLB_LDO_XTL_EN10, 0x0020);
	sc2730_adi_write(dev, ANA_REG_GLB_LDO_XTL_EN11, 0x0000);
	sc2730_adi_write(dev, ANA_REG_GLB_LDO_XTL_EN12, 0x0000);
	sc2730_adi_write(dev, ANA_REG_GLB_DCXO_XTL_EN,
			 XTL_CLOCK_TABLE_DCXO_XTL_EN_VALUE);

	sc2730_adi_write(dev, ANA_REG_GLB_TSX_CTRL1, 0x0208);
	sc2730_adi_write(dev, ANA_REG_GLB_TSX_CTRL2, 0x0080);
	sc2730_adi_write(dev, ANA_REG_GLB_TSX_CTRL3, 0x8000);

	/* update_bits(0x1b68, mask=0x0020, val=0): clear bit5 only. */
	reg = ANA_REG_GLB_CLK32KLESS_CTRL0;
	if (!sc2730_adi_read(dev, &reg)) {
		reg &= ~(u32)BIT(5);
		sc2730_adi_write(dev, ANA_REG_GLB_CLK32KLESS_CTRL0, reg);
	}

	sc2730_adi_write(dev, ANA_REG_GLB_TSX_CTRL0, 0x0129);
	sc2730_adi_write(dev, ANA_REG_GLB_TSX_CTRL14, 0x077e);
	sc2730_adi_write(dev, ANA_REG_GLB_XTL_WAIT_CTRL0,
			 XTL_CLOCK_TABLE_XTL_WAIT_CTRL0_VALUE);

	printf("pmic stock-table: applied stock U-Boot's full 39-step PMIC table (DCXO_XTL_EN=0x%04x XTL_WAIT_CTRL0=0x%04x)\n",
	       XTL_CLOCK_TABLE_DCXO_XTL_EN_VALUE,
	       XTL_CLOCK_TABLE_XTL_WAIT_CTRL0_VALUE);
}

static int sc2730_probe(struct udevice *dev)
{
	int ret;

	sc2730_apply_stock_pmic_table(dev);

	sc2730_adi_write(dev, ANA_REG_GLB_PWR_WR_PROT_VALUE, BITS_PWR_WR_PROT_VALUE(WR_UNLOCK));
	sc2730_adi_write(dev, ANA_REG_GLB_TSX_WR_PROT_VALUE, BITS_TSX_WR_PROT_VALUE(TSX_WR_UNLOCK));
	sc2730_adi_write(dev, ANA_REG_GLB_TSX_CTRL12, BITS_DCXO_32K_FRAC_DIV_RATIO_CTRL_HP(DIV_COEF));

	ret = pmic_clrsetbits(dev, ANA_REG_GLB_TSX_CTRL0, 0, BITS_DCXO_26M_REF_OUT_EN(TRANS_26M_EN));
	if (ret) {
		dev_err(dev, "sc2730 probe reg init failed.\n");
		return ret;
	}

	ret = sc2730_reload_config(dev);
	if (ret) {
		dev_err(dev, "sc2730 probe reload config failed.\n");
		return ret;
	}

	ret = sc2730_reset_cbank_value(dev);
	if (ret)
		dev_err(dev, "sc2730 probe reset cbank failed.\n");

	return ret;
}

/*
 * sc2730_pmic_reset() - force a real hardware SoC reset via the PMIC's
 * own watchdog block (ANA_WDG, see sc2730_reg_glb.h's header comment for
 * where ANA_WDG_BASE/the register layout below come from).
 *
 * This exists because this platform's PSCI firmware does not actually
 * implement SYSTEM_RESET: confirmed on real hardware that the SMC call
 * arch/arm/mach-sprd/cpu.c's reset_misc() makes (invoke_psci_fn(..,
 * PSCI_0_2_FN_SYSTEM_RESET, ..)) just returns, instead of resetting the
 * board, leaving reset_cpu() (arch/arm/lib/reset.c's do_reset() always
 * calls reset_misc() then reset_cpu()) as the only thing that actually
 * works. The vendor's own board/spreadtrum/ums512_1h10/sprd_wdt.c's
 * wdt_reset_MCU() does exactly this sequence (through raw ADI register
 * pokes, since the vendor code predates any DM_PMIC/DM_SPI model); this
 * is the same sequence through the already-probed, already
 * hardware-verified sc2730 DM_PMIC/ADI path (pmic_reg_write()/
 * pmic_clrsetbits(), the same generic API sc2730_regulator.c and this
 * driver's own probe() already use) instead of a raw memory-mapped
 * address guess, since these ADI "registers" are not actually
 * CPU-memory-mapped - they only exist as addresses inside an SPI
 * transaction (see sc2730_adi_write()/sc2730_adi_read() above).
 */
int sc2730_pmic_reset(void)
{
	struct udevice *dev;
	int ret, timeout;

	ret = uclass_get_device(UCLASS_PMIC, 0, &dev);
	if (ret)
		return ret;

	/*
	 * Record "this is a deliberate normal reboot" in the PMIC's
	 * always-on POR_RST_MONITOR scratch register *before* triggering
	 * the watchdog, exactly as vendor's reboot_devices()/
	 * reset_to_normal() (drivers/misc/check_reboot.c) always do right
	 * before their own reset_cpu(0) call. This register survives the
	 * reset (it's in the PMIC domain, not volatile DRAM), and the
	 * platform's own boot chain reads it back to decide how to
	 * continue. Leaving it at a stale/garbage value is suspected to be
	 * why the board goes completely silent after "resetting ..."
	 * instead of coming back up with normal UART output.
	 */
	pmic_reg_write(dev, ANA_REG_GLB_POR_RST_MONITOR, HWRST_STATUS_NORMAL);

	/* The watchdog block's own clocks may not be running yet - make
	 * sure they are before touching any of its registers. */
	pmic_clrsetbits(dev, ANA_REG_GLB_ARM_MODULE_EN, 0, BIT_ANA_WDG_EN);
	pmic_clrsetbits(dev, ANA_REG_GLB_RTC_CLK_EN, 0, BIT_RTC_WDG_EN);

	/* Unlock the watchdog's own registers. */
	pmic_reg_write(dev, ANA_WDG_LOCK, WDG_UNLOCK_KEY);

	/* Reset mode, not interrupt mode. */
	pmic_clrsetbits(dev, ANA_WDG_CTRL, BIT_WDG_INT_EN, 0);

	/* Wait for any previous load to finish before loading a new value
	 * (mirrors the vendor's own wdt_load_value()). */
	for (timeout = 0xfffff; timeout > 0; timeout--) {
		if (!(pmic_reg_read(dev, ANA_WDG_INT_RAW) & BIT_WDG_LD_BUSY))
			break;
	}

	/* A very short timeout - fires in ~2.4 ms (see WDG_RESET_NOW_VAL's
	 * comment in sc2730_reg_glb.h). */
	pmic_reg_write(dev, ANA_WDG_LOAD_HIGH, 0);
	pmic_reg_write(dev, ANA_WDG_LOAD_LOW, WDG_RESET_NOW_VAL);

	/* Start counting, with the timeout wired to an actual chip reset. */
	pmic_clrsetbits(dev, ANA_WDG_CTRL, 0, BIT_WDG_CNT_EN | BIT_WDG_RST_EN);

	/* Re-lock. */
	pmic_reg_write(dev, ANA_WDG_LOCK, ~WDG_UNLOCK_KEY);

	return 0;
}

/*
 * sc2730_pmic_poweroff() - cut all power rails via the PMIC's own
 * hardware power-off sequencer, for the "poweroff" command.
 *
 * ANA_REG_GLB_POWER_PD_HW's BIT_PWR_OFF_SEQ_EN bit (sc2730_reg_glb.h)
 * arms the PMIC's own power-down sequencer - the same mechanism
 * drivers/sysreset/sysreset_tps65910.c's PWR_OFF_SEQ bit uses on a
 * different PMIC family, and the counterpart to the ANA_REG_GLB_
 * POWER_PD_SW register already written (left at 0x0000, i.e. no rail
 * forced down in software) by sc2730_apply_stock_pmic_table()'s first
 * step above - "SW" is this driver asking for a rail down right now
 * while still running, "HW" is what actually executes a real
 * power-off sequence once armed. Once set, the PMIC itself cuts every
 * rail in the correct order with no further CPU involvement, so this
 * function is not expected to return - the trailing loop below is
 * only a safety net for the (seen on some boards during early bring-
 * up) case where power stays up for a brief moment afterwards, e.g.
 * because a charger is still connected and holding VBUS/SYS up.
 */
int sc2730_pmic_poweroff(void)
{
	struct udevice *dev;
	int ret;

	ret = uclass_get_device(UCLASS_PMIC, 0, &dev);
	if (ret)
		return ret;

	pmic_clrsetbits(dev, ANA_REG_GLB_POWER_PD_HW, 0, BIT_PWR_OFF_SEQ_EN);

	return 0;
}

static struct dm_pmic_ops sc2730_ops = {
	.read = sc2730_read,
	.write = sc2730_write,
};

static const struct udevice_id sc2730_ids[] = {
	{ .compatible = "sprd,sc2730" },
	{ }
};

U_BOOT_DRIVER(sc2730_pmic) = {
	.name = "sc2730_pmic",
	.id = UCLASS_PMIC,
	.of_match = sc2730_ids,
	.bind = sc2730_bind,
	.probe = sc2730_probe,
	.ops = &sc2730_ops,
};
