// SPDX-License-Identifier: GPL-2.0+
/*
 * Board support for Unisoc UMS512 (sharkl5pro) boards, including the
 * Unisoc UMS512-1H10 reference board and the Aidata ADT1061 tablet.
 *
 * Ported from the Unisoc/Aidata vendor U-Boot 2015.07 tree
 * (board/spreadtrum/ums512_1h10/ums512_1h10.c) to mainline U-Boot.
 *
 * On this platform, DRAM is already trained and sized by the vendor
 * FDL1/FDL2 preloader before U-Boot proper is loaded, so we only need
 * to read the size back out of the control device tree's /memory node.
 */

#include <asm-generic/gpio.h>
#include <asm/io.h>
#include <bcb.h>
#include <blk.h>
#include <dm.h>
#include <dm/uclass-id.h>
#include <env.h>
#include <fastboot.h>
#include <fdtdec.h>
#include <gzip.h>
#include <init.h>
#include <log.h>
#include <malloc.h>
#include <memalign.h>
#include <part.h>
#include <cyclic.h>
#include <power/bq2560x_charger.h>
#include <power/pmic.h>
#include <power/regulator.h>
#include <power/sprd_fgu.h>
#include <power/sprd_vibrator.h>
#include <splash.h>
#include <video.h>
#include <video_console.h>
#include <wdt.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <u-boot/schedule.h>

#include "hwfeature.h"
#include "pinmap.h"

/*
 * AON_APB block's chip version-id register (CTL_AON_APB_BASE + 0xf8 in
 * the vendor tree's arch/arm/include/asm/arch-sharkl5pro/chip_sharkl5pro/
 * aon_apb.h - SCI_IOMAP() is an identity mapping on this SoC, and
 * CTL_AON_APB_BASE is the fixed physical/bus address 0x327d0000, so this
 * is a plain MMIO read, same as vendor's CHIP_REG_GET() macro did).
 */
#define UMS512_AON_APB_AON_VER_ID	0x327d00f8
#define UMS512_DDIE_ID_AA		0x00
#define UMS512_DDIE_ID_AB		0x01
#define UMS512_DDIE_ID_MASK		0xff

static int sprd_regulator_set_enable(const char *name, bool enable)
{
	struct udevice *dev;
	int ret;

	ret = regulator_get_by_platname(name, &dev);
	if (ret) {
		log_debug("regulator '%s' not found (%d)\n", name, ret);
		return ret;
	}

	return regulator_set_enable(dev, enable);
}

static int sprd_regulator_set_voltage_mv(const char *name, int mv)
{
	struct udevice *dev;
	int ret;

	ret = regulator_get_by_platname(name, &dev);
	if (ret) {
		log_debug("regulator '%s' not found (%d)\n", name, ret);
		return ret;
	}

	return regulator_set_value(dev, mv * 1000);
}

/*
 * Ported from vendor's board/spreadtrum/ums512_1h10/regulator_init.c.
 * vddsram's target voltage depends on the digital-die silicon revision;
 * vddrf1v25 is simply always tuned to 1245mV regardless of revision.
 */
static void sprd_regulator_voltage_init(void)
{
	u32 ddie_id = readl((void __iomem *)(ulong)UMS512_AON_APB_AON_VER_ID) &
		      UMS512_DDIE_ID_MASK;

	if (ddie_id == UMS512_DDIE_ID_AA)
		sprd_regulator_set_voltage_mv("vddsram", 1000);
	else if (ddie_id == UMS512_DDIE_ID_AB)
		sprd_regulator_set_voltage_mv("vddsram", 900);

	sprd_regulator_set_voltage_mv("vddrf1v25", 1245);
}

/*
 * Ported from vendor's board/spreadtrum/ums512_1h10/ldo_sleep.c's
 * dcdc_ldo_power_on(), CONFIG_ADIE_SC2730 branch only (this board's PMIC
 * is always SC2730): enables the handful of rails that are powered off
 * by SC2730's own reset default and that U-Boot needs up (RF/analog/
 * camera-DCXO-clock/general-purpose IO rails). The rest of vendor's
 * list (vddsram, vddmemq, vddmodem, vdd18dcxo, vddemm, vddgen0/1,
 * vddmem, vddcore, vddcpu, vddgpu, vdd18) is commented out in vendor's
 * own source too - SC2730 already powers those on by default - and the
 * CONFIG_DEBUG_DCDCLDO_FORCE_ON block (camera/SIM/WCN/USB rails forced
 * on purely for vendor bring-up debugging) is deliberately not ported.
 *
 * Name note: vendor's "vddrf1v8"/"vdddcxo" are this tree's devicetree
 * regulator-name "vddrf18"/"vdd18_dcxo" (arch/arm/dts/sc2730.dtsi) -
 * same rails, just named slightly differently when they were ported.
 */
static void sprd_dcdc_ldo_power_on(void)
{
	sprd_regulator_set_enable("vddrf18", true);
	sprd_regulator_set_enable("vddrf1v25", true);
	sprd_regulator_set_enable("avdd12", true);
	sprd_regulator_set_enable("avdd18", true);
	sprd_regulator_set_enable("vdd28", true);
	sprd_regulator_set_voltage_mv("vdd28", 2800);
	sprd_regulator_set_enable("vdd18_dcxo", true);
	sprd_regulator_set_enable("vddldo0", true);
}

/* sprd_bl.c - direct PWM-channel backlight control, see that file's header comment */
void set_backlight(u32 brightness);

