// SPDX-License-Identifier: GPL-2.0+
/*
 * DM_WDT driver for the watchdog block built into the Unisoc/Spreadtrum
 * SC27xx-family PMICs (SC2730 on this board).
 *
 * Ported from the Unisoc/Aidata vendor U-Boot 2015.07 tree's
 * drivers/watchdog/sprd_wdt.c (register layout/semantics, lock-key
 * sequence, clock-gate enable bits) and include/sprd_wdt.h (register
 * offsets/bits), but accessed through the DM PMIC uclass's generic
 * pmic_read()/pmic_write() instead of vendor's raw ANA_REG_GET/SET
 * macros - same pattern as this tree's drivers/rtc/rtc-sc27xx.c.
 *
 * Like the RTC and PMIC EIC blocks, this watchdog lives in the PMIC's
 * own ADI register space, not on the AP side - a 32-bit timeout value
 * is split into two 16-bit LOAD_LOW/LOAD_HIGH registers because the
 * ADI bus itself only carries 16 bits of data per transaction (see
 * rtc-sc27xx.c's header comment for the same point about the RTC).
 */

#include <dm.h>
#include <log.h>
#include <dm/device_compat.h>
#include <wdt.h>
#include <power/pmic.h>
#include <linux/bitops.h>
#include <linux/delay.h>

/* Register offsets, relative to this node's "reg" (0x40). */
#define SPRD_WDT_LOAD_LOW	0x00
#define SPRD_WDT_LOAD_HIGH	0x04
#define SPRD_WDT_CTRL		0x08
#define SPRD_WDT_INT_CLR	0x0c
#define SPRD_WDT_INT_RAW	0x10
#define SPRD_WDT_INT_MSK	0x14
#define SPRD_WDT_CNT_LOW	0x18
#define SPRD_WDT_CNT_HIGH	0x1c
#define SPRD_WDT_LOCK		0x20

#define SPRD_WDT_INT_EN_BIT	BIT(0)
#define SPRD_WDT_CNT_EN_BIT	BIT(1)
#define SPRD_WDT_RST_EN_BIT	BIT(3)

#define SPRD_WDT_INT_RST_BIT	BIT(3)
#define SPRD_WDT_INT_CLEAR_BIT	BIT(0)
#define SPRD_WDT_LD_BUSY_BIT	BIT(4)

#define SPRD_WDT_UNLOCK_KEY	0xe551
#define SPRD_WDT_LOAD_TIMEOUT_LOOPS	0xfffff

/* 32768Hz clock feeding the counter - vendor's WDG_CLK. */
#define SPRD_WDT_CLK_HZ		32768

/*
 * Clock-gate registers for this watchdog live in the PMIC's global
 * (0x1800-based) register block, not under this node's own 0x40 base -
 * vendor's start_watchdog()/hw_watchdog_rst_pending() both set these
 * before touching the watchdog's own registers. See
 * drivers/power/pmic/sc2730_reg_glb.h's ANA_REG_GLB_ARM_MODULE_EN
 * (== ANA_REG_GLB_MODULE_EN0) / ANA_REG_GLB_RTC_CLK_EN
 * (== ANA_REG_GLB_RTC_CLK_EN0) and BIT_WDG_EN/BIT_RTC_WDG_EN (both
 * BIT(2)) - duplicated here as plain offsets rather than pulling in
 * that whole header, since only these two registers are needed.
 */
#define SPRD_WDT_GLB_MODULE_EN0		0x1808
#define SPRD_WDT_GLB_MODULE_EN0_WDG_EN	BIT(2)
#define SPRD_WDT_GLB_RTC_CLK_EN0	0x1810
#define SPRD_WDT_GLB_RTC_CLK_EN0_WDG_EN	BIT(2)

struct sprd_wdt_priv {
	u32 base;
	u32 timeout_ticks;
};

static int sprd_wdt_reg_get(struct udevice *dev, u32 offset, u32 *value)
{
	struct sprd_wdt_priv *priv = dev_get_priv(dev);
	struct udevice *pmic_dev = dev_get_parent(dev);
	int ret;

	ret = pmic_read(pmic_dev, priv->base + offset, (uint8_t *)value,
			 sizeof(u32));
	if (ret)
		dev_err(dev, "wdt reg 0x%x read failed (%d)\n", offset, ret);

	return ret;
}

static int sprd_wdt_reg_set(struct udevice *dev, u32 offset, u32 value)
{
	struct sprd_wdt_priv *priv = dev_get_priv(dev);
	struct udevice *pmic_dev = dev_get_parent(dev);
	int ret;

	ret = pmic_write(pmic_dev, priv->base + offset, (uint8_t *)&value,
			  sizeof(u32));
	if (ret)
		dev_err(dev, "wdt reg 0x%x write failed (%d)\n", offset, ret);

	return ret;
}

static int sprd_wdt_glb_set(struct udevice *dev, u32 glb_reg, u32 value)
{
	struct udevice *pmic_dev = dev_get_parent(dev);

	return pmic_write(pmic_dev, glb_reg, (uint8_t *)&value, sizeof(u32));
}

