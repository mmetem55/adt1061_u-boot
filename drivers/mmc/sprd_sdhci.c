// SPDX-License-Identifier: GPL-2.0+
/*
 * Thin DM glue driver for the Unisoc UMS512 (sharkl5pro) "sprd,sdhci-r11"
 * eMMC/SD controller, binding upstream's own generic SDHCI core
 * (drivers/mmc/sdhci.c / sdhci-uclass.c).
 *
 * The vendor U-Boot 2015.07 tree's drivers/mmc/sprd_sdhci_r11.c (and the
 * sprd_mmc_r11.c caller that actually drives this board, via
 * sprd_sdhci_init()) turned out to be close to a verbatim copy of an
 * old (pre-DM) version of U-Boot's own generic SDHCI core: same
 * SDHCI_QUIRK_* names, same struct sdhci_host/mmc_config shape, same
 * sdhci_setup_cfg()-style init flow. Rather than port that
 * reimplementation, this driver just supplies the two things that are
 * actually board/SoC specific - the register base (from the DT "reg")
 * and the two clocks ("sdio": the AP clk "*_2X" mux, "enable": the
 * apapb_gate bus-enable gate) - and lets the upstream core do the rest.
 *
 * Confirmed from the vendor's sprd_host_init()/board/spreadtrum/
 * ums512_1h10_go/sdio_cfg.c reference table (same SoC family): the
 * eMMC/SD host is initialized with quirks = 0 (no device-specific SDHCI
 * quirks), max_clk ~= 195 MHz ("2X" clock domain, so the controller's
 * external base clock is actually driven at 2x that and halved
 * internally - hence clk_set_rate() below targets max_clk * 2), and
 * min_clk = 400 kHz (card identification).
 */

#include <clk.h>
#include <dm.h>
#include <sdhci.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/io.h>

/* "2X" clock domain: external clock is driven at 2x the SDHCI base
 * clock the controller actually uses (see file header comment).
 */
#define SPRD_SDHCI_MAX_CLK	195000000UL
#define SPRD_SDHCI_MIN_CLK	400000UL

/*
 * SPRD-specific DLL delay-line register, NOT part of the standard SDHCI
 * register set (confirmed from the vendor's drivers/mmc/sprd_sdhci_r11.c /
 * include/sprd_sdhci_r11.h: SDHCI_REG_32_DLL_DLY_OFFSET). It controls
 * invert/delay settings for the write, command and read (posedge/negedge)
 * data-sampling clocks.
 *
 * The vendor's sdhci_set_ios() calls sdhc_set_dll_invert(host,
 * SPRD_SDHC_BIT_CMD_DLY_INV | SPRD_SDHC_BIT_POSRD_DLY_INV, 1)
 * UNCONDITIONALLY on every set_ios (i.e. every clock/bus-width change,
 * not just once at probe or only for high-speed/DDR modes) - this is
 * mandatory sampling-clock-polarity setup for this controller, not an
 * optional tuning step. Confirmed missing was the actual bug behind a
 * real-hardware DATA_CRC error reading the eMMC's EXT_CSD register
 * (CMD8 with data, the very first real DATA-phase transfer attempted):
 * all data-less commands (CMD0/1/2/3/9...) worked fine without this,
 * but the first genuine 512-byte PIO data read consistently failed its
 * CRC check. Reproduced via upstream's sdhci.c "set_ios_post" hook,
 * which exists precisely for this kind of platform-specific post-set_ios
 * register poke.
 */
#define SPRD_SDHCI_REG_DLL_DLY_OFFSET	0x208
#define SPRD_SDHCI_BIT_CMD_DLY_INV	BIT(13)
#define SPRD_SDHCI_BIT_POSRD_DLY_INV	BIT(21)

/*
 * The DLL invert bits above, confirmed set and sticking on real hardware,
 * made zero observable difference to the EXT_CSD DATA_CRC failure. Found
 * a second, separate SPRD-specific quirk in the same vendor source: every
 * sdhci_send_command()-equivalent in drivers/mmc/sprd_sdhci_r11.c writes
 * the *16-bit* value 0x080e to SDHCI_TIMEOUT_CONTROL (0x2E), not the
 * plain 8-bit 0xe that upstream's generic sdhci_send_command() writes.
 * Since SDHCI_TIMEOUT_CONTROL is only 1 byte, a 16-bit write there also
 * writes the byte at 0x2F - which is SDHCI_SOFTWARE_RESET standard bits
 * (ALL=0x01/CMD=0x02/DATA=0x04) in upstream, but bit3 (0x08) is unused by
 * any of those. This looks like a SPRD-private extra enable bit at that
 * same byte offset, always set alongside the timeout value on this
 * controller - plausibly something this custom silicon needs for correct
 * data-phase CRC/timeout behavior, which upstream's plain 8-bit write
 * never sets. Set it here (read-modify-write, so any in-progress standard
 * reset bit is left alone) alongside the DLL invert bits.
 */
