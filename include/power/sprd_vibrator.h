/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * SC2730 vibrator - minimal on/off control, for simple haptic feedback
 * with no display and no UART reader guaranteed to be watching.
 *
 * See drivers/power/sprd_vibrator.c's header comment for what this is
 * and isn't (plain LDO on/off, not the vendor's PWM-driven intensity/
 * waveform vibrator framework).
 */

#ifndef __SPRD_VIBRATOR_H
#define __SPRD_VIBRATOR_H

struct udevice;

/*
 * sprd_vibrator_init() - power up the vibrator LDO's voltage trim.
 *
 * @pmic: the already-probed sc2730 PMIC device (drivers/power/pmic/
 *        pmic_sc2730.c) whose ADI bus this is reached through.
 *
 * Returns 0 on success, or a negative error code. Must be called once
 * before the first sprd_vibrator_set().
 */
int sprd_vibrator_init(struct udevice *pmic);

/*
 * sprd_vibrator_set() - turn the vibrator motor on or off.
 *
 * @pmic: the same device passed to sprd_vibrator_init().
 * @on: nonzero to start vibrating, zero to stop.
 *
 * Returns 0 on success, or a negative error code.
 */
int sprd_vibrator_set(struct udevice *pmic, int on);

/*
 * sprd_vibrator_pulse() - blocking helper: buzz @count short pulses.
 *
 * @pmic: the same device passed to sprd_vibrator_init().
 * @count: number of short on/off pulses.
 *
 * Each pulse is a fixed short on-time followed by a fixed short gap;
 * this call blocks for roughly count * (on-time + gap). Errors from
 * the individual sprd_vibrator_set() calls are ignored (deliberately -
 * a failed buzz is not worth aborting whatever the caller is doing
 * for), so this always returns void.
 */
void sprd_vibrator_pulse(struct udevice *pmic, int count);

#endif /* __SPRD_VIBRATOR_H */
