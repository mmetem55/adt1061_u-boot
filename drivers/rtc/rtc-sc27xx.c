// SPDX-License-Identifier: GPL-2.0+
/*
 * RTC driver for the Unisoc/Spreadtrum SC27xx-family PMICs (SC2730 on
 * this board).
 *
 * Ported from the Unisoc/Aidata vendor U-Boot 2015.07 tree's
 * drivers/rtc/rtc-sc27xx.c, modernized against current upstream APIs:
 *  - rtc_ops here only has .get/.set/.reset (no .poweroff_alarm - that
 *    was a vendor-only addition to the ops struct, not part of
 *    upstream's uclass; the power-off-alarm wake path isn't ported).
 *  - broken-out <-> seconds-since-epoch conversion now goes through
 *    upstream's own rtc_to_tm()/rtc_mktime() (lib/date.c) instead of
 *    the vendor's hand-rolled leap-year arithmetic - same semantics,
 *    less code to carry.
 *
 * The devicetree node this binds to (rtc@200, compatible
 * "sprd,sc27xx-rtc") was already present verbatim in the ported
 * arch/arm/dts/sc2730.dtsi (copied over together with the rest of the
 * PMIC's child nodes before any RTC driver existed to bind it), and
 * drivers/power/pmic/pmic_sc2730.c's pmic_children_info[] already has
 * the {"rtc", "sprd-rtc"} entry wired up - this file is the missing
 * other half.
 */

#include <dm.h>
#include <log.h>
#include <dm/device_compat.h>
#include <rtc.h>
#include <power/pmic.h>
#include <linux/bitops.h>
#include <linux/delay.h>

/* Register offsets, relative to this node's "reg" (0x200). */
#define SPRD_RTC_SEC_CNT_VAL		0x00
#define SPRD_RTC_MIN_CNT_VAL		0x04
#define SPRD_RTC_HOUR_CNT_VAL		0x08
#define SPRD_RTC_DAY_CNT_VAL		0x0c
#define SPRD_RTC_SEC_CNT_UPD		0x10
#define SPRD_RTC_MIN_CNT_UPD		0x14
#define SPRD_RTC_HOUR_CNT_UPD		0x18
#define SPRD_RTC_DAY_CNT_UPD		0x1c
#define SPRD_RTC_SEC_ALM_UPD		0x20
#define SPRD_RTC_MIN_ALM_UPD		0x24
#define SPRD_RTC_HOUR_ALM_UPD		0x28
#define SPRD_RTC_DAY_ALM_UPD		0x2c
#define SPRD_RTC_INT_EN			0x30
#define SPRD_RTC_INT_RAW_STS		0x34
#define SPRD_RTC_INT_CLR		0x38
#define SPRD_RTC_SEC_ALM_VAL		0x40
#define SPRD_RTC_MIN_ALM_VAL		0x44
#define SPRD_RTC_HOUR_ALM_VAL		0x48
#define SPRD_RTC_DAY_ALM_VAL		0x4c
#define SPRD_RTC_SEC_AUX_UPD		0x60
#define SPRD_RTC_MIN_AUX_UPD		0x64
#define SPRD_RTC_HOUR_AUX_UPD		0x68
#define SPRD_RTC_DAY_AUX_UPD		0x6c

#define SPRD_RTC_SEC_UPD_EN		BIT(8)
#define SPRD_RTC_MIN_UPD_EN		BIT(9)
#define SPRD_RTC_HOUR_UPD_EN		BIT(10)
#define SPRD_RTC_DAY_UPD_EN		BIT(11)
#define SPRD_RTC_ALMSEC_UPD_EN		BIT(12)
#define SPRD_RTC_ALMMIN_UPD_EN		BIT(13)
#define SPRD_RTC_ALMHOUR_UPD_EN	BIT(14)
#define SPRD_RTC_ALMDAY_UPD_EN		BIT(15)

#define SPRD_RTC_TIME_UPD_MASK (SPRD_RTC_SEC_UPD_EN | SPRD_RTC_MIN_UPD_EN | \
				 SPRD_RTC_HOUR_UPD_EN | SPRD_RTC_DAY_UPD_EN)
#define SPRD_RTC_ALM_UPD_MASK (SPRD_RTC_ALMSEC_UPD_EN | SPRD_RTC_ALMMIN_UPD_EN | \
				SPRD_RTC_ALMHOUR_UPD_EN | SPRD_RTC_ALMDAY_UPD_EN)