/*
 * The vendor's own splash-screen subsystem (board/spreadtrum/ums512_1h10/
 * logo_display(), drivers/video/sprd/sprd_main.c's logo_mem_init()/
 * get_logo_bin_info()) reads a boot-time logo straight out of a raw "logo"
 * partition on the eMMC - either a plain .bmp (checked for a leading "BM"
 * signature) or, for OTA-style multi-resolution images, its own small
 * gzip-container format. Upstream U-Boot already has an equivalent,
 * idiomatic mechanism for exactly the plain-.bmp case: CONFIG_SPLASH_SOURCE
 * (common/splash_source.c) + CONFIG_CMD_BMP's bmp_display() - so rather
 * than porting the vendor's bespoke reader wholesale, this points that
 * existing mechanism at the same "logo" partition and only adds a small
 * pre-pass of our own in front of it for the vendor's gzip-container
 * case, which upstream's splash_load_raw() has no notion of (it always
 * assumes the first bytes are a BMP header, so pointed straight at a
 * gzip-container "logo" partition it would misparse the container's own
 * header fields as a bogus BMP file_size and fail outright).
 *
 * splash_screen_prepare() is declared __weak in common/splash.c for exactly
 * this kind of board override; the default implementation's
 * default_splash_locations[] only knows generic names like "mmc_raw"
 * pointing at the first partition (devpart "0:1"), so it has to be
 * replaced here with one that points at our actual "logo" partition by
 * name ("0#logo" - part_get_info_by_dev_and_name_or_num()'s
 * "<mmc dev>#<partition name>" syntax).
 */
static struct splash_location adt1061_splash_locations[] = {
	{
		.name = "logo",
		.storage = SPLASH_STORAGE_MMC,
		.flags = SPLASH_STORAGE_RAW,
		.devpart = "0#logo",
	},
};

/*
 * Vendor's include/logo_bin.h "struct header_info" + the gz_size[]
 * flexible-array tail that follows it in the partition (not expressible
 * in a single C struct without a zero-length array, which this port
 * avoids - see ADT1061_LOGO_HDR_SIZE below instead). Layout, verbatim
 * from vendor:
 *   char     signature[2];       // != "BM" is how vendor tells this
 *                                // container apart from a plain .bmp
 *   uint16_t file_number;        // number of packed gzip entries
 *   uint32_t reserved_num[5];
 *   uint32_t gz_size[file_number]; // compressed size of each entry,
 *                                  // immediately concatenated after
 *                                  // this header, in order
 */
struct adt1061_logo_hdr {
	char signature[2];
	u16 file_number;
	u32 reserved_num[5];
};

#define ADT1061_LOGO_HDR_SIZE		sizeof(struct adt1061_logo_hdr)
#define ADT1061_LOGO_MAX_ENTRIES	64	/* sanity cap, not a real limit */
#define ADT1061_LOGO_MAX_GZ_SIZE	SZ_16M	/* sanity cap, not a real limit */

/*
 * Vendor's include/logo_bin.h LOGO_NORMAL_POWER - the normal boot splash
 * entry. Vendor also packs LOGO_LOW_VOL (0) and LOGO_LOW_BATTERY_CHARGING
 * (1) variants for its fuel-gauge-driven low-battery/charging splash
 * screens, but this port has no fuel-gauge driver at all (see the
 * earlier vendor-vs-upstream gap analysis), so only the normal-boot
 * entry is ever selected here.
 */
#define ADT1061_LOGO_INDEX_NORMAL	3

/*
 * Read [offset, offset+size) raw bytes out of an MMC partition, taking
 * care of the fact that blk_dread() only works in whole-block units -
 * unlike vendor's own common_raw_read(), which could get away with a
 * flat memcpy() because its "partition" was just a fixed physical DRAM
 * offset, not real eMMC storage.
 */
static int adt1061_part_raw_read(struct blk_desc *desc,
				  const struct disk_partition *part,
				  u32 offset, void *buf, u32 size)
{
	lbaint_t blk = offset / part->blksz;
	u32 blk_off = offset % part->blksz;
	lbaint_t nblks = DIV_ROUND_UP(blk_off + size, part->blksz);
	void *tmp;

	tmp = malloc(nblks * part->blksz);
	if (!tmp)
		return -ENOMEM;

	if (blk_dread(desc, part->start + blk, nblks, tmp) != nblks) {
		free(tmp);
		return -EIO;
	}

	memcpy(buf, tmp + blk_off, size);
	free(tmp);

	return 0;
}

/*
 * Mirrors vendor's drivers/video/sprd/sprd_main.c's get_logo_bin_info(),
 * for the "else" (non-BMP, gzip-container) branch only: locate this
 * device's normal-boot entry in the container, gunzip it straight into
 * the splash buffer at bmp_load_addr, and let the caller's normal
 * bmp_display() handle the now-plain-BMP result exactly as if it had
 * been the uncompressed case all along.
 *
 * Returns 0 on success, -ENOENT if the partition does not look like one
 * of these containers at all (signature == "BM", i.e. a plain .bmp -
 * the caller should fall back to the normal splash_source_load() path),
 * or another negative error on a real failure.
 */
static int adt1061_splash_try_gzip_container(struct blk_desc *desc,
					      const struct disk_partition *part,
					      ulong bmp_load_addr,
					      size_t bmp_load_max)
{
	struct adt1061_logo_hdr hdr;
	u32 *gz_size;
	u32 header_size, offset, in_size;
	unsigned long inout_size;
	int logo_index, ret, i;
	void *gz_buf;

	ret = adt1061_part_raw_read(desc, part, 0, &hdr, ADT1061_LOGO_HDR_SIZE);
	if (ret)
		return ret;

	if (hdr.signature[0] == 'B' && hdr.signature[1] == 'M')
		return -ENOENT;

	if (!hdr.file_number || hdr.file_number > ADT1061_LOGO_MAX_ENTRIES) {
		log_err("logo: implausible gzip-container entry count %u\n",
			hdr.file_number);
		return -EINVAL;
	}

	header_size = (u32)hdr.file_number * sizeof(u32) + ADT1061_LOGO_HDR_SIZE;

	gz_size = malloc(hdr.file_number * sizeof(u32));
	if (!gz_size)
		return -ENOMEM;

	ret = adt1061_part_raw_read(desc, part, ADT1061_LOGO_HDR_SIZE, gz_size,
				     hdr.file_number * sizeof(u32));
	if (ret) {
		free(gz_size);
		return ret;
	}

	logo_index = ADT1061_LOGO_INDEX_NORMAL;
	if (logo_index >= hdr.file_number)
		logo_index = hdr.file_number - 1;

	offset = header_size;
	for (i = 0; i < logo_index; i++)
		offset += gz_size[i];
	in_size = gz_size[logo_index];
	free(gz_size);

	if (!in_size || in_size > ADT1061_LOGO_MAX_GZ_SIZE) {
		log_err("logo: implausible gzip entry size %u\n", in_size);
		return -EINVAL;
	}

	gz_buf = malloc(in_size);
	if (!gz_buf)
		return -ENOMEM;

	ret = adt1061_part_raw_read(desc, part, offset, gz_buf, in_size);
	if (ret) {
		free(gz_buf);
		return ret;
	}

	inout_size = bmp_load_max;
	ret = gunzip((void *)bmp_load_addr, inout_size, gz_buf, &inout_size);
	free(gz_buf);
	if (ret)
		log_err("logo: gunzip of gzip-container entry failed (%d)\n", ret);

	return ret;
}