static int sprd_wdt_enable_clocks(struct udevice *dev)
{
	int ret;

	ret = sprd_wdt_glb_set(dev, SPRD_WDT_GLB_MODULE_EN0,
				SPRD_WDT_GLB_MODULE_EN0_WDG_EN);
	if (ret)
		return ret;

	return sprd_wdt_glb_set(dev, SPRD_WDT_GLB_RTC_CLK_EN0,
				 SPRD_WDT_GLB_RTC_CLK_EN0_WDG_EN);
}

static int sprd_wdt_load_value(struct udevice *dev, u32 value)
{
	u32 int_raw = 0;
	int timeout_cnt = SPRD_WDT_LOAD_TIMEOUT_LOOPS;
	int ret;

	do {
		ret = sprd_wdt_reg_get(dev, SPRD_WDT_INT_RAW, &int_raw);
		if (ret)
			return ret;
	} while ((int_raw & SPRD_WDT_LD_BUSY_BIT) && --timeout_cnt);

	ret = sprd_wdt_reg_set(dev, SPRD_WDT_LOAD_HIGH, (value >> 16) & 0xffff);
	if (ret)
		return ret;

	return sprd_wdt_reg_set(dev, SPRD_WDT_LOAD_LOW, value & 0xffff);
}

static int sprd_wdt_unlock(struct udevice *dev)
{
	return sprd_wdt_reg_set(dev, SPRD_WDT_LOCK, SPRD_WDT_UNLOCK_KEY);
}

static int sprd_wdt_lock(struct udevice *dev)
{
	return sprd_wdt_reg_set(dev, SPRD_WDT_LOCK, ~SPRD_WDT_UNLOCK_KEY);
}

static int sprd_wdt_start(struct udevice *dev, u64 timeout_ms, ulong flags)
{
	struct sprd_wdt_priv *priv = dev_get_priv(dev);
	/* vendor's start_watchdog()'s val formula, in 1/32768s ticks. */
	u32 value = (u32)((timeout_ms / 1000) * SPRD_WDT_CLK_HZ +
			   ((timeout_ms % 1000) * SPRD_WDT_CLK_HZ) / 1000);
	int ret;

	ret = sprd_wdt_enable_clocks(dev);
	if (ret)
		return ret;

	ret = sprd_wdt_unlock(dev);
	if (ret)
		return ret;

	/* Reset mode (not interrupt mode): clear WDT_INT_EN_BIT. */
	ret = sprd_wdt_reg_set(dev, SPRD_WDT_CTRL, 0);
	if (ret)
		goto out_lock;

	ret = sprd_wdt_load_value(dev, value);
	if (ret)
		goto out_lock;

	ret = sprd_wdt_reg_set(dev, SPRD_WDT_CTRL,
				SPRD_WDT_CNT_EN_BIT | SPRD_WDT_RST_EN_BIT);
	if (!ret)
		priv->timeout_ticks = value;

out_lock:
	sprd_wdt_lock(dev);

	return ret;
}

static int sprd_wdt_stop(struct udevice *dev)
{
	int ret;

	ret = sprd_wdt_unlock(dev);
	if (ret)
		return ret;

	ret = sprd_wdt_reg_set(dev, SPRD_WDT_INT_CLR,
				SPRD_WDT_INT_CLEAR_BIT | SPRD_WDT_INT_RST_BIT);
	if (ret)
		goto out_lock;

	ret = sprd_wdt_reg_set(dev, SPRD_WDT_CTRL, 0);

out_lock:
	sprd_wdt_lock(dev);

	return ret;
}

static int sprd_wdt_reset(struct udevice *dev)
{
	/*
	 * Re-arming via load_value() needs the lock open, same as
	 * start(); vendor's feed_watchdog() is actually a no-op (the
	 * counter free-runs down from the loaded value and never needs
	 * re-feeding until it's reloaded), so this is a reasonable
	 * strengthening: reload the counter back to its max so
	 * CONFIG_WATCHDOG's periodic wdt_reset() calls genuinely keep
	 * postponing expiry.
	 */
	struct sprd_wdt_priv *priv = dev_get_priv(dev);
	u32 ctrl;
	int ret;

	ret = sprd_wdt_reg_get(dev, SPRD_WDT_CTRL, &ctrl);
	if (ret)
		return ret;

	if (!(ctrl & SPRD_WDT_CNT_EN_BIT))
		return 0;

	ret = sprd_wdt_unlock(dev);
	if (ret)
		return ret;

	ret = sprd_wdt_load_value(dev, priv->timeout_ticks);

	sprd_wdt_lock(dev);

	return ret;
}

static int sprd_wdt_probe(struct udevice *dev)
{
	struct sprd_wdt_priv *priv = dev_get_priv(dev);

	priv->base = (u32)dev_read_addr(dev);
	if (priv->base == (u32)FDT_ADDR_T_NONE)
		return -EINVAL;

	return 0;
}

static const struct wdt_ops sprd_wdt_ops = {
	.start = sprd_wdt_start,
	.stop = sprd_wdt_stop,
	.reset = sprd_wdt_reset,
};

static const struct udevice_id sprd_wdt_ids[] = {
	{ .compatible = "sprd,sc2731-wdt" },
	{ }
};

U_BOOT_DRIVER(sprd_wdt) = {
	.name = "sprd-wdt",
	.id = UCLASS_WDT,
	.of_match = sprd_wdt_ids,
	.ops = &sprd_wdt_ops,
	.probe = sprd_wdt_probe,
	.priv_auto = sizeof(struct sprd_wdt_priv),
};