#define SPRD_SDHCI_SOFTWARE_RESET_EXTRA_BIT	BIT(3)

/*
 * Neither of the two SPRD SDHCI-level register quirks above made any
 * difference to the EXT_CSD DATA_CRC failure on real hardware. Found the
 * actual missing piece by reading the vendor's real board bring-up path
 * for THIS exact board (board/spreadtrum/ums512_1h10/sdio_cfg.c's EMMC
 * entry + drivers/mmc/sprd_mmc_r11.c's sprd_sdhci_init()): after enabling
 * the AHB/APB bus-clock gate for the eMMC controller, the vendor driver
 * also explicitly pulses a SEPARATE soft-reset line for the controller -
 * assert then immediately deassert a bit in the AP_APB block's own reset
 * register (arch/arm/include/asm/arch-sharkl5pro/chip_sharkl5pro/
 * ap_apb.h: REG_AP_APB_APB_RST, BIT_AP_APB_EMMC_SOFT_RST). This is a
 * second, independent register block from the SDHCI controller's own
 * MMIO range (host->ioaddr) - it is AP-global AHB/APB infrastructure,
 * not part of the SDHCI register set - so clk_enable() on the "enable"
 * clock (which only toggles the bus-clock *gate* bit, REG_AP_APB_APB_EB /
 * BIT_AP_APB_EMMC_EB, via the generic DM clock framework) never touches
 * it. Without this reset pulse the controller can power up holding
 * stale state from whatever the previous boot stage left it in, which
 * would explain a failure specific to the very first real DATA-phase
 * transfer (EXT_CSD) while every data-less command (CMD0/1/2/3/9...)
 * worked fine.
 */
#define SPRD_AP_APB_BASE		0x71000000UL
#define SPRD_AP_APB_APB_RST		(SPRD_AP_APB_BASE + 0x0004)
#define SPRD_AP_APB_EMMC_SOFT_RST	BIT(19)

/*
 * The AHB/APB reset pulse above also made zero difference on real
 * hardware. Went back to the vendor source once more and found
 * something far more fundamental in include/sprd_sdhci_r11.h: on this
 * controller, SDHCI_BLOCK_COUNT is redefined to offset 0x00 (with the
 * legacy offset 0x06 explicitly commented out right next to it), and
 * the file's own top-of-file comment says outright: "this code only
 * support [...] host controller version 4.10". That is the SD Host
 * Controller spec's "Version 4" register layout: when Host Control2's
 * "V4 Mode" bit is set, the legacy 16-bit Block Count register at 0x06
 * is superseded by a 32-bit Block Count register living at 0x00 (the
 * same offset upstream's SDHCI_DMA_ADDRESS legacy register occupies,
 * which our PIO-only driver never touches anyway).
 *
 * Upstream's generic sdhci.c has no notion of V4 mode at all - it
 * always programs the legacy SDHCI_BLOCK_COUNT (0x06). If this SoC's
 * controller boots up with V4 Mode left enabled by the previous boot
 * stage (BootROM/FDL), our block-count write to 0x06 lands on a
 * register the hardware no longer treats as the active block count,
 * so the controller transfers using whatever stale/zero count is left
 * in the real (0x00) register - while SDHCI_BLOCK_SIZE (0x04, unchanged
 * between legacy and V4 layouts) is still programmed correctly. That
 * mismatch between what we PIO-read (one blocksize's worth) and what
 * the controller's state machine actually thinks it's transferring
 * would manifest as exactly a DATA_CRC/transfer-boundary error on the
 * very first genuine data-phase command (EXT_CSD) while every
 * data-less command (which never touches block count) keeps working.
 *
 * First tried explicitly clearing Host Control2's V4 Mode bit (BIT(12))
 * so the controller would revert to the legacy register layout upstream's
 * sdhci.c programs. Confirmed on real hardware this bit is stuck-at-1 -
 * readback after the clear still showed it set - so V4 Mode on this
 * controller is NOT a runtime-switchable register, it's the permanent,
 * hard-wired register layout. SDHCI_32BIT_BLK_CNT / SDHCI_QUIRK_USE_
 * 32BIT_BLK_CNT (include/sdhci.h, drivers/mmc/sdhci.c) teach upstream's
 * generic sdhci.c to write block count to the real (0x00) register
 * instead of trying to disable V4 Mode. Set that quirk below.
 */