#define SPRD_RTC_INT_ALL_MSK		0xffff

#define SPRD_RTC_SEC_VAL_MASK		0x3f
#define SPRD_RTC_MIN_VAL_MASK		0x3f
#define SPRD_RTC_HOUR_VAL_MASK		0x1f
#define SPRD_RTC_DAY_VAL_MASK		0xffff

enum sprd_rtc_type {
	SPRD_RTC_TIME,
	SPRD_RTC_ALARM,
	SPRD_RTC_AUX_ALARM,
};

struct sprd_rtc {
	u32 rtc_base;
};

static int sprd_rtc_reg_get(struct udevice *dev, u32 offset, u32 *value)
{
	struct sprd_rtc *priv = dev_get_priv(dev);
	struct udevice *pmic_dev = dev_get_parent(dev);
	int ret;

	ret = pmic_read(pmic_dev, priv->rtc_base + offset, (uint8_t *)value,
			 sizeof(u32));
	if (ret)
		dev_err(dev, "rtc reg 0x%x read failed (%d)\n", offset, ret);

	return ret;
}

static int sprd_rtc_reg_set(struct udevice *dev, u32 offset, u32 value)
{
	struct sprd_rtc *priv = dev_get_priv(dev);
	struct udevice *pmic_dev = dev_get_parent(dev);
	int ret;

	ret = pmic_write(pmic_dev, priv->rtc_base + offset, (uint8_t *)&value,
			  sizeof(u32));
	if (ret)
		dev_err(dev, "rtc reg 0x%x write failed (%d)\n", offset, ret);

	return ret;
}

static int sprd_rtc_get_secs(struct udevice *dev, enum sprd_rtc_type type,
			      u32 *rtc_secs)
{
	u32 sec_reg, min_reg, hour_reg, day_reg;
	u32 sec, min, hour, day;
	int ret;

	switch (type) {
	case SPRD_RTC_TIME:
		sec_reg = SPRD_RTC_SEC_CNT_VAL;
		min_reg = SPRD_RTC_MIN_CNT_VAL;
		hour_reg = SPRD_RTC_HOUR_CNT_VAL;
		day_reg = SPRD_RTC_DAY_CNT_VAL;
		break;
	case SPRD_RTC_ALARM:
		sec_reg = SPRD_RTC_SEC_ALM_VAL;
		min_reg = SPRD_RTC_MIN_ALM_VAL;
		hour_reg = SPRD_RTC_HOUR_ALM_VAL;
		day_reg = SPRD_RTC_DAY_ALM_VAL;
		break;
	case SPRD_RTC_AUX_ALARM:
		sec_reg = SPRD_RTC_SEC_AUX_UPD;
		min_reg = SPRD_RTC_MIN_AUX_UPD;
		hour_reg = SPRD_RTC_HOUR_AUX_UPD;
		day_reg = SPRD_RTC_DAY_AUX_UPD;
		break;
	default:
		return -EINVAL;
	}

	ret = sprd_rtc_reg_get(dev, sec_reg, &sec);
	if (ret)
		return ret;
	sec &= SPRD_RTC_SEC_VAL_MASK;

	ret = sprd_rtc_reg_get(dev, min_reg, &min);
	if (ret)
		return ret;
	min &= SPRD_RTC_MIN_VAL_MASK;

	ret = sprd_rtc_reg_get(dev, hour_reg, &hour);
	if (ret)
		return ret;
	hour &= SPRD_RTC_HOUR_VAL_MASK;

	ret = sprd_rtc_reg_get(dev, day_reg, &day);
	if (ret)
		return ret;
	day &= SPRD_RTC_DAY_VAL_MASK;

	*rtc_secs = ((((day * 24) + hour) * 60 + min) * 60 + sec);

	return 0;
}

