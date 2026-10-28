/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Compat shim for the vendor adt1061_u-boot 2015.07 tree's "flat
 * global GPIO number" calling convention, used throughout
 * panel-if/panel_ops_mipi.c (sprd_gpio_request(NULL, gpio), etc. - a
 * *different*, older API shape than the DM_GPIO driver ported in
 * drivers/gpio/sprd_gpio.c, which these calls predate).
 *
 * This board has exactly one GPIO controller (the ported "ap_gpio",
 * compatible "sprd,pike2-gpio", covering all 256 on-chip GPIO
 * numbers), and it is the first (and only) GPIO uclass device bound,
 * so the DM_GPIO uclass assigns it gpio_base == 0. That means the
 * vendor's raw GPIO numbers (as used verbatim in the board's
 * /lcd-panel DT node, e.g. "sprd,lcd-reset-gpio = <50>") already line
 * up 1:1 with U-Boot's own global/flat GPIO numbering, so the plain
 * (non-DM) <asm-generic/gpio.h> API - gpio_request()/
 * gpio_direction_output()/gpio_direction_input()/gpio_get_value(),
 * which resolve a flat number to the right DM_GPIO device internally
 * - can stand in directly. The unused first argument in every vendor
 * call site (always a literal NULL "struct gpio_chip *") is simply
 * ignored here.
 */

#ifndef _SPRD_GPIO_COMPAT_H_
#define _SPRD_GPIO_COMPAT_H_

#include <asm-generic/gpio.h>

static inline int sprd_gpio_request(void *chip, unsigned int gpio)
{
	(void)chip;
	return gpio_request(gpio, "sprd-panel");
}

static inline int sprd_gpio_direction_output(void *chip, unsigned int gpio,
					      int value)
{
	(void)chip;
	return gpio_direction_output(gpio, value);
}

static inline int sprd_gpio_direction_input(void *chip, unsigned int gpio)
{
	(void)chip;
	return gpio_direction_input(gpio);
}

static inline int sprd_gpio_get(void *chip, unsigned int gpio)
{
	(void)chip;
	return gpio_get_value(gpio);
}

#endif /* _SPRD_GPIO_COMPAT_H_ */