static void sprd_sdhci_reset_pulse(void)
{
	void __iomem *rst_reg = (void __iomem *)SPRD_AP_APB_APB_RST;
	u32 val;

	val = readl(rst_reg);
	writel(val | SPRD_AP_APB_EMMC_SOFT_RST, rst_reg);
	udelay(10);
	val = readl(rst_reg);
	writel(val & ~SPRD_AP_APB_EMMC_SOFT_RST, rst_reg);
	udelay(10);
}

/*
 * The CMD_DLY_INV|POSRD_DLY_INV-only poke above is sufficient at the
 * 400 kHz ID-mode clock (where sampling margins are huge) but CMD13
 * (plain R1 status, no data phase at all) started failing with a
 * genuine command TIMEOUT the moment the clock moved to the negotiated
 * high-speed rate (25 MHz) during mode selection - not a HISPD-bit
 * issue (SDHCI_QUIRK_NO_HISPD_BIT made no difference; hostctrl already
 * read back 0x00 before that fix too), so this is a sampling-clock
 * tuning problem specific to running at speed, affecting commands in
 * general, not just data.
 *
 * Confirmed from the vendor's real sdhci_set_ios() (drivers/mmc/
 * sprd_sdhci_r11.c): it calls back into a per-device sdhci_get_delay()
 * hook ONLY when "mmc->clock == mmc->tran_speed" (i.e. only once the
 * clock has actually reached the final negotiated speed, never during
 * ID-mode or other intermediate clock changes), writing the result
 * directly to this same DLL_DLY register (0x208). The real per-device
 * value for this board family (board/spreadtrum/ums512_1h10_go/
 * sdio_cfg.c's hs_dly field) is 0x30303060 - which already has both
 * CMD_DLY_INV (bit13) and POSRD_DLY_INV (bit21) baked in, plus actual
 * non-zero delay-line tap values in the other bitfields that our
 * invert-only poke never set. Apply it under the same condition the
 * vendor uses.
 */
#define SPRD_SDHCI_HS_DLY_VALUE		0x30303060UL

static int sprd_sdhci_set_ios_post(struct sdhci_host *host)
{
	u32 dll_dly_offset;
	bool at_tran_speed = host->mmc && host->mmc->clock &&
			     host->mmc->clock == host->mmc->tran_speed;

	if (at_tran_speed) {
		dll_dly_offset = SPRD_SDHCI_HS_DLY_VALUE;
	} else {
		dll_dly_offset = sdhci_readl(host, SPRD_SDHCI_REG_DLL_DLY_OFFSET);
		dll_dly_offset |= SPRD_SDHCI_BIT_CMD_DLY_INV |
				   SPRD_SDHCI_BIT_POSRD_DLY_INV;
	}
	sdhci_writel(host, dll_dly_offset, SPRD_SDHCI_REG_DLL_DLY_OFFSET);

	return 0;
}

static const struct sdhci_ops sprd_sdhci_ops = {
	.set_ios_post	= sprd_sdhci_set_ios_post,
};

struct sprd_sdhci_plat {
	struct mmc_config cfg;
	struct mmc mmc;
};