int splash_screen_prepare(void)
{
	struct blk_desc *desc;
	struct disk_partition part;
	char *s;
	ulong bmp_load_addr;
	int ret;

	s = env_get("splashimage");
	if (!s)
		return -EINVAL;
	bmp_load_addr = hextoul(s, NULL);

	ret = part_get_info_by_dev_and_name_or_num(
		"mmc", adt1061_splash_locations[0].devpart, &desc, &part, 1);
	if (!ret) {
		ret = adt1061_splash_try_gzip_container(desc, &part,
							 bmp_load_addr,
							 SZ_16M);
		if (!ret)
			return 0;
		if (ret != -ENOENT)
			log_warning("logo: gzip-container load failed (%d), falling back\n",
				    ret);
	}

	return splash_source_load(adt1061_splash_locations,
				   ARRAY_SIZE(adt1061_splash_locations));
}

/*
 * Real per-unit serial number, read straight out of the "miscdata"
 * partition - the same place and format the vendor's own
 * common/loader/loader_common.c's get_product_sn() reads it from
 * (PRODUCTINFO_FILE_PATITION "miscdata", see that file and
 * include/loader_common.h's SP09_PHASE_CHECK_T/SP15_PHASE_CHECK_T).
 * This is a factory-provisioned record, not derived from any chip
 * efuse/hardware ID - real retail units (like the one this was tested
 * on) already have it written at manufacturing time, which is exactly
 * why plugging in the placeholder below was only ever a cosmetic stand-in.
 *
 * Only the leading magic + SN1 + SN2 fields are declared here, deliberately
 * leaving out every field that comes after them in the vendor's real
 * on-flash structs (StationNum, StationName[], ...). Doing it this way
 * sidesteps any assumption about how those later fields are
 * sized/aligned (e.g. the vendor struct's "unsigned long" fields, whose
 * width differs between the 32-bit toolchain the on-flash format
 * presumably predates and this port's 64-bit one) - everything we
 * actually need is packed contiguously (byte-for-byte, no padding freely
 * occurring between a leading u32 and the char arrays that directly
 * follow it) within the first 132 bytes regardless.
 */
#define ADT1061_MISCDATA_PART		"0#miscdata"
#define ADT1061_MISCDATA_SP09_MAGIC	0x53503039U	/* "SP09" */
#define ADT1061_MISCDATA_SP15_MAGIC	0x53503135U	/* "SP15" */

struct adt1061_miscdata_sn_sp09 {
	u32 magic;
	char sn1[24];
	char sn2[24];
};

struct adt1061_miscdata_sn_sp15 {
	u32 magic;
	char sn1[64];
	char sn2[64];
};

/*
 * Returns 0 and fills "buf" (nul-terminated, up to buflen - 1 bytes) with
 * the real serial number on success, or a negative error code (no
 * "miscdata" partition, empty/unprovisioned record, etc) on failure -
 * the caller falls back to the old hardcoded placeholder in that case.
 */
static int adt1061_read_serial(char *buf, size_t buflen)
{
	ALLOC_CACHE_ALIGN_BUFFER(u8, raw, 512);
	struct blk_desc *desc;
	struct disk_partition info;
	struct adt1061_miscdata_sn_sp09 *sp09 = (void *)raw;
	struct adt1061_miscdata_sn_sp15 *sp15 = (void *)raw;
	u32 magic;
	const char *sn;
	int ret;

	ret = part_get_info_by_dev_and_name_or_num("mmc", ADT1061_MISCDATA_PART,
						    &desc, &info, 1);
	if (ret < 0)
		return ret;

	if (blk_dread(desc, info.start, 1, raw) != 1)
		return -EIO;

	magic = *(u32 *)raw;
	if (magic == ADT1061_MISCDATA_SP15_MAGIC) {
		sn = sp15->sn2[0] ? sp15->sn2 : sp15->sn1;
	} else if (magic == ADT1061_MISCDATA_SP09_MAGIC) {
		sn = sp09->sn2[0] ? sp09->sn2 : sp09->sn1;
	} else {
		return -ENOENT;
	}

	if (!sn[0])
		return -ENODATA;

	strlcpy(buf, sn, buflen);

	return 0;
}

/*
 * Standard Android bootloader behavior: give some visible feedback that
 * the board has entered fastboot mode. The vendor's own
 * common/loader/boot_mode.c's fastboot_mode() does this with
 * logo_display() + a weak fastboot_lcd_printf() ("   fastboot mode",
 * via the old lcd.c text console); this is the same idea through
 * upstream's DM_VIDEO vidconsole (the same API splash_display_banner()
 * in common/splash.c already uses) - see board_fastboot_setup()'s
 * declaration in include/fastboot.h for why this is called from
 * fastboot_init() rather than from our own board code directly (it
 * needs to fire whether fastboot was entered manually at the prompt or
 * via CONFIG_BOOTCOMMAND's own fallback).
 */
