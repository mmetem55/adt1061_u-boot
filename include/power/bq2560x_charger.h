/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __BQ2560X_CHARGER_H
#define __BQ2560X_CHARGER_H

struct udevice;

/*
 * Toggle the charger's own CHG_CONFIG enable bit (REG01) after probe.
 * bq2560x_probe() already turns charging on once at init time; this
 * lets a caller apply its own protection logic on top of the chip's
 * CC/CV regulation loop - e.g. a software over-voltage safety net, or
 * the overnight charge-only hold loop's full/recharge hysteresis.
 */
int bq2560x_set_charge_enable(struct udevice *dev, int enable);

/*
 * Pet the chip's own I2C watchdog (REG01 WDT_RESET). Per the BQ2560x
 * family datasheet, if this isn't done periodically (POR default
 * timeout is on the order of tens of seconds) the chip reverts
 * REG00-REG06 to their power-on defaults - not dangerous by itself
 * (CHG_CONFIG's own reset default still leaves charging enabled), but
 * it would silently undo this driver's calibrated charge-current/
 * charge-voltage settings. Called periodically by the board's
 * background charge-monitor cyclic task so charging keeps running
 * correctly-tuned for as long as U-Boot sits idle at its prompt, and
 * for as much of the handoff to the kernel as U-Boot's own cyclic
 * framework still gets to run.
 */
int bq2560x_feed_watchdog(struct udevice *dev);

#endif /* __BQ2560X_CHARGER_H */
