// SPDX-License-Identifier: GPL-2.0+
/*
 * TI BQ2560x battery charger - minimal U-Boot init driver.
 *
 * Ported from (the relevant parts of) the vendor adt1061_u-boot
 * 2015.07 tree's drivers/power/battery/bq2560x.c. Upstream U-Boot has
 * no charger/battery uclass or charging state machine at all
 * (confirmed: no UCLASS_CHARGER/UCLASS_BATTERY, no existing
 * bq2560x/bq25890 driver - only a kernel DT-binding doc gets synced
 * into dts/upstream/Bindings/), so despite the name matching a
 * TI-family part, this was not something to "just enable": it needed
 * porting.
 *
 * The vendor driver wires into a bespoke "sprdchg" charge-management
 * framework (timer callbacks, chipram_env download-mode checks, its
 * own ops-registration table) that is specific to that vendor tree
 * and isn't ported here, since it has no equivalent upstream and
 * isn't needed for basic boot-time behavior. What's ported instead is
 * the part that matters for U-Boot's purposes: on probe, soft-reset
 * the chip and enable charging with the input-current-limit/
 * charge-current the vendor's chg_bq2560x_init() +
 * chg_bq2560x_enable_chg() used for the normal (non-download-mode)
 * boot path. The vendor's register map, offsets and encodings
 * (REG00/REG01/REG02/REG0B) are kept verbatim.
 *
 * DT binding: bound as a plain I2C-client child (reg = <0x6b>, the
 * BQ2560x's fixed slave address) of the i2c bus the vendor's own
 * arch/arm/dts/adt1061.dts says it's on (sprd,bq2560x-i2c-bus = <4>
 * -> i2c4) - this is simpler and more idiomatic than the vendor's own
 * "read a bus-number property, then open a raw i2c chip at runtime"
 * approach, since upstream's DM_I2C uclass (i2c_child_post_bind() in
 * drivers/i2c/i2c-uclass.c) already populates a child device's
 * dm_i2c_chip plat data from its "reg" property automatically,
 * letting dm_i2c_reg_read()/dm_i2c_reg_write() be used directly on
 * this device with no manual bus/address lookup.
 *
 * Calibration (REG02 ICHG, REG03 IPRECHG/ITERM, REG04 VREG/VRECHG):
 * chosen for this board's actual battery - confirmed by the user to
 * be two 3.8V/3000mAh/11.4Wh cells behind one shared BMS circuit,
 * whose output is the single pack connection to the motherboard, for
 * a combined 6000mAh/22.8Wh pack. Two cells behind one BMS driving a
 * single downstream connection are in parallel (a series pair would
 * present ~7.6V nominal to the BMS output, not 3.8V) - electrically
 * the charger still sees one "cell" at the standard single Li-ion
 * 3.0-4.2V-class voltage range, just at double the capacity. Nothing
 * here indicates a high-voltage (4.35/4.4V-class) cell, so this
 * targets the standard, conservative 4.2V ceiling - the same target
 * the vendor's own sprd-battery.dtsi uses for this SoC family's
 * reference battery profile (chg-end-vol = 4200, chg-bat-safety-vol
 * = 4280) and very close to this chip's own POR default regulation
 * voltage (~4.208V): REG04_VREG_DEFAULT_MV below is the nearest value
 * this register's 32mV steps can actually express to that 4200mV
 * target, so this is a precise, explicit version of what the chip
 * would do anyway on its own, not a guess. Voltage thresholds scale
 * with cell chemistry, not capacity, so they're unaffected by the
 * pack actually being 6000mAh rather than 3000mAh.
 *
 * Current *does* scale with capacity. 2400mA (0.4C for 6000mAh) is
 * the charge rate, deliberately short of the textbook 0.5C (3000mA)
 * that would otherwise apply, because 3000mA is also this chip's
 * absolute register maximum (REG02_ICHG_MAX) - sitting right at a
 * small charger IC's documented ceiling for hours every charge cycle
 * leaves no thermal/tolerance margin, so 0.4C keeps a deliberate
 * buffer below it while still being a reasonably quick charge.
 * 360mA precharge/termination current (~0.06C of 6000mAh, the same
 * fraction as before, scaled up) is a conservative, by-the-book
 * choice for both "this cell is too flat to fast-charge yet" and
 * "this cell is done charging" detection.
 */