void board_fastboot_setup(void)
{
	struct udevice *dev;

	/*
	 * vidconsole is auto-bound as a child of the video device itself
	 * whenever CONFIG_VIDEO is enabled (drivers/video/video-uclass.c) -
	 * there's no separate Kconfig symbol gating it, so this simply
	 * no-ops (uclass_first_device_err() fails harmlessly) on a build
	 * without CONFIG_VIDEO, or if the video device was never probed
	 * (e.g. no panel attached).
	 */
	if (uclass_first_device_err(UCLASS_VIDEO_CONSOLE, &dev))
		return;

	/*
	 * Small text in a corner, drawn directly over the boot logo rather
	 * than clearing the screen first - matches how real Android
	 * bootloaders show their "fastboot mode" status text (and the
	 * vendor's own fastboot_lcd_printf() -> lcd_printf() did the same,
	 * over whatever logo_display() had already drawn).
	 *
	 * The boot logo (logo.bmp on the "logo" partition) on this device
	 * has a white-ish background there, so the vidconsole's default
	 * foreground/background pair (usually light-on-dark) would blend
	 * straight into it - confirmed invisible on real hardware.
	 * vidconsole_push_colour()/pop_colour() swap in black-on-white for
	 * just this one string, the same API splash.c's own
	 * splash_display_banner() could use for the same reason, then
	 * restore whatever the console's colors were before.
	 */
	struct vidconsole_colour old;

	vidconsole_push_colour(dev, VID_BLACK, VID_WHITE, &old);
	vidconsole_position_cursor(dev, 2, 2);
	vidconsole_put_string(dev, "   fastboot mode");
	vidconsole_pop_colour(dev, &old);

	/*
	 * force=true - make sure this actually reaches the panel right
	 * now, rather than possibly being skipped by video_sync()'s normal
	 * CONFIG_VIDEO_SYNC_MS throttle (which only matters for repeated
	 * calls in quick succession - this is a one-off).
	 */
	video_sync(dev->parent, true);

	env_set("bootmode", "fastboot");
}

int board_init(void)
{
	/*
	 * PMIC DCDC/LDO voltage-rail sequencing, ported from vendor's
	 * regulator_init()/dcdc_ldo_power_on() (both normally called from
	 * vendor's own board_init(), well before pin_init() there too) -
	 * these rails (RF/analog/camera-clock/IO) need to be up before
	 * anything downstream of them (e.g. the LCD panel, powered on
	 * later from board_late_init()) is touched.
	 */
	if (CONFIG_IS_ENABLED(DM_REGULATOR)) {
		sprd_regulator_voltage_init();
		sprd_dcdc_ldo_power_on();
	}

	pin_init();

	return 0;
}

/*
 * Volume-up is wired into the SC2730 PMIC's own "EIC" (external
 * interrupt controller) block, not a plain AP-side GPIO like
 * volume-down (GPIO 124 below) - vendor's board/.../sprd_kp.c reads it
 * via sprd_eic_get(EIC_KEY2_7S_RST_EXT_RSTN_ACTIVE) (=164), and
 * arch-sharkl5pro/sprd_eic.h's SPRD_ADIE_EIC_START..END (160-167)
 * means that's bit 4 (164 & 0xf) of the PMIC's own EIC_DATA/EIC_MASK
 * registers at ADI offset 0x280 (drivers/power/pmic/sc2730_reg_base.h's
 * ANA_EIC_BASE) - a different register block from volume-down's flat
 * AP-side GPIO space entirely. This PMIC EIC block is declared in
 * arch/arm/dts/sc2730.dtsi as "gpio-controller@280" but has no DM_GPIO
 * driver of its own (unlike watchdog@40/rtc@200, which now do), so
 * it's read directly here with pmic_read()/pmic_write() on the parent
 * PMIC device instead - the same low-level approach
 * drivers/rtc/rtc-sc27xx.c and drivers/watchdog/wdt-sc27xx.c use for
 * their own PMIC sub-blocks, just without a dedicated driver file for
 * a single read-only bit. Active-high (unlike volume-down's
 * active-low GPIO), per vendor's own board_key_scan(): "if
 * (gpio_volumeup > 0) key_code = KEY_VOLUMEUP;".
 */
#define ADT1061_PMIC_EIC_DATA		0x280
#define ADT1061_PMIC_EIC_MASK		0x284
#define ADT1061_PMIC_EIC_VOLUP_BIT	BIT(4)

static int adt1061_volume_up_pressed(void)
{
	struct udevice *pmic_dev;
	u32 mask, data;

	if (uclass_get_device(UCLASS_PMIC, 0, &pmic_dev))
		return 0;

	/* Unmask (vendor's sprd_eic_request()): enable the data register. */
	if (pmic_read(pmic_dev, ADT1061_PMIC_EIC_MASK, (uint8_t *)&mask,
		       sizeof(mask)))
		return 0;
	if (!(mask & ADT1061_PMIC_EIC_VOLUP_BIT)) {
		mask |= ADT1061_PMIC_EIC_VOLUP_BIT;
		pmic_write(pmic_dev, ADT1061_PMIC_EIC_MASK, (uint8_t *)&mask,
			   sizeof(mask));
	}

	if (pmic_read(pmic_dev, ADT1061_PMIC_EIC_DATA, (uint8_t *)&data,
		       sizeof(data)))
		return 0;

	return !!(data & ADT1061_PMIC_EIC_VOLUP_BIT);
}

/*
 * Background charge-safety monitor state - file-scope because
 * bq2560x_charge_monitor_cyclic() below is called by U-Boot's cyclic
 * framework (via schedule(), from arbitrary call sites) long after
 * board_late_init()'s own local variables are gone.
 */
static struct udevice *s_pmic_dev;
static struct udevice *s_chg_dev;
static struct cyclic_info s_charge_cyclic;

