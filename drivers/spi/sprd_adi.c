// SPDX-License-Identifier: GPL-2.0+
/*
 * Unisoc (Spreadtrum) ADI bus driver.
 *
 * The ADI ("Anolog Die Interface") bus is Spreadtrum's proprietary
 * 2-wire bus used by the AP to talk to the PMIC (and other analog-die
 * peripherals) register space. It is modeled here as a UCLASS_SPI
 * controller, exactly as the vendor U-Boot 2015.07 tree's
 * drivers/spi/sprd_adi.c (Copyright (C) 2019 Spreadtrum Communications
 * Inc.) does; this port only modernizes it for the current DM API
 * (priv_auto, dev_read_addr_ptr) and fixes a latent bug in the
 * original's sprd_adi_check_addr(), which referenced an undeclared
 * "dev" identifier in its dev_err() call.
 */

#include <dm.h>
#include <log.h>
#include <spi.h>
#include <dm/device_compat.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/sizes.h>

/* registers definitions for controller CTL_ADI */
#define REG_ADI_CTRL0			0x4
#define REG_ADI_CHN_PRIL		0x8
#define REG_ADI_CHN_PRIH		0xc
#define REG_ADI_INT_EN			0x10
#define REG_ADI_INT_RAW			0x14
#define REG_ADI_INT_MASK		0x18
#define REG_ADI_INT_CLR			0x1c
#define REG_ADI_GSSI_CFG0		0x20
#define REG_ADI_GSSI_CFG1		0x24
#define REG_ADI_RD_CMD			0x28
#define REG_ADI_RD_DATA			0x2c
#define REG_ADI_ARM_FIFO_STS		0x30
#define REG_ADI_STS			0x34
#define REG_ADI_EVT_FIFO_STS		0x38
#define REG_ADI_ARM_CMD_STS		0x3c
#define REG_ADI_CHN_EN			0x40
#define REG_ADI_CHNL_ADDR(id)		(0x44 + ((id) - 2) * 4)
#define REG_ADI_CHN_EN1		0x20c

/* Bits definitions for register REG_ADI_GSSI_CFG0 */
#define BIT_CLK_ALL_ON			BIT(30)

/* Bits definitions for register REG_ADI_RD_DATA */
#define BIT_RD_CMD_BUSY			BIT(31)
#define RD_ADDR_SHIFT			16
#define RD_VALUE_MASK			GENMASK(15, 0)
#define RD_ADDR_MASK			GENMASK(30, 16)

/* Bits definitions for register REG_ADI_ARM_FIFO_STS */
#define BIT_FIFO_FULL			BIT(11)
#define BIT_FIFO_EMPTY			BIT(10)

#define ADI_HW_CHNS			50
#define ADI_15BIT_SLVAE_OFFSET		0x20000
#define ADI_12BIT_SLVAE_OFFSET		0x8000
#define ADI_15BIT_VALID_ADDR		0x1FFFC
#define ADI_12BIT_VALID_ADDR		0x3FFC

#define ADI_READ_TIMEOUT		2000
#define ADI_FIFO_DRAIN_TIMEOUT		1000
#define REG_ADDR_LOW_MASK		GENMASK(16, 0)
#define RDBACK_ADDR_OFFSET		2
#define ADI_SLAVE_ADDR_SIZE		SZ_8K

struct sprd_adi_data {
	u32 slave_offset;
	u32 valid_addr;
};

struct sprd_adi_priv {
	void __iomem *base;
	const struct sprd_adi_data *data;
};

static const struct sprd_adi_data sharkl3_data = {
	.slave_offset = ADI_12BIT_SLVAE_OFFSET,
	.valid_addr = ADI_12BIT_VALID_ADDR,
};

static const struct sprd_adi_data sharkl5pro_data = {
	.slave_offset = ADI_15BIT_SLVAE_OFFSET,
	.valid_addr = ADI_15BIT_VALID_ADDR,
};

