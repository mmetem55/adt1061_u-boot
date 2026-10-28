// SPDX-License-Identifier: GPL-2.0+
/*
 * Unisoc/Spreadtrum "pike2" on-chip GPIO controller.
 *
 * Ported nearly verbatim from the vendor adt1061_u-boot 2015.07 tree's
 * drivers/gpio/sprd_gpio.c - that file was already a modern DM_GPIO
 * driver (struct dm_gpio_ops, UCLASS_GPIO), so only the DM API renames
 * that happened since 2015.07 were needed: dev->priv = ... is now
 * dev_set_priv(), and the matching read site uses dev_get_priv()
 * instead of dev->priv directly. __raw_readl/__raw_writel are replaced
 * with plain readl/writel (the "__raw" variants were dropped upstream).
 *
 * This is a plain bank/bit register block (DATA/DMSK/DIR/INEN per
 * 16-pin bank), needed by the ported SPRD LCD panel driver
 * (drivers/video/sprd/panel-if/panel_ops_mipi.c) for panel reset/power
 * GPIO control - it did not exist in upstream U-Boot before this.
 */

#include <clk.h>
#include <dm.h>
#include <log.h>
#include <malloc.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <asm-generic/gpio.h>
#include <dt-bindings/gpio/gpio.h>
#include <dm/device-internal.h>

/* GPIO registers definition */
#define SPRD_GPIO_DATA		0x0
#define SPRD_GPIO_DMSK		0x4
#define SPRD_GPIO_DIR		0x8
#define SPRD_GPIO_INEN		0x28

#define SPRD_GPIO_NR		256
#define SPRD_GPIO_BANK_NR	16
#define SPRD_GPIO_BANK_SIZE	0x80
#define SPRD_GPIO_BIT(x)	((x) & (SPRD_GPIO_BANK_NR - 1))

static inline void __iomem *sprd_gpio_bank_base(struct udevice *dev,
						 u32 bank)
{
	return (void __iomem *)dev_get_priv(dev) + SPRD_GPIO_BANK_SIZE * bank;
}

static void sprd_gpio_update(struct udevice *dev,
			      u32 offset, u16 reg, int value)
{
	void __iomem *base = sprd_gpio_bank_base(dev, offset / SPRD_GPIO_BANK_NR);
	u32 tmp;

	tmp = readl(base + reg);

	if (value)
		tmp |= BIT(SPRD_GPIO_BIT(offset));
	else
		tmp &= ~BIT(SPRD_GPIO_BIT(offset));

	writel(tmp, base + reg);
}

static int sprd_gpio_read(struct udevice *dev, u32 offset, u16 reg)
{
	void __iomem *base = sprd_gpio_bank_base(dev, offset / SPRD_GPIO_BANK_NR);

	return !!(readl(base + reg) & BIT(SPRD_GPIO_BIT(offset)));
}

static int sprd_gpio_request(struct udevice *dev, u32 offset, const char *label)
{
	sprd_gpio_update(dev, offset, SPRD_GPIO_DMSK, 1);

	return 0;
}

static int sprd_gpio_free(struct udevice *dev, u32 offset)
{
	sprd_gpio_update(dev, offset, SPRD_GPIO_DMSK, 0);

	return 0;
}

static int sprd_gpio_direction_input(struct udevice *dev, u32 offset)
{
	sprd_gpio_update(dev, offset, SPRD_GPIO_DIR, 0);
	sprd_gpio_update(dev, offset, SPRD_GPIO_INEN, 1);

	return 0;
}

static int sprd_gpio_direction_output(struct udevice *dev, u32 offset,
				       int value)
{
	sprd_gpio_update(dev, offset, SPRD_GPIO_DIR, 1);
	sprd_gpio_update(dev, offset, SPRD_GPIO_INEN, 0);
	sprd_gpio_update(dev, offset, SPRD_GPIO_DATA, !!value);

	return 0;
}

static int sprd_gpio_get_value(struct udevice *dev, u32 offset)
{
	return sprd_gpio_read(dev, offset, SPRD_GPIO_DATA);
}

static int sprd_gpio_set_value(struct udevice *dev, u32 offset,
				int value)
{
	sprd_gpio_update(dev, offset, SPRD_GPIO_DATA, !!value);

	return 0;
}

static const struct dm_gpio_ops sprd_gpio_ops = {
	.request		= sprd_gpio_request,
	.rfree			= sprd_gpio_free,
	.direction_input	= sprd_gpio_direction_input,
	.direction_output	= sprd_gpio_direction_output,
	.get_value		= sprd_gpio_get_value,
	.set_value		= sprd_gpio_set_value,
};

static int sprd_gpio_probe(struct udevice *dev)
{
	struct gpio_dev_priv *uc_priv = dev_get_uclass_priv(dev);
	struct clk clk;
	int ret;

	/* Tell the uclass how many GPIOs we have */
	uc_priv->gpio_count = SPRD_GPIO_NR;
	uc_priv->bank_name = dev_read_string(dev, "gpio-bank-name");

	dev_set_priv(dev, dev_read_addr_ptr(dev));

	/*
	 * Confirmed on real hardware: this register block's own bus clock
	 * gate (CLK_GPIO_EB, in the "aon-gate" clock domain - see
	 * drivers/clk/sprd/sharkl5pro-clk.c's sharkl5pro_aon_gate_desc,
	 * bound to the devicetree's "aonapb_gate" label) isn't already
	 * running by the time U-Boot gets control, unlike e.g. the pin
	 * controller pin_init() touches, which lives on a different,
	 * already-enabled bus. Accessing any ap_gpio register before this
	 * clock is on takes a synchronous external abort (ESR data-fault
	 * code 0x10, "synchronous external abort, not on a translation
	 * table walk") straight from the AHB/APB bus - not an MMU fault,
	 * the whole low 2 GiB MMIO window is already mapped. Enable it
	 * here, the same way drivers/i2c/sprd_i2c.c enables its own
	 * "enable" clock, rather than depending on whatever happened to be
	 * left on by the ROM/FDL preloader.
	 */
	ret = clk_get_by_name(dev, "enable", &clk);
	if (!ret)
		clk_enable(&clk);

	return 0;
}

static const struct udevice_id sprd_gpio_ids[] = {
	{ .compatible = "sprd,pike2-gpio" },
	{ }
};

U_BOOT_DRIVER(gpio_sprd) = {
	.name	= "gpio_sprd",
	.id	= UCLASS_GPIO,
	.of_match = sprd_gpio_ids,
	.probe	= sprd_gpio_probe,
	.ops	= &sprd_gpio_ops,
};