static int sprd_rtc_set_secs(struct udevice *dev, enum sprd_rtc_type type,
			      u32 secs)
{
	u32 sec_reg, min_reg, hour_reg, day_reg;
	u32 sec, min, hour, day, temp;
	u32 set_mask = 0, int_sts;
	int timeout_cnt = 10;
	int ret;

	sec = secs % 60;
	temp = (secs - sec) / 60;
	min = temp % 60;
	temp = (temp - min) / 60;
	hour = temp % 24;
	day = (temp - hour) / 24;

	switch (type) {
	case SPRD_RTC_TIME:
		sec_reg = SPRD_RTC_SEC_CNT_UPD;
		min_reg = SPRD_RTC_MIN_CNT_UPD;
		hour_reg = SPRD_RTC_HOUR_CNT_UPD;
		day_reg = SPRD_RTC_DAY_CNT_UPD;
		set_mask = SPRD_RTC_TIME_UPD_MASK;
		sprd_rtc_reg_set(dev, SPRD_RTC_INT_CLR, SPRD_RTC_TIME_UPD_MASK);
		break;
	case SPRD_RTC_ALARM:
		sec_reg = SPRD_RTC_SEC_ALM_UPD;
		min_reg = SPRD_RTC_MIN_ALM_UPD;
		hour_reg = SPRD_RTC_HOUR_ALM_UPD;
		day_reg = SPRD_RTC_DAY_ALM_UPD;
		set_mask = SPRD_RTC_ALM_UPD_MASK;
		sprd_rtc_reg_set(dev, SPRD_RTC_INT_CLR, SPRD_RTC_ALM_UPD_MASK);
		break;
	case SPRD_RTC_AUX_ALARM:
		sec_reg = SPRD_RTC_SEC_AUX_UPD;
		min_reg = SPRD_RTC_MIN_AUX_UPD;
		hour_reg = SPRD_RTC_HOUR_AUX_UPD;
		day_reg = SPRD_RTC_DAY_AUX_UPD;
		break;
	default:
		return -EINVAL;
	}

	ret = sprd_rtc_reg_set(dev, sec_reg, sec);
	if (ret)
		return ret;
	ret = sprd_rtc_reg_set(dev, min_reg, min);
	if (ret)
		return ret;
	ret = sprd_rtc_reg_set(dev, hour_reg, hour);
	if (ret)
		return ret;
	ret = sprd_rtc_reg_set(dev, day_reg, day);
	if (ret)
		return ret;

	if (type == SPRD_RTC_AUX_ALARM)
		return 0;

	/* Wait for all four field updates to latch before clearing them. */
	do {
		ret = sprd_rtc_reg_get(dev, SPRD_RTC_INT_RAW_STS, &int_sts);
		if (ret)
			return ret;

		if ((int_sts & set_mask) == set_mask)
			return sprd_rtc_reg_set(dev, SPRD_RTC_INT_CLR, set_mask);

		udelay(20000);
	} while (--timeout_cnt);

	return -ETIMEDOUT;
}

static int sprd_rtc_get(struct udevice *dev, struct rtc_time *time)
{
	u32 secs;
	int ret;

	ret = sprd_rtc_get_secs(dev, SPRD_RTC_TIME, &secs);
	if (ret)
		return ret;

	rtc_to_tm(secs, time);

	return 0;
}

static int sprd_rtc_set(struct udevice *dev, const struct rtc_time *time)
{
	time64_t secs = rtc_mktime(time);

	if (secs < 0)
		return -EINVAL;

	return sprd_rtc_set_secs(dev, SPRD_RTC_TIME, (u32)secs);
}

static int sprd_rtc_reset(struct udevice *dev)
{
	sprd_rtc_reg_set(dev, SPRD_RTC_INT_EN, 0);

	sprd_rtc_set_secs(dev, SPRD_RTC_TIME, 0);
	sprd_rtc_set_secs(dev, SPRD_RTC_ALARM, 0);
	sprd_rtc_set_secs(dev, SPRD_RTC_AUX_ALARM, 0);

	return sprd_rtc_reg_set(dev, SPRD_RTC_INT_CLR, SPRD_RTC_INT_ALL_MSK);
}

static int sprd_rtc_probe(struct udevice *dev)
{
	struct sprd_rtc *priv = dev_get_priv(dev);

	priv->rtc_base = (u32)dev_read_addr(dev);
	if (priv->rtc_base == (u32)FDT_ADDR_T_NONE)
		return -EINVAL;

	return 0;
}

static const struct rtc_ops sprd_rtc_ops = {
	.get = sprd_rtc_get,
	.set = sprd_rtc_set,
	.reset = sprd_rtc_reset,
};

static const struct udevice_id sprd_rtc_ids[] = {
	{ .compatible = "sprd,sc27xx-rtc" },
	{ }
};

U_BOOT_DRIVER(sprd_rtc) = {
	.name = "sprd-rtc",
	.id = UCLASS_RTC,
	.of_match = sprd_rtc_ids,
	.ops = &sprd_rtc_ops,
	.probe = sprd_rtc_probe,
	.priv_auto = sizeof(struct sprd_rtc),
};