static int sprd_adi_probe(struct udevice *bus)
{
	struct sprd_adi_priv *priv = dev_get_priv(bus);
	int i, size, chn_cnt;
	const fdt32_t *list;

	priv->base = dev_read_addr_ptr(bus);
	priv->data = (const struct sprd_adi_data *)dev_get_driver_data(bus);

	list = fdt_getprop(gd->fdt_blob, dev_of_offset(bus), "sprd,hw-channels",
			    &size);
	if (!list || !size) {
		dev_info(bus, "no hw channels setting in node\n");
		return 0;
	}

	chn_cnt = size / (2 * sizeof(*list));

	for (i = 0; i < chn_cnt; i++) {
		u32 value;
		u32 chn_id = fdt32_to_cpu(*list++);
		u32 chn_config = fdt32_to_cpu(*list++);

		if (chn_id < 2)
			continue;

		writel(chn_config, priv->base + REG_ADI_CHNL_ADDR(chn_id));

		if (chn_id < 32) {
			value = readl(priv->base + REG_ADI_CHN_EN);
			value |= BIT(chn_id);
			writel(value, priv->base + REG_ADI_CHN_EN);
		} else if (chn_id < ADI_HW_CHNS) {
			value = readl(priv->base + REG_ADI_CHN_EN1);
			value |= BIT(chn_id - 32);
			writel(value, priv->base + REG_ADI_CHN_EN1);
		}
	}

	return 0;
}

static inline int sprd_adi_drain_fifo(struct udevice *dev)
{
	struct sprd_adi_priv *priv = dev_get_priv(dev);
	u32 timeout = ADI_FIFO_DRAIN_TIMEOUT;
	u32 sts;

	do {
		sts = readl(priv->base + REG_ADI_ARM_FIFO_STS);
		if (sts & BIT_FIFO_EMPTY)
			break;
		udelay(1);
	} while (--timeout);

	if (timeout == 0) {
		dev_err(dev, "drain write fifo timeout\n");
		return -EBUSY;
	}

	return 0;
}

static int sprd_adi_fifo_is_full(struct sprd_adi_priv *priv)
{
	return readl(priv->base + REG_ADI_ARM_FIFO_STS) & BIT_FIFO_FULL;
}

static int sprd_adi_write(struct udevice *dev, u32 reg, u32 val)
{
	struct sprd_adi_priv *priv = dev_get_priv(dev);
	u32 timeout = ADI_FIFO_DRAIN_TIMEOUT;
	int ret;

	ret = sprd_adi_drain_fifo(dev);
	if (ret < 0)
		return ret;

	/*
	 * we should wait for write fifo is empty before writing data to PMIC
	 * registers.
	 */
	do {
		if (!sprd_adi_fifo_is_full(priv)) {
			writel(val, (void *)(uintptr_t)reg);
			break;
		}
		udelay(1);
	} while (--timeout);

	if (timeout == 0) {
		dev_err(dev, "write fifo is full\n");
		ret = -EBUSY;
	} else {
		ret = 0;
	}

	return ret;
}

static int sprd_adi_read(struct udevice *dev, u32 reg, u32 *read_val)
{
	struct sprd_adi_priv *priv = dev_get_priv(dev);
	int read_timeout = ADI_READ_TIMEOUT;
	u32 val, rd_addr;

	/*
	 * Set the register address need to read into RD_CMD register,
	 * then ADI controller will start to transfer automatically.
	 */
	writel(reg, priv->base + REG_ADI_RD_CMD);

	/*
	 * Wait read operation complete, the BIT_RD_CMD_BUSY will be set
	 * simultaneously when writing read command to register, and the
	 * BIT_RD_CMD_BUSY will be cleared after the read operation is
	 * completed.
	 */
	do {
		val = readl(priv->base + REG_ADI_RD_DATA);
		if (!(val & BIT_RD_CMD_BUSY))
			break;
		udelay(1);
	} while (--read_timeout);

	if (read_timeout == 0) {
		dev_err(dev, "ADI read timeout\n");
		return -EBUSY;
	}

	/*
	 * The return value includes data and read register address, from bit 0
	 * to bit 15 are data, and from bit 16 to bit 30 are read register
	 * address. Then we can check the returned register address to validate
	 * data.
	 */
	rd_addr = (val & RD_ADDR_MASK) >> RD_ADDR_SHIFT;

	if (rd_addr != (reg & REG_ADDR_LOW_MASK) >> RDBACK_ADDR_OFFSET) {
		dev_err(dev, "read error, reg addr = 0x%x, val = 0x%x\n",
			reg, val);
		return -EIO;
	}

	*read_val = val & RD_VALUE_MASK;

	return 0;
}