static int sprd_sdhci_probe(struct udevice *dev)
{
	struct mmc_uclass_priv *upriv = dev_get_uclass_priv(dev);
	struct sprd_sdhci_plat *plat = dev_get_plat(dev);
	struct sdhci_host *host = dev_get_priv(dev);
	struct clk sdio_clk, enable_clk;
	ulong max_clk;
	int ret;

	host->ioaddr = dev_read_addr_ptr(dev);
	if (!host->ioaddr)
		return -EINVAL;

	ret = clk_get_by_name(dev, "enable", &enable_clk);
	if (ret)
		return ret;

	ret = clk_enable(&enable_clk);
	if (ret)
		return ret;

	/*
	 * Matches the vendor's real sprd_sdhci_init() order: bus-clock
	 * gate enable, then an explicit soft-reset pulse of the
	 * controller, before touching any SDHCI registers. See the
	 * comment above sprd_sdhci_reset_pulse().
	 */
	sprd_sdhci_reset_pulse();

	ret = clk_get_by_name(dev, "sdio", &sdio_clk);
	if (ret)
		return ret;

	/*
	 * Drive the external "2X" clock domain; the controller itself
	 * only ever sees half of this (see file header comment).
	 *
	 * "sdio" (CLK_EMMC_2X) is a SPRD_MUX_CLK (drivers/clk/sprd/
	 * sharkl5pro-clk.c) - a plain input mux with no divider, so it
	 * has no .set_rate op at all and clk_set_rate() on it always
	 * returns -ENOSYS (confirmed on real ADT1061 hardware: this
	 * aborted the whole eMMC probe). That's fine - the mux's fixed
	 * parent rate is already the right ballpark (see the max_clk
	 * fallback below), so treat -ENOSYS here as "nothing to tune,
	 * keep going" rather than a probe failure.
	 */
	ret = clk_set_rate(&sdio_clk, SPRD_SDHCI_MAX_CLK * 2);
	if (IS_ERR_VALUE(ret) && ret != -ENOSYS)
		return ret;

	ret = clk_enable(&sdio_clk);
	if (ret)
		return ret;

	max_clk = clk_get_rate(&sdio_clk);
	if (IS_ERR_VALUE(max_clk) || !max_clk)
		max_clk = SPRD_SDHCI_MAX_CLK * 2;

	host->name = dev->name;
	/*
	 * Confirmed from the vendor's real sdhci_send_command()/
	 * sdhci_transfer_data() (drivers/mmc/sprd_sdhci_r11.c, built with
	 * CONFIG_MMC_SDMA defined for this exact board in
	 * include/configs/ums512_1h10.h): every data command sets
	 * SDHCI_TRNS_DMA, programs the SDMA address registers and flushes
	 * the cache - this controller's data phase is driven by simple
	 * SDMA in production, not bare PIO. The 32-bit block count fix
	 * alone got the EXT_CSD command to complete without a DATA_CRC
	 * error, but the bytes placed in our buffer by pure PIO reads
	 * still came out wrong/periodically corrupted (confirmed by
	 * dumping the raw ext_csd buffer on real hardware) - consistent
	 * with this controller's PIO data path not being reliable/
	 * complete on its own, mirroring the earlier confirmed finding
	 * that this SoC's USB MUSB bulk endpoints are hardware-DMA-locked
	 * too. USE_SDMA (below) turns on upstream's existing generic SDMA
	 * support (drivers/mmc/sdhci.c); SDHCI_QUIRK_V4_DMA_ADDRESS redirects
	 * its SDMA address register write to the real V4-mode offset
	 * (0x58, matching vendor's SDHCI_DMA_ADDRESS_LOW) instead of the
	 * legacy 0x00 offset, which is this controller's 32-bit Block
	 * Count register instead.
	 */
	/*
	 * EXT_CSD now reads back as fully coherent, real card data (byte
	 * 192 EXT_CSD_REV=8, byte 194 EXT_CSD_STRUCTURE=2, byte 196
	 * CARD_TYPE=0x57 - all textbook-plausible eMMC 5.1 values) -
	 * confirmed the SDMA fix above actually works. The next failure
	 * is CMD6 (SWITCH, R1b/busy response, no data phase) during speed
	 * mode selection: INT_STATUS repeatedly comes back with
	 * CMD_COMPLETE + ERROR set but never DATA_END, for every speed
	 * mode attempted, at every retry. mask requires DATA_END for R1b
	 * commands (busy-signal completion) - this controller's busy/R1b
	 * completion signaling via that interrupt looks unreliable, which
	 * is exactly what SDHCI_QUIRK_BROKEN_R1B exists for: for a
	 * data-less R1b command it polls SDHCI_PRESENT_STATE's
	 * SDHCI_DAT_ACTIVE bit directly instead of waiting on the
	 * interrupt-based DATA_END signal.
	 */
	/*
	 * BROKEN_R1B fixed the data-less CMD6 (SWITCH) busy-wait: this
	 * controller's busy/R1b completion signaling via the DATA_END
	 * interrupt is unreliable, so for a data-less R1b command it polls
	 * SDHCI_PRESENT_STATE's SDHCI_DAT_ACTIVE bit directly instead.
	 * Confirmed necessary on real hardware: removing it makes CMD6 fail
	 * immediately again. SDHCI_QUIRK_NO_HISPD_BIT mirrors the vendor's
	 * own sdhci_set_ios() comment ("SPRD MMC controller can not be set
	 * high speed mode, this bit must be set to zero") but is actually a
	 * no-op on this controller - SDHCI_CTRL_HISPD already reads back as
	 * 0x00 with or without the quirk. Kept for documentation/parity with
	 * the vendor driver; harmless either way. The real fix for the
	 * CMD13-after-CMD6 timeout at 25 MHz turned out to be
	 * SDHCI_QUIRK_SPRD_CMD_TIMEOUT_RESET below.
	 */
	/*
	 * SDHCI_QUIRK_SPRD_CMD_TIMEOUT_RESET: confirmed root cause of the
	 * CMD13-after-CMD6 command timeout at 25 MHz - the vendor driver
	 * writes the combined TIMEOUT_CONTROL/SOFTWARE_RESET 16-bit value
	 * (0x080e) before *every* command, not just data/busy ones. See
	 * the quirk's comment in include/sdhci.h.
	 */
	host->quirks = SDHCI_QUIRK_USE_32BIT_BLK_CNT |
		       SDHCI_QUIRK_V4_DMA_ADDRESS |
		       SDHCI_QUIRK_BROKEN_R1B |
		       SDHCI_QUIRK_NO_HISPD_BIT |
		       SDHCI_QUIRK_SPRD_CMD_TIMEOUT_RESET;
	host->flags |= USE_SDMA;
	host->ops = &sprd_sdhci_ops;
	host->bus_width = dev_read_u32_default(dev, "bus-width", 4);
	host->max_clk = max_clk / 2;

	host->mmc = &plat->mmc;
	host->mmc->priv = host;
	host->mmc->dev = dev;
	upriv->mmc = host->mmc;

	/*
	 * mmc_of_parse() is what actually reads standard mmc DT properties
	 * ("non-removable", "bus-width", "no-1-8-v", ...) into cfg->host_caps
	 * - without calling it, cfg->host_caps never gets MMC_CAP_NONREMOVABLE
	 * set even though the DT node has "non-removable;" (see adt1061.dts'
	 * &sdio3 node). sdhci_get_cd() then falls through to reading the
	 * SDHCI_PRESENT_STATE/SDHCI_CARD_PRESENT register bit instead of
	 * short-circuiting to "always present" - and this eMMC chip has no
	 * card-detect line wired at all (it's soldered down), so that bit
	 * reads as "not present". mmc_getcd() then returns 0 and
	 * mmc_init() bails out early with "MMC: no card present" / -ENOMEDIUM
	 * (-123), confirmed on real ADT1061 hardware (mmc@1400000 showed up
	 * via "mmc list" - so probe() itself succeeded - but "mmc info"/
	 * "mmc part" reported no card). Call mmc_of_parse() here, before
	 * sdhci_setup_cfg(), so it does.
	 */
	ret = mmc_of_parse(dev, &plat->cfg);
	if (ret)
		return ret;

	ret = sdhci_setup_cfg(&plat->cfg, host, host->max_clk,
			      SPRD_SDHCI_MIN_CLK);
	if (ret)
		return ret;

	return sdhci_probe(dev);
}

static int sprd_sdhci_bind(struct udevice *dev)
{
	struct sprd_sdhci_plat *plat = dev_get_plat(dev);

	return sdhci_bind(dev, &plat->mmc, &plat->cfg);
}

static const struct udevice_id sprd_sdhci_ids[] = {
	{ .compatible = "sprd,sdhci-r11" },
	{ }
};

U_BOOT_DRIVER(sprd_sdhci_drv) = {
	.name		= "sprd_sdhci",
	.id		= UCLASS_MMC,
	.of_match	= sprd_sdhci_ids,
	.ops		= &sdhci_ops,
	.bind		= sprd_sdhci_bind,
	.probe		= sprd_sdhci_probe,
	.priv_auto	= sizeof(struct sdhci_host),
	.plat_auto	= sizeof(struct sprd_sdhci_plat),
};
