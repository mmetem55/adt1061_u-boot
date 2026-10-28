// SPDX-License-Identifier: GPL-2.0+
/*
 * SC2730 fuel gauge unit (FGU) - minimal battery-voltage readout for
 * U-Boot's own purposes.
 *
 * Ported from (the relevant slice of) the vendor U-Boot 2015.07 tree's
 * drivers/power/battery/sprd_fgu.c. That driver is the front end of a
 * full state-of-charge/coulomb-counter subsystem (OCV/POCV tracking,
 * current integration, battery-capacity persistence across reboots in
 * the FGU's "user area" registers, first-power-on-with-charger-
 * attached handling, ...) built for the vendor's own "sprdbat" battery
 * framework, which has no equivalent here and isn't ported. What *is*
 * ported is the one thing this tree actually needs: a real, chip-
 * calibrated instantaneous battery voltage reading
 * (sprdfgu_read_vbat_vol()'s functionality), used by the board code's
 * low-battery boot gate (see board/spreadtrum/ums512_1h10/
 * ums512_1h10.c's board_late_init()) to decide whether to hold off
 * booting and let the bq2560x charger (already enabled by
 * bq2560x_probe() by the time this runs) bring the battery up past a
 * safe voltage first.
 *
 * Access pattern: like pmic_sc2730.c and wdt-sc27xx.c, this reaches
 * the FGU/efuse ADI-slave sub-blocks (see sc2730_reg_glb.h's header
 * comments for where their addresses come from) through the already-
 * probed sc2730 PMIC device's generic pmic_reg_read()/pmic_reg_write()/
 * pmic_clrsetbits() DM_PMIC ops, instead of the vendor's raw
 * sci_adi_read()/sci_adi_write() calls - there is no separate FGU or
 * efuse uclass/device here, just plain functions operating on that one
 * PMIC device, exactly like the vendor's own non-DM sprdfgu_*()
 * functions operate on no device at all (global ADI register access).
 */

#include <dm.h>
#include <log.h>
#include <dm/device_compat.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <power/pmic.h>
#include <power/sprd_fgu.h>

#include "pmic/sc2730_reg_glb.h"

/*
 * Efuse calibration block holding this chip's one-point (4.2V) FGU ADC
 * trim - confirmed against the vendor's drivers/power/battery/
 * sprd_fgu.c's sprdfgu_cal_get() CONFIG_ADIE_SC2730 branch:
 * sprd_pmic_efuse_read_bits(BITSINDEX(3, 0, 16), 9), i.e. block 3,
 * the low 9 bits.
 */
#define SPRD_FGU_CAL_EFUSE_BLK		3
#define SPRD_FGU_CAL_BIT_LEN		9

/*
 * Vendor's own fallback two-point calibration constants (the compiled-
 * in defaults sprd_fgu.c's fgu_nv_4200mv/fgu_nv_3600mv globals start
 * at, and keep if efuse calibration isn't available). Used here the
 * same way: if the efuse read fails, fall back to these instead of
 * refusing to report a voltage at all.
 */
#define SPRD_FGU_FALLBACK_4200MV	2872
#define SPRD_FGU_FALLBACK_3600MV	2374

struct sprd_fgu_cal {
	int vol_1000mv_adc;
	int vol_offset;
};

static struct sprd_fgu_cal fgu_cal;

/*
 * A plain pmic_reg_read() of ANA_FGU_VOLT_VAL (and, less often, of
 * ANA_FGU_INT_RAW) can come back as a single transient ADI bus error
 * (sprd_adi_read()'s readback-address-mismatch check, drivers/spi/
 * sprd_adi.c) - confirmed on real hardware: "sprd_adi ...: read error,
 * reg addr = 0x32120c20 ...", immediately aborting the low-battery
 * boot gate in board code even though a battery was genuinely at
 * 3.3V. Every *other* FGU register (INT_EN/INT_CLR/CURT_OFFSET, and
 * INT_RAW on every other poll) reads back fine around it, so this
 * isn't a wrong address or an unclocked block - VOLT_VAL is the one
 * register here that a live, continuously-running ADC sampler can be
 * updating in the middle of an ADI read, which is exactly the kind of
 * thing that can occasionally desync that bus protocol's read/
 * address-echo handshake for one transaction. The vendor's own
 * sprdfgu_reg_get() already retries on *unstable values*; this retries
 * on an outright bus *error* the same way, instead of treating a
 * single flaky transaction as fatal.
 */
static int sprd_fgu_reg_read_retry(struct udevice *pmic, uint reg, int tries)
{
	int ret;

	while (tries-- > 0) {
		ret = pmic_reg_read(pmic, reg);
		if (ret >= 0)
			return ret;
		udelay(100);
	}

	return ret;
}

static int sc2730_efuse_wait(struct udevice *pmic, uint reg, u32 bit)
{
	int ret, retry = 2000;

	/*
	 * SC2730's efuse STATUS/MODE_CTRL "busy"/"done" bits are active-
	 * high-means-ready (confirmed against the vendor's
	 * pmic_efuse_wait_clear()'s CONFIG_ADIE_SC2730 branch, which
	 * computes bit_status as (~val) & bits and loops while that's
	 * nonzero - i.e. waits for the bit to read back as *set*, the
	 * opposite of what the "wait_clear" name suggests). 2000 retries
	 * * 2us is a generous 4ms bound - real hardware settles in well
	 * under 1ms for either of these.
	 */
	while (retry--) {
		ret = pmic_reg_read(pmic, reg);
		if (ret < 0)
			return ret;
		if ((u32)ret & bit)
			return 0;
		udelay(2);
	}

	return -ETIMEDOUT;
}