static int sprd_adi_check_addr(struct udevice *dev, u32 addr)
{
	struct sprd_adi_priv *priv = dev_get_priv(dev);
	uintptr_t base = (uintptr_t)priv->base;

	if (addr < base + priv->data->slave_offset ||
	    addr > base + priv->data->slave_offset + ADI_SLAVE_ADDR_SIZE) {
		dev_err(dev, "slave physical address is incorrect, addr = 0x%x\n",
			addr);
		return -EINVAL;
	}

	return 0;
}

/*
 * sprd_adi_xfer() interface:
 * @slave:	The slave device to communicate with.
 * @bitlen:	Number of bits to write/read.
 * @dout:	Pointer to a pmic address to write, followed by the value.
 * @din:	Pointer to a pmic address to read.
 * @flags:	Unused.
 *
 * Returns: 0 on success, negative errno on failure.
 */
static int sprd_adi_xfer(struct udevice *slave, unsigned int bitlen,
			  const void *dout, void *din, unsigned long flags)
{
	struct udevice *dev = slave->parent;
	struct sprd_adi_priv *priv = dev_get_priv(dev);
	u32 reg, val, msk;
	int ret;

	if (bitlen == 0 || bitlen > 16) {
		dev_err(dev, "ADI xfer bitlen is wrong!\n");
		return -EINVAL;
	}

	msk = GENMASK(bitlen - 1, 0);

	if (din) {
		reg = ((*(u32 *)din) & priv->data->valid_addr) +
			(uintptr_t)priv->base + priv->data->slave_offset;

		ret = sprd_adi_check_addr(dev, reg);
		if (ret)
			return ret;

		ret = sprd_adi_read(dev, reg, &val);
		if (ret)
			return ret;

		*(u32 *)din = val & msk;
	} else if (dout) {
		const u32 *p = dout;
		u32 read_back;

		reg = (p[0] & priv->data->valid_addr) +
			(uintptr_t)priv->base + priv->data->slave_offset;
		val = p[1];

		ret = sprd_adi_check_addr(dev, reg);
		if (ret)
			return ret;

		ret = sprd_adi_read(dev, reg, &read_back);
		if (ret)
			return ret;

		val = (val & msk) | (read_back & ~msk);

		ret = sprd_adi_write(dev, reg, val);
		if (ret)
			return ret;
	} else {
		dev_err(dev, "no buffer for transfer!\n");
		return -EINVAL;
	}

	return 0;
}

static int sprd_adi_claim_bus(struct udevice *slave)
{
	return 0;
}

static int sprd_adi_release_bus(struct udevice *slave)
{
	return 0;
}

static int sprd_adi_set_speed(struct udevice *bus, uint speed)
{
	return 0;
}

static int sprd_adi_set_mode(struct udevice *bus, uint mode)
{
	return 0;
}

static const struct dm_spi_ops sprd_adi_ops = {
	.claim_bus	= sprd_adi_claim_bus,
	.release_bus	= sprd_adi_release_bus,
	.xfer		= sprd_adi_xfer,
	.set_speed	= sprd_adi_set_speed,
	.set_mode	= sprd_adi_set_mode,
};

static const struct udevice_id sprd_adi_ids[] = {
	{ .compatible = "sprd,sharkl3-adi", .data = (ulong)&sharkl3_data },
	{ .compatible = "sprd,sharkl5pro-adi", .data = (ulong)&sharkl5pro_data },
	{ }
};

U_BOOT_DRIVER(sprd_adi) = {
	.name		= "sprd_adi",
	.id		= UCLASS_SPI,
	.of_match	= sprd_adi_ids,
	.ops		= &sprd_adi_ops,
	.priv_auto	= sizeof(struct sprd_adi_priv),
	.probe		= sprd_adi_probe,
};
