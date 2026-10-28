// SPDX-License-Identifier: GPL-2.0+
/*
 * SC2730 vibrator - minimal on/off control.
 *
 * Ported from (the SC2730 branch of) the vendor U-Boot 2015.07 tree's
 * drivers/misc/vibrator.c. That driver also has a non-SC2730 branch
 * (older PMICs needing CUR_DRV_CAL_SEL_U and an RTC-domain clock
 * enable) and is meant to be driven by a PWM-based intensity/waveform
 * framework elsewhere in the vendor tree - neither is ported, since
 * this board's PMIC is SC2730-only and all this needs to do is turn
 * the motor fully on or fully off for a fixed short pulse (see
 * sprd_vibrator_pulse()), as haptic feedback for the board's
 * low-battery boot gate (board/spreadtrum/ums512_1h10/ums512_1h10.c)
 * when nothing may be watching the UART or the (deliberately still
 * off, to save power) screen.
 *
 * Access pattern: like sprd_fgu.c, this reaches the vibrator's ADI-
 * slave register (sc2730_reg_glb.h's ANA_REG_GLB_VIBR_CTRL0 - already
 * defined there, no new register block needed) through the already-
 * probed sc2730 PMIC device's generic pmic_reg_write()/
 * pmic_clrsetbits() DM_PMIC ops, instead of the vendor's raw
 * ANA_REG_OR()/ANA_REG_AND()/ANA_REG_MSK_OR() macros.
 */

#include <dm.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <power/pmic.h>
#include <power/sprd_vibrator.h>

#include "pmic/sc2730_reg_glb.h"

/*
 * Vendor's literal VDDVIB LDO output-voltage trim value (written as
 * both value and mask into ANA_REG_GLB_VIBR_CTRL0's low byte - see
 * vibrator_hw_init()'s CONFIG_ADIE_SC2730 branch). Kept verbatim,
 * same as this port's other ported magic constants (e.g.
 * bq2560x_charger.c's default charge-current registers).
 */
#define LDO_VIBR_V			0xB4

#define VIBRATOR_PULSE_ON_MS		150
#define VIBRATOR_PULSE_GAP_MS		150

int sprd_vibrator_init(struct udevice *pmic)
{
	return pmic_clrsetbits(pmic, ANA_REG_GLB_VIBR_CTRL0, LDO_VIBR_V,
			       LDO_VIBR_V);
}

int sprd_vibrator_set(struct udevice *pmic, int on)
{
	if (on)
		return pmic_clrsetbits(pmic, ANA_REG_GLB_VIBR_CTRL0,
				       BIT_LDO_VDDVIB_PD |
				       BIT_SLP_LDO_VDDVIB_PD_EN, 0);

	return pmic_clrsetbits(pmic, ANA_REG_GLB_VIBR_CTRL0, 0,
			       BIT_LDO_VDDVIB_PD |
			       BIT_SLP_LDO_VDDVIB_PD_EN);
}

void sprd_vibrator_pulse(struct udevice *pmic, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		sprd_vibrator_set(pmic, 1);
		mdelay(VIBRATOR_PULSE_ON_MS);
		sprd_vibrator_set(pmic, 0);
		if (i + 1 < count)
			mdelay(VIBRATOR_PULSE_GAP_MS);
	}
}