#include <dm.h>
#include <i2c.h>
#include <log.h>
#include <linux/bitops.h>
#include <dm/device_compat.h>
#include <power/bq2560x_charger.h>

/* Register 00h */
#define BQ2560X_REG_00			0x00
#define REG00_IINLIM_MASK		0x1f
#define REG00_IINLIM_SHIFT		0
#define REG00_IINLIM_BASE		100
#define REG00_IINLIM_CURRENT_MAX	3200
#define REG00_IINLIM_CURRENT_OFFSET	100
#define REG00_IINLIM_DEFAULT_MA		500

/* Register 01h */
#define BQ2560X_REG_01			0x01
#define REG01_CHG_CONFIG_MASK		BIT(4)
#define REG01_CHG_CONFIG_SHIFT		4
#define REG01_CHG_ENABLE		1

#define REG01_WDT_RESET_MASK		BIT(6)
#define REG01_WDT_RESET_SHIFT		6
#define REG01_WDT_RESET			1

/* Register 02h */
#define BQ2560X_REG_02			0x02
#define REG02_ICHG_MASK			0x3f
#define REG02_ICHG_SHIFT		0
#define REG02_ICHG_LSB			60
#define REG02_ICHG_MAX			3000
/* 0.4C for this board's 6000mAh pack - see file header comment. */
#define REG02_ICHG_DEFAULT_MA		2400

/* Register 03h: precharge current (bits 7:4) / termination current (bits 3:0) */
#define BQ2560X_REG_03			0x03
#define REG03_IPRECHG_MASK		0xf0
#define REG03_IPRECHG_SHIFT		4
#define REG03_IPRECHG_BASE		60
#define REG03_IPRECHG_LSB		60
#define REG03_IPRECHG_MAX		780
#define REG03_IPRECHG_DEFAULT_MA	360

#define REG03_ITERM_MASK		0x0f
#define REG03_ITERM_SHIFT		0
#define REG03_ITERM_BASE		60
#define REG03_ITERM_LSB		60
#define REG03_ITERM_MAX			960
#define REG03_ITERM_DEFAULT_MA		360

/* Register 04h: charge voltage regulation limit (bits 7:3) / recharge offset (bit 0) */
#define BQ2560X_REG_04			0x04
#define REG04_VREG_MASK			0xf8
#define REG04_VREG_SHIFT		3
#define REG04_VREG_BASE_MV		3856
#define REG04_VREG_LSB_MV		32
/* Nearest value this register's 32mV grid can express to the 4200mV
 * target - see file header comment.
 */
#define REG04_VREG_DEFAULT_MV		4208

#define REG04_VRECHG_MASK		BIT(0)
#define REG04_VRECHG_SHIFT		0
#define REG04_VRECHG_100MV		0
#define REG04_VRECHG_200MV		1

/* Register 0Bh */
#define BQ2560X_REG_0B			0x0b
#define REG0B_REG_RESET_MASK		BIT(7)
#define REG0B_REG_RESET_SHIFT		7
#define REG0B_REG_RESET			1

static int bq2560x_read_reg(struct udevice *dev, u8 reg, u8 *val)
{
	int ret;

	ret = dm_i2c_reg_read(dev, reg);
	if (ret < 0)
		return ret;

	*val = (u8)ret;

	return 0;
}