/*
 * Runs continuously via U-Boot's cyclic framework (schedule() calls
 * every registered cyclic function) for as long as this build stays
 * in U-Boot - at the idle/autoboot-countdown prompt, between shell
 * commands, and for whatever part of the kernel handoff still calls
 * schedule() before the actual jump to the kernel's entry point.
 * Past that jump this stops running (the kernel owns the CPU), but
 * the chip itself keeps charging autonomously in hardware with
 * CHG_CONFIG left enabled - no software babysitting needed for that
 * part; this is as close as U-Boot-side software can get to "keep
 * charging in the background until the kernel takes over".
 *
 * Does two things every tick:
 *  - feeds the bq2560x's own I2C watchdog (see
 *    bq2560x_feed_watchdog()'s doc comment) so a long idle stretch at
 *    the U-Boot prompt can't quietly revert this driver's calibrated
 *    charge-current/charge-voltage settings back to chip POR
 *    defaults.
 *  - a software over-voltage safety net on top of the chip's own
 *    REG04 VREG regulation and its hardware BAT_OVP fault detection:
 *    if the FGU ever reads the battery at/above chg_ovp_mv (this
 *    board's battery's absolute safety ceiling, matching the
 *    vendor's own chg-bat-safety-vol for this SoC's reference battery
 *    profile - see bq2560x_charger.c's header comment), charging is
 *    force-disabled; it only resumes once voltage has dropped back to
 *    chg_ovp_resume_mv, a deliberate hysteresis gap so this doesn't
 *    chatter on/off right at the ceiling.
 */
static void bq2560x_charge_monitor_cyclic(struct cyclic_info *c)
{
	static int ovp_tripped;
	const int chg_ovp_mv = 4280;
	const int chg_ovp_resume_mv = 4200;
	int vbat_mv;

	if (!s_pmic_dev)
		return;

	if (s_chg_dev)
		bq2560x_feed_watchdog(s_chg_dev);

	if (!CONFIG_IS_ENABLED(SPRD_FGU) || !s_chg_dev)
		return;

	vbat_mv = sprd_fgu_read_vbat_mv(s_pmic_dev);
	if (vbat_mv < 0)
		return;

	if (!ovp_tripped && vbat_mv >= chg_ovp_mv) {
		bq2560x_set_charge_enable(s_chg_dev, 0);
		ovp_tripped = 1;
		printf("charger: battery at %dmV, at/above safety ceiling %dmV - charging disabled\n",
		       vbat_mv, chg_ovp_mv);
	} else if (ovp_tripped && vbat_mv <= chg_ovp_resume_mv) {
		bq2560x_set_charge_enable(s_chg_dev, 1);
		ovp_tripped = 0;
		printf("charger: battery back to %dmV, below %dmV - charging re-enabled\n",
		       vbat_mv, chg_ovp_resume_mv);
	}
}

