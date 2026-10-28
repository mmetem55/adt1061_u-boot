// SPDX-License-Identifier: GPL-2.0+
/*
 * Unisoc/Spreadtrum UART driver (sc9836/sc9860/ums512/sharkl family).
 *
 * Ported from the vendor Unisoc/Aidata U-Boot 2015.07 tree
 * (drivers/serial/serial_dm_sprd.c) to mainline U-Boot's current
 * driver model API (of_to_plat/plat/plat_auto instead of the old
 * ofdata_to_platdata/platdata/platdata_auto_alloc_size names).
 *
 * The UART clock input is a fixed 26 MHz reference ("ext_26m" in the
 * device tree), handled by the generic fixed-rate clock driver, so no
 * SoC-specific clock driver is required for the console to work.
 */

#include <clk.h>
#include <dm.h>
#include <errno.h>
#include <serial.h>
#include <asm/io.h>
#include <linux/err.h>

#include "serial_sprd.h"

struct sprd_serial_plat {
	struct sprd_uart_regs __iomem *regs;
	ulong uart_clk_rate;
};

static int sprd_serial_setbrg(struct udevice *dev, int baudrate)
{
	struct sprd_serial_plat *plat = dev_get_plat(dev);

	if (!baudrate)
		return -EINVAL;

	writel(plat->uart_clk_rate / baudrate, &plat->regs->clkd0);

	return 0;
}

static int sprd_serial_getc(struct udevice *dev)
{
	struct sprd_serial_plat *plat = dev_get_plat(dev);

	if (!(readl(&plat->regs->status1) & SPRD_UART_STS1_RXF_CNT_MASK))
		return -EAGAIN;

	return readl(&plat->regs->rxd);
}

static int sprd_serial_putc(struct udevice *dev, const char ch)
{
	struct sprd_serial_plat *plat = dev_get_plat(dev);

	if ((readl(&plat->regs->status1) >> SPRD_UART_STS1_TXF_CNT_SHIFT) &
	    SPRD_UART_STS1_TXF_CNT_MASK)
		return -EAGAIN;

	writel(ch, &plat->regs->txd);

	return 0;
}

static int sprd_serial_pending(struct udevice *dev, bool input)
{
	struct sprd_serial_plat *plat = dev_get_plat(dev);
	u32 status1 = readl(&plat->regs->status1);

	if (input)
		return status1 & SPRD_UART_STS1_RXF_CNT_MASK;

	return (status1 >> SPRD_UART_STS1_TXF_CNT_SHIFT) &
		SPRD_UART_STS1_TXF_CNT_MASK;
}

static int sprd_serial_probe(struct udevice *dev)
{
	/*
	 * The UART pins and the "ext-26m" reference clock are already
	 * enabled by the Unisoc FDL1/FDL2 preloader (it uses this same
	 * UART for the download protocol), so there is nothing else to
	 * set up here besides the baud rate divisor.
	 */
	return sprd_serial_setbrg(dev, CONFIG_BAUDRATE);
}

static int sprd_serial_of_to_plat(struct udevice *dev)
{
	struct sprd_serial_plat *plat = dev_get_plat(dev);
	struct clk clk;
	int ret;

	plat->regs = dev_read_addr_ptr(dev);
	if (!plat->regs)
		return -EINVAL;

	ret = clk_get_by_index(dev, 0, &clk);
	if (ret) {
		/* No clock phandle in the DT: assume the documented 26 MHz */
		plat->uart_clk_rate = 26000000UL;
		return 0;
	}

	ret = clk_enable(&clk);
	if (ret && ret != -ENOSYS)
		return ret;

	plat->uart_clk_rate = clk_get_rate(&clk);
	if (IS_ERR_VALUE(plat->uart_clk_rate) || !plat->uart_clk_rate)
		plat->uart_clk_rate = 26000000UL;

	return 0;
}

static const struct dm_serial_ops sprd_serial_ops = {
	.putc = sprd_serial_putc,
	.pending = sprd_serial_pending,
	.getc = sprd_serial_getc,
	.setbrg = sprd_serial_setbrg,
};

static const struct udevice_id sprd_serial_ids[] = {
	{ .compatible = "sprd,ums512-uart" },
	{ .compatible = "sprd,sc9863a-uart" },
	{ .compatible = "sprd,sc9836-uart" },
	{ }
};

U_BOOT_DRIVER(sprd_serial) = {
	.name = "sprd-serial",
	.id = UCLASS_SERIAL,
	.of_match = sprd_serial_ids,
	.of_to_plat = sprd_serial_of_to_plat,
	.plat_auto = sizeof(struct sprd_serial_plat),
	.probe = sprd_serial_probe,
	.ops = &sprd_serial_ops,
	.flags = DM_FLAG_PRE_RELOC,
};