static int bq2560x_set_value(struct udevice *dev, u8 reg, u8 mask, u8 shift,
			      u8 val)
{
	u8 tmp;
	int ret;

	ret = bq2560x_read_reg(dev, reg, &tmp);
	if (ret)
		return ret;

	tmp = (tmp & ~mask) | ((val << shift) & mask);

	return dm_i2c_reg_write(dev, reg, tmp);
}

static int bq2560x_set_chg_cur(struct udevice *dev, u32 cur_ma)
{
	u8 reg_value;

	if (cur_ma > REG02_ICHG_MAX)
		cur_ma = REG02_ICHG_MAX;

	reg_value = cur_ma / REG02_ICHG_LSB;

	return bq2560x_set_value(dev, BQ2560X_REG_02, REG02_ICHG_MASK,
				  REG02_ICHG_SHIFT, reg_value);
}

static int bq2560x_set_limit_cur(struct udevice *dev, u32 limit_ma)
{
	u8 reg_value;

	if (limit_ma > REG00_IINLIM_CURRENT_MAX)
		limit_ma = REG00_IINLIM_CURRENT_MAX;

	limit_ma -= REG00_IINLIM_CURRENT_OFFSET;
	reg_value = limit_ma / REG00_IINLIM_BASE;

	return bq2560x_set_value(dev, BQ2560X_REG_00, REG00_IINLIM_MASK,
				  REG00_IINLIM_SHIFT, reg_value);
}

static int bq2560x_set_prechg_cur(struct udevice *dev, u32 cur_ma)
{
	u8 reg_value;

	if (cur_ma > REG03_IPRECHG_MAX)
		cur_ma = REG03_IPRECHG_MAX;
	if (cur_ma < REG03_IPRECHG_BASE)
		cur_ma = REG03_IPRECHG_BASE;

	reg_value = (cur_ma - REG03_IPRECHG_BASE) / REG03_IPRECHG_LSB;

	return bq2560x_set_value(dev, BQ2560X_REG_03, REG03_IPRECHG_MASK,
				  REG03_IPRECHG_SHIFT, reg_value);
}

static int bq2560x_set_term_cur(struct udevice *dev, u32 cur_ma)
{
	u8 reg_value;

	if (cur_ma > REG03_ITERM_MAX)
		cur_ma = REG03_ITERM_MAX;
	if (cur_ma < REG03_ITERM_BASE)
		cur_ma = REG03_ITERM_BASE;

	reg_value = (cur_ma - REG03_ITERM_BASE) / REG03_ITERM_LSB;

	return bq2560x_set_value(dev, BQ2560X_REG_03, REG03_ITERM_MASK,
				  REG03_ITERM_SHIFT, reg_value);
}

static int bq2560x_set_chg_volt(struct udevice *dev, u32 volt_mv)
{
	u8 reg_value;

	if (volt_mv < REG04_VREG_BASE_MV)
		volt_mv = REG04_VREG_BASE_MV;

	reg_value = (volt_mv - REG04_VREG_BASE_MV) / REG04_VREG_LSB_MV;

	return bq2560x_set_value(dev, BQ2560X_REG_04, REG04_VREG_MASK,
				  REG04_VREG_SHIFT, reg_value);
}

static int bq2560x_set_rechg_offset(struct udevice *dev, u8 offset)
{
	return bq2560x_set_value(dev, BQ2560X_REG_04, REG04_VRECHG_MASK,
				  REG04_VRECHG_SHIFT, offset);
}

int bq2560x_set_charge_enable(struct udevice *dev, int enable)
{
	return bq2560x_set_value(dev, BQ2560X_REG_01, REG01_CHG_CONFIG_MASK,
				  REG01_CHG_CONFIG_SHIFT,
				  enable ? REG01_CHG_ENABLE : 0);
}

int bq2560x_feed_watchdog(struct udevice *dev)
{
	return bq2560x_set_value(dev, BQ2560X_REG_01, REG01_WDT_RESET_MASK,
				  REG01_WDT_RESET_SHIFT, REG01_WDT_RESET);
}