static int sc2730_efuse_read_block(struct udevice *pmic, int blk_index,
				    u32 *val)
{
	int ret;

	ret = pmic_clrsetbits(pmic, ANA_REG_GLB_ARM_MODULE_EN, 0,
			      BIT_ANA_EFS_EN);
	if (ret)
		return ret;

	ret = pmic_clrsetbits(pmic, ANA_REG_EFUSE_MODE_CTRL, 0,
			      BIT_EFUSE_NORMAL_RD_FLAG_CLR);
	if (ret)
		goto out;

	ret = sc2730_efuse_wait(pmic, ANA_REG_EFUSE_STATUS,
				BIT_EFUSE_STANDBY_BUSY);
	if (ret)
		goto out;

	ret = pmic_reg_write(pmic, ANA_REG_EFUSE_BLOCK_INDEX,
			     BITS_EFUSE_BLOCK_INDEX(blk_index));
	if (ret)
		goto out;

	ret = pmic_clrsetbits(pmic, ANA_REG_EFUSE_MODE_CTRL, 0,
			      BIT_EFUSE_RD_START);
	if (ret)
		goto out;

	ret = sc2730_efuse_wait(pmic, ANA_REG_EFUSE_STATUS,
				BIT_EFUSE_NORMAL_RD_DONE);
	if (ret)
		goto out;

	ret = pmic_reg_read(pmic, ANA_REG_EFUSE_DATA_RD);
	if (ret < 0)
		goto out;
	*val = (u32)ret;

	ret = pmic_clrsetbits(pmic, ANA_REG_EFUSE_MODE_CTRL, 0,
			      BIT_EFUSE_NORMAL_RD_FLAG_CLR);

out:
	pmic_clrsetbits(pmic, ANA_REG_GLB_ARM_MODULE_EN, BIT_ANA_EFS_EN, 0);
	return ret;
}

static void sprd_fgu_cal_init(struct udevice *pmic)
{
	u32 raw = 0;
	int fgu_nv_4200mv = SPRD_FGU_FALLBACK_4200MV;
	int fgu_nv_3600mv = SPRD_FGU_FALLBACK_3600MV;
	int ret;

	ret = sc2730_efuse_read_block(pmic, SPRD_FGU_CAL_EFUSE_BLK, &raw);
	if (!ret) {
		raw &= BIT(SPRD_FGU_CAL_BIT_LEN) - 1;
		/* sprdfgu_cal_get()'s one-point conversion, verbatim. */
		fgu_nv_4200mv = (int)raw + 6963 - 4096 - 256;
		fgu_nv_3600mv = 0;
		dev_info(pmic, "fgu: using chip efuse calibration (4200mv_adc=%d)\n",
			 fgu_nv_4200mv);
	} else {
		dev_warn(pmic, "fgu: efuse calibration read failed (%d), using generic defaults\n",
			 ret);
	}

	/* sprdfgu_cal_init(), verbatim. */
	if (fgu_nv_3600mv == 0) {
		fgu_cal.vol_1000mv_adc =
			DIV_ROUND_CLOSEST(fgu_nv_4200mv * 10, 42);
		fgu_cal.vol_offset = 0;
	} else {
		fgu_cal.vol_1000mv_adc =
			DIV_ROUND_CLOSEST((fgu_nv_4200mv - fgu_nv_3600mv) * 10, 6);
		fgu_cal.vol_offset =
			0 - (fgu_nv_4200mv * 10 - fgu_cal.vol_1000mv_adc * 42) / 10;
	}
}

int sprd_fgu_init(struct udevice *pmic)
{
	int ret, retry = 1000;

	ret = pmic_clrsetbits(pmic, ANA_REG_GLB_ARM_MODULE_EN, 0,
			      BIT_ANA_FGU_EN);
	if (ret)
		return ret;

	ret = pmic_clrsetbits(pmic, ANA_REG_GLB_RTC_CLK_EN, 0,
			      BIT_RTC_FGU_EN);
	if (ret)
		return ret;

	/* Disable/clear any interrupt state left over from a watchdog
	 * reset, matching sprdfgu_init(). */
	pmic_clrsetbits(pmic, ANA_FGU_INT_EN, 0xffff, 0);
	pmic_clrsetbits(pmic, ANA_FGU_INT_CLR, 0, 0xffff);
	pmic_reg_write(pmic, ANA_FGU_CURT_OFFSET, 0);

	/* Wait for the first voltage conversion to be ready. */
	while (retry--) {
		ret = sprd_fgu_reg_read_retry(pmic, ANA_FGU_INT_RAW, 5);
		if (ret < 0)
			return ret;
		if ((u32)ret & BIT_VOL_READY_INT)
			break;
		mdelay(2);
	}
	if (retry < 0)
		dev_warn(pmic, "fgu: voltage not ready after 2s, reading anyway\n");

	sprd_fgu_cal_init(pmic);

	return 0;
}

int sprd_fgu_read_vbat_mv(struct udevice *pmic)
{
	int adc, prev, retry = 1000;

	/*
	 * sprdfgu_reg_get(): the ADC register can be mid-update when read,
	 * so read it back-to-back until it settles (or give up after a
	 * bounded number of tries and use the last value anyway).
	 */
	prev = sprd_fgu_reg_read_retry(pmic, ANA_FGU_VOLT_VAL, 10);
	if (prev < 0)
		return prev;

	while (retry--) {
		adc = sprd_fgu_reg_read_retry(pmic, ANA_FGU_VOLT_VAL, 10);
		if (adc < 0)
			return adc;
		if (adc == prev)
			break;
		prev = adc;
	}

	if (!fgu_cal.vol_1000mv_adc)
		return -EINVAL;

	return ((adc + fgu_cal.vol_offset) * 1000) / fgu_cal.vol_1000mv_adc;
}