int board_late_init(void)
{
	/*
	 * Chip-variant (T610/T618/T700) and silicon-revision (AA/AB/AC)
	 * detection, straight from efuse bits - see hwfeature.c. Cheap,
	 * no dependencies on anything else here, so it runs first; sets
	 * "hwfeature_bootargs" (folded into bootargs by
	 * CONFIG_BOOTCOMMAND) so the kernel sees the same
	 * androidboot.auto.efuse=/auto.chipid=/lwfq.type= triplet stock
	 * U-Boot's own cmdline carries, whether or not ft_board_setup()
	 * below also got to run (CONFIG_OF_BOARD_SETUP covers the
	 * devicetree /hwfeature node; this covers the cmdline either way).
	 */
	adt1061_hwfeature_set_bootargs();

	/*
	 * Deliberately runs *before* the video/LCD block below. The panel
	 * (sprd_dsi_resume()/dphy/dpu power-up, see drivers/video/sprd/
	 * sprd_video.c) and especially the backlight PWM are themselves a
	 * real, non-trivial draw on the same battery/charge rail this gate
	 * is protecting - at the user's explicit request, a battery this
	 * low doesn't even get the screen turned on to look at, let alone
	 * a kernel boot: probe the PMIC/charger and hold here first, then
	 * let the video block run (whether because the battery was already
	 * fine, or because charging just brought it up past the
	 * threshold).
	 *
	 * Same lazy-DM-probing story as video below: nothing else in this
	 * tree's boot path ever looks up UCLASS_PMIC or UCLASS_MISC, so the
	 * sc2730 PMIC driver (drivers/power/pmic/pmic_sc2730.c, reached over
	 * the ADI bus) and the bq2560x battery charger
	 * (drivers/power/bq2560x_charger.c, over i2c4 per adt1061.dts'
	 * "bq2560x-charger" node) stay completely untouched even though
	 * both are built in and wired up in the devicetree - left alone,
	 * the charger in particular never gets bq2560x_probe()'s reset +
	 * charge-current-limit programming + CHG_CONFIG enable, so whatever
	 * charge state the ROM/FDL preloader happened to leave it in is
	 * never corrected. Force both probes here; sc2730_regulator.c's
	 * individual DCDC/LDO regulator children are children of the PMIC
	 * node (bound by pmic_bind_children()) but are still probed only
	 * on demand by an actual consumer - nothing here currently needs a
	 * specific rail turned on, so they are deliberately left as-is.
	 */
	{
		struct udevice *pmic = NULL;
		struct udevice *chg = NULL;
		int ret;

		if (CONFIG_IS_ENABLED(PMIC_SC2730)) {
			ret = uclass_first_device_err(UCLASS_PMIC, &pmic);
			if (ret) {
				printf("pmic: sc2730 not probed (%d)\n", ret);
				pmic = NULL;
			}
		}
		if (CONFIG_IS_ENABLED(BQ2560X_CHARGER)) {
			ret = uclass_first_device_err(UCLASS_MISC, &chg);
			if (ret) {
				printf("charger: bq2560x not probed (%d)\n", ret);
				chg = NULL;
			}
		}

		/*
		 * Low-battery boot gate: holds the boot here and lets the
		 * charger (already enabled above by bq2560x_probe()) keep
		 * running until the battery is back above a safe level,
		 * instead of continuing on to boot the kernel on a battery
		 * that's already running low.
		 *
		 * critical_mv (3.5V) replaces the originally-ported 3.8V
		 * threshold - 3.8V is this battery's *nominal* voltage, not
		 * a low-battery cutoff, so gating on it held the boot
		 * almost any time the tablet wasn't freshly fully charged.
		 * Loosened at the user's request to an actual "getting low"
		 * level, now that the battery is known to be a 3.8V-
		 * nominal, dual-cell-single-BMS pack (two 3000mAh/11.4Wh
		 * cells in parallel, 6000mAh/22.8Wh combined - a standard
		 * 3.0-4.2V-class Li-ion range) - see
		 * bq2560x_charger.c's header comment for the full
		 * reasoning. critical_floor_mv (3.2V) is a true emergency
		 * floor, close to this chemistry's practical over-discharge/
		 * UVLO region (the vendor's own reference battery profile
		 * for this SoC puts soft-vbat-uvlo at 3.1V): below it, the
		 * gate does not give up after max_wait_iters - a battery
		 * this flat shouldn't boot the kernel no matter how long
		 * charging takes. Between 3.2V and 3.5V, a bounded wait
		 * still applies, so a FGU read glitch or a genuinely slow
		 * charger can't strand the board here forever.
		 *
		 * This only runs when the PMIC actually probed (no point
		 * gating on a voltage reading we can't trust). schedule()
		 * each loop iteration services this tree's own cyclic
		 * watchdog (see drivers/watchdog/wdt-sc27xx.c) the same way
		 * any other long-running U-Boot loop does, so this gate
		 * can't itself trip a watchdog reset while waiting.
		 */
		if (pmic && CONFIG_IS_ENABLED(SPRD_FGU)) {
			const int critical_mv = 3500;
			const int critical_floor_mv = 3200;
			const int max_wait_iters = 600; /* ~10 min @ 1s */
			int iter;
			bool vibrator_ok;

			/*
			 * Vibrator feedback for this gate specifically: the
			 * screen is deliberately still off here and nobody
			 * may be watching a UART, so a voltage number in a
			 * log nobody reads doesn't tell the person holding
			 * the tablet anything. Two short buzzes the moment
			 * the gate actually engages (only once, when iter
			 * == 0 first sees a low reading) says "charging,
			 * hold on"; one short buzz the moment it releases
			 * (only if it ever engaged - iter > 0) says
			 * "continuing now". A battery that was already fine
			 * gets no buzz at all - there was nothing to report.
			 */
			vibrator_ok = CONFIG_IS_ENABLED(SPRD_VIBRATOR) &&
				      !sprd_vibrator_init(pmic);

			ret = sprd_fgu_init(pmic);
			if (ret) {
				printf("fgu: init failed (%d), skipping low-battery boot gate\n",
				       ret);
			} else {
				for (iter = 0; ; iter++) {
					int vbat_mv = sprd_fgu_read_vbat_mv(pmic);

					if (vbat_mv < 0) {
						printf("fgu: voltage read failed (%d), skipping low-battery boot gate\n",
						       vbat_mv);
						break;
					}

					if (vbat_mv > critical_mv) {
						if (iter > 0) {
							printf("fgu: battery at %dmV, above %dmV - continuing boot\n",
							       vbat_mv, critical_mv);
							if (vibrator_ok)
								sprd_vibrator_pulse(pmic, 1);
						}
						break;
					}

					printf("fgu: battery at %dmV, at/below critical %dmV - holding boot, charging...\n",
					       vbat_mv, critical_mv);
					if (iter == 0 && vibrator_ok)
						sprd_vibrator_pulse(pmic, 2);

					/*
					 * Above the emergency floor, give up
					 * after max_wait_iters and boot
					 * anyway. Below it, there is no
					 * amount of waiting that's "too
					 * long" - keep holding regardless.
					 */
					if (vbat_mv > critical_floor_mv &&
					    iter + 1 >= max_wait_iters) {
						printf("fgu: battery still at/below %dmV after waiting - continuing boot anyway\n",
						       critical_mv);
						break;
					}

					schedule();
					mdelay(1000);
				}
			}
		}

		/*
		 * Background charge-safety monitor: runs continuously from
		 * here on via U-Boot's cyclic framework (not just during
		 * the gate above) - see bq2560x_charge_monitor_cyclic()'s
		 * doc comment above for what it does and why. Registered
		 * whenever the PMIC probed, independent of whether the
		 * low-battery gate above actually engaged, so charging
		 * keeps being supervised (watchdog fed, over-voltage
		 * ceiling enforced) for as long as this build stays in
		 * U-Boot - including sitting idle at the "adt1061=>"
		 * prompt if autoboot is interrupted.
		 */
		if (pmic && CONFIG_IS_ENABLED(SPRD_FGU)) {
			s_pmic_dev = pmic;
			s_chg_dev = chg;
			cyclic_register(&s_charge_cyclic,
					 bq2560x_charge_monitor_cyclic,
					 10 * 1000 * 1000, /* 10s */
					 "bq2560x_charge_mon");
		}
	}

	/*
	 * Unlike storage (mmc_initialize() unconditionally probes every MMC
	 * controller earlier in board_r.c's init sequence, which is why the
	 * "MMC:   mmc@1400000: 0" banner always shows up on its own), this
	 * tree's video stack is never probed automatically: there is no
	 * initr_video() call anywhere in board_r.c any more, and nothing
	 * else here selects "vidconsole" as a console device or triggers a
	 * splash screen - the only two other things that would otherwise
	 * cause a DM uclass lookup of the video device. Left alone, the
	 * "dpu" device (and so sprd_video_probe() -> sprd_panel_probe(),
	 * which is the function that actually powers on and initializes
	 * the LCD panel - see drivers/video/sprd/sprd_video.c) is simply
	 * never instantiated, and the panel stays dark through the whole
	 * U-Boot stage even though the devicetree and panel drivers are
	 * otherwise fully wired up. Force the probe here, the same way
	 * storage ends up "just working" by the time this function runs -
	 * a failure here (e.g. no panel actually attached) is reported but
	 * not fatal to boot. Runs *after* the low-battery gate above, on
	 * purpose - see that block's comment.
	 */
	if (CONFIG_IS_ENABLED(VIDEO)) {
		struct udevice *vdev;
		int ret;

		ret = uclass_first_device_err(UCLASS_VIDEO, &vdev);
		if (ret) {
			printf("video: no display probed (%d)\n", ret);
		} else {
			/*
			 * The panel itself (sprd_panel.c/sprd_dispc.c) has
			 * no backlight of its own - on this hardware the
			 * backlight is a separate PWM output (see
			 * sprd_bl.c, ported from the vendor's own
			 * board/spreadtrum/ums512_1h10/sprd_bl.c) that the
			 * vendor's U-Boot only ever turns on from its
			 * logo_display()/boot_mode.c splash-screen
			 * subsystem, which this tree does not carry over.
			 * Without this call the panel is fully initialized
			 * and streaming video but the screen stays black -
			 * confirmed on real hardware. 200 matches the
			 * vendor's own BACKLIGHT_ON constant
			 * (include/boot_mode.h).
			 */
			set_backlight(200);

			/*
			 * Draw the boot logo from the "logo" partition now
			 * that the panel is up (see splash_screen_prepare()
			 * above for where it actually comes from).
			 * "splashimage" is the scratch RAM address
			 * splash_display() reads the raw partition data into
			 * before handing it to bmp_display() - 0x92000000 is
			 * comfortably clear of both the relocated U-Boot
			 * itself (near the top of the 2 GiB DRAM) and of the
			 * 0x90000000 boot.img staging buffer
			 * CONFIG_BOOTCOMMAND uses later, since the logo is
			 * drawn and done with well before that read happens.
			 * A failure here (no "logo" partition, empty/corrupt
			 * image, etc.) is reported but not fatal to boot.
			 */
			if (CONFIG_IS_ENABLED(SPLASH_SCREEN)) {
				if (!env_get("splashimage"))
					env_set("splashimage", "0x92000000");

				ret = splash_display();
				if (ret)
					printf("logo: splash display from 'logo' partition failed (%d)\n",
					       ret);
			}
		}
	}

	/*
	 * fastboot's "product"/"platform" getvars (see
	 * getvar_product()/getvar_platform() in drivers/fastboot/
	 * fb_getvar.c) just read these two plain env vars - left unset,
	 * which is why "fastboot getvar all" used to report back
	 * "Board not set"/"platform not set". "board" deliberately holds
	 * the specific device codename (adt1061), matching how a real
	 * bootloader answers this getvar - not CONFIG_SYS_BOARD
	 * ("ums512_1h10", the build-system board directory name, which
	 * other devices sharing this same SoC/board code could end up
	 * using too).
	 *
	 * "fastboot.manufacturer"/"fastboot.model" aren't part of the base
	 * AOSP fastboot variable list, but fastboot_getvar() already has a
	 * generic "fastboot.<var>" env fallback for exactly this (see
	 * fastboot_getvar() in fb_getvar.c) - a common vendor convention
	 * that answers "fastboot getvar manufacturer"/"fastboot getvar
	 * model" without needing a dedicated dispatch table entry. They
	 * don't show up under "getvar all" though, since that only walks
	 * the fixed dispatch table.
	 *
	 * Only one device (adt1061) actually exists on this board code
	 * right now, so these are hardcoded; if a second one is added
	 * later, branch on the DT's root "model" property instead.
	 */
	if (!env_get("board"))
		env_set("board", "adt1061");
	if (!env_get("platform"))
		env_set("platform", "sharkl5pro");
	if (!env_get("fastboot.manufacturer"))
		env_set("fastboot.manufacturer", "Aidata");
	if (!env_get("fastboot.model"))
		env_set("fastboot.model", "ADT1061");

	/*
	 * f_fastboot.c only calls g_dnl_set_serialnumber() when env
	 * "serial#" is actually set (drivers/usb/gadget/f_fastboot.c,
	 * fastboot_bind()) - left unset, the USB device descriptor's
	 * iSerialNumber index stays 0 (no string at all), which is why
	 * host tools showed a garbled/blank serial ("????????????") for
	 * this device instead of failing to read a real one.
	 *
	 * Try the real, factory-provisioned serial out of the "miscdata"
	 * partition first (adt1061_read_serial(), above) - confirmed
	 * present and correctly parsed on real retail hardware. Only fall
	 * back to the old fixed placeholder if that partition is missing,
	 * unprovisioned, or on some other device/variant that never had it
	 * written.
	 */
	if (!env_get("serial#")) {
		char sn[64];

		if (adt1061_read_serial(sn, sizeof(sn)) == 0)
			env_set("serial#", sn);
		else
			env_set("serial#", "ADT1061000001");
	}

	/*
	 * Volume-down forces fastboot mode at boot - matches vendor's
	 * board/.../sprd_kp.c: SPRD_VOLUMEDOWN_GPIO is flat/global GPIO
	 * 124, active low (board_key_scan() treats a 0 reading as
	 * "pressed"). CONFIG_BOOTCOMMAND checks the "force_fastboot" env
	 * var this sets and skips straight to fastboot before ever trying
	 * to read the "boot" partition. Vendor's own low_bat exception
	 * (volume-down ignored on critically low battery) isn't ported -
	 * this tree has no battery-gauge/fuel-gauge driver to read that
	 * from.
	 *
	 * This is not vendor behavior, but an addition: volume-down held
	 * together with volume-up (read below, from the PMIC's own EIC
	 * block rather than a plain GPIO) instead forces a recovery boot
	 * - see the BCB block below, which is where that combination is
	 * actually resolved (it needs bcb_set()/bcb_store(), which live
	 * there) and overrides the plain fastboot request set here.
	 */
	int vol_down_pressed = 0;

	if (!gpio_request(124, "vol-down")) {
		gpio_direction_input(124);
		vol_down_pressed = (gpio_get_value(124) == 0);
	}
	if (vol_down_pressed)
		env_set("force_fastboot", "1");

	/*
	 * Standard Android BCB ("misc" partition) reboot-reason handling,
	 * per doc/android/bcb.rst - the mainline equivalent of vendor's
	 * common/loader/recv_mode.c's get_mode_from_file()
	 * ("boot-recovery"/"update-radio" commands) combined with
	 * drivers/misc/check_reboot.c's HWRST_STATUS_FASTBOOT/RECOVERY
	 * dispatch. "update-radio" (vendor's modem-firmware-update
	 * trigger) has no destination in this port - there's no modem/CP
	 * firmware-update mechanism ported at all - so it's deliberately
	 * not handled here; everything else vendor's switch covers either
	 * reduces to one of the two cases below or doesn't apply to this
	 * non-A/B device (no "boot-fastboot"-equivalent slot logic, etc).
	 *
	 * CONFIG_BOOTCOMMAND reads "bootpart" (defaulting to "boot") and
	 * prepends "extra_bootargs" to the kernel command line - this is
	 * the only thing that sets either one.
	 */
	if (IS_ENABLED(CONFIG_CMD_BCB) &&
	    !bcb_find_partition_and_load("mmc", 0, "misc")) {
		char bcb_cmd[32] = { 0 };
		int have_cmd;

		/*
		 * Volume-up + volume-down held together forces a recovery
		 * boot - synthesized as the exact same BCB "boot-recovery"
		 * command a software reboot-to-recovery request (ADB
		 * "reboot recovery", Android Recovery System spec, etc)
		 * would use, so the outcome (and its survival across an
		 * interrupted boot, since it's about to be persisted below
		 * exactly like a real one) is identical either way. This
		 * also overrides the plain volume-down-only fastboot
		 * request above - recovery takes priority when both keys
		 * are held, matching how volume-up is the "stronger"
		 * modifier key on stock Android key combos in general.
		 */
		if (vol_down_pressed && adt1061_volume_up_pressed()) {
			env_set("force_fastboot", "");
			strlcpy(bcb_cmd, "boot-recovery", sizeof(bcb_cmd));
			have_cmd = 1;
		} else {
			have_cmd = !bcb_get(BCB_FIELD_COMMAND, bcb_cmd,
					    sizeof(bcb_cmd)) && bcb_cmd[0];
		}

		if (have_cmd) {
			/*
			 * "boot-fastboot" is vendor's own string for this;
			 * "bootonce-bootloader" is the AOSP-standard one
			 * doc/android/bcb.rst itself documents - honor
			 * both. Either way this means the bootloader's own
			 * fastboot (fastboot usb 0 below), not a separate
			 * fastbootd-in-recovery boot - this device has no
			 * recovery-ramdisk-driven fastbootd path ported.
			 */
			if (!strcmp(bcb_cmd, "boot-fastboot") ||
			    !strcmp(bcb_cmd, "bootonce-bootloader")) {
				bcb_set(BCB_FIELD_COMMAND, "");
				bcb_store();
				env_set("force_fastboot", "1");
			} else if (!strcmp(bcb_cmd, "boot-recovery")) {
				bcb_set(BCB_FIELD_COMMAND, "");
				bcb_store();
				/*
				 * If this device turns out to have no
				 * separate "recovery" GPT partition,
				 * CONFIG_BOOTCOMMAND's "part start mmc 0
				 * recovery ..." lookup simply fails and the
				 * whole chain falls through to its own
				 * "fastboot usb 0" fallback - safe (lands in
				 * the bootloader, not a silently-wrong normal
				 * boot) either way, but only real hardware
				 * testing says which case this device is.
				 */
				env_set("bootpart", "recovery");
				env_set("extra_bootargs",
					"androidboot.bootreason=boot-recovery");
			}
		}

		bcb_reset();
	}

	return 0;
}