static int bq2560x_reset(struct udevice *dev)
{
	return bq2560x_set_value(dev, BQ2560X_REG_0B, REG0B_REG_RESET_MASK,
				  REG0B_REG_RESET_SHIFT, REG0B_REG_RESET);
}

static int bq2560x_enable_chg(struct udevice *dev)
{
	return bq2560x_set_value(dev, BQ2560X_REG_01, REG01_CHG_CONFIG_MASK,
				  REG01_CHG_CONFIG_SHIFT, REG01_CHG_ENABLE);
}

static int bq2560x_probe(struct udevice *dev)
{
	u32 chg_cur_ma, limit_cur_ma, prechg_cur_ma, term_cur_ma, chg_volt_mv;
	int ret;

	ret = bq2560x_reset(dev);
	if (ret) {
		dev_err(dev, "bq2560x reset failed: %d\n", ret);
		return ret;
	}

	chg_cur_ma = dev_read_u32_default(dev, "sprd,bq2560x-chg-cur-ma",
					   REG02_ICHG_DEFAULT_MA);
	limit_cur_ma = dev_read_u32_default(dev, "sprd,bq2560x-limit-cur-ma",
					     REG00_IINLIM_DEFAULT_MA);
	prechg_cur_ma = dev_read_u32_default(dev, "sprd,bq2560x-prechg-cur-ma",
					      REG03_IPRECHG_DEFAULT_MA);
	term_cur_ma = dev_read_u32_default(dev, "sprd,bq2560x-term-cur-ma",
					    REG03_ITERM_DEFAULT_MA);
	chg_volt_mv = dev_read_u32_default(dev, "sprd,bq2560x-chg-volt-mv",
					    REG04_VREG_DEFAULT_MV);

	ret = bq2560x_set_chg_cur(dev, chg_cur_ma);
	if (ret)
		dev_warn(dev, "failed to set charge current: %d\n", ret);

	ret = bq2560x_set_limit_cur(dev, limit_cur_ma);
	if (ret)
		dev_warn(dev, "failed to set input current limit: %d\n", ret);

	ret = bq2560x_set_prechg_cur(dev, prechg_cur_ma);
	if (ret)
		dev_warn(dev, "failed to set precharge current: %d\n", ret);

	ret = bq2560x_set_term_cur(dev, term_cur_ma);
	if (ret)
		dev_warn(dev, "failed to set termination current: %d\n", ret);

	ret = bq2560x_set_chg_volt(dev, chg_volt_mv);
	if (ret)
		dev_warn(dev, "failed to set charge voltage: %d\n", ret);

	/*
	 * Tighter 100mV recharge offset (vs. the alternative 200mV) -
	 * keeps this pack topped up more aggressively once full, which
	 * is what "full ve öyle kalsın" (stay topped up) calls for.
	 */
	ret = bq2560x_set_rechg_offset(dev, REG04_VRECHG_100MV);
	if (ret)
		dev_warn(dev, "failed to set recharge offset: %d\n", ret);

	ret = bq2560x_enable_chg(dev);
	if (ret) {
		dev_err(dev, "failed to enable charging: %d\n", ret);
		return ret;
	}

	dev_info(dev,
		 "bq2560x charger enabled (ichg=%umA, ilim=%umA, iprechg=%umA, iterm=%umA, vreg=%umV)\n",
		 chg_cur_ma, limit_cur_ma, prechg_cur_ma, term_cur_ma,
		 chg_volt_mv);

	return 0;
}

static const struct udevice_id bq2560x_ids[] = {
	{ .compatible = "ti,bq2560x" },
	{ .compatible = "sprd,bq2560x-charger" },
	{ }
};

U_BOOT_DRIVER(bq2560x_charger) = {
	.name		= "bq2560x_charger",
	.id		= UCLASS_MISC,
	.of_match	= bq2560x_ids,
	.probe		= bq2560x_probe,
};
