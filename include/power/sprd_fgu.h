/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * SC2730 fuel gauge unit (FGU) - minimal battery-voltage readout.
 *
 * See drivers/power/sprd_fgu.c's header comment for what this is and
 * isn't (a plain instantaneous ADC voltage read, not the vendor's full
 * coulomb-counter/state-of-charge machinery).
 */

#ifndef __SPRD_FGU_H
#define __SPRD_FGU_H

struct udevice;

/*
 * sprd_fgu_init() - power up the FGU block and load its ADC
 * calibration.
 *
 * @pmic: the already-probed sc2730 PMIC device (drivers/power/pmic/
 *        pmic_sc2730.c) whose ADI bus this block is reached through.
 *
 * Returns 0 on success, or a negative error code. On a calibration
 * read failure this still returns 0 and leaves the generic, not-this-
 * chip-specific calibration constants in place (sprd_fgu_read_vbat_mv()
 * keeps working, just less precisely) - matching the vendor's own
 * fail-open behavior.
 */
int sprd_fgu_init(struct udevice *pmic);

/*
 * sprd_fgu_read_vbat_mv() - read the current battery voltage.
 *
 * @pmic: the same device passed to sprd_fgu_init().
 *
 * Returns the battery voltage in millivolts, or a negative error code.
 * Must be called after a successful sprd_fgu_init().
 */
int sprd_fgu_read_vbat_mv(struct udevice *pmic);

#endif /* __SPRD_FGU_H */