/*
 * arch_preboot_os() is boot/bootm_os.c's weak hook, called right before the
 * architecture-specific jump into the kernel (both the legacy "bootm" path
 * via boot_selected_os() and "booti"/"bootz" go through it). CONFIG_WATCHDOG
 * above only makes *U-Boot itself* feed the SC2730 watchdog periodically via
 * the cyclic framework while U-Boot's own main loop is alive - that feeding
 * stops dead the instant control passes to the kernel, but the hardware
 * counter keeps counting down in silicon regardless of what's running on
 * the CPU. If the kernel's own matching driver (mainline Linux also uses
 * "sprd,sc2731-wdt") doesn't probe and re-arm it before the current timeout
 * (CONFIG_WATCHDOG_TIMEOUT_MSECS, 60s default) expires, the board resets
 * itself partway through the kernel boot - a silent, hard-to-diagnose
 * regression that this watchdog work would otherwise introduce. Stopping it
 * here hands the kernel a clean slate: if its devicetree/driver re-enables
 * the watchdog, that is the kernel's own explicit decision, not a leftover
 * countdown from the bootloader. Same pattern as
 * board/alliedtelesis/x530/x530.c's arch_preboot_os().
 */
void arch_preboot_os(void)
{
	if (CONFIG_IS_ENABLED(WDT))
		wdt_stop_all();
}

int dram_init(void)
{
	return fdtdec_setup_mem_size_base();
}

int dram_init_banksize(void)
{
	return fdtdec_setup_memory_banksize();
}

#if CONFIG_IS_ENABLED(OF_BOARD_SETUP)
/*
 * Called by boot/image-fdt.c's image_setup_libfdt() right before the
 * kernel starts, on the exact same FDT blob CONFIG_BOOTCOMMAND already
 * extracted from the boot image and is about to hand to bootm/booti -
 * this is in addition to, not instead of, "hwfeature_bootargs" above
 * (kernel-side code that reads /hwfeature directly rather than only
 * the cmdline still finds it this way).
 */
int ft_board_setup(void *blob, struct bd_info *bd)
{
	int ret;

	ret = adt1061_hwfeature_fixup_fdt(blob);
	if (ret)
		printf("hwfeature: failed to write /hwfeature to devicetree (%d)\n",
		       ret);

	return 0;
}
#endif

