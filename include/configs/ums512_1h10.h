/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Configuration for Unisoc UMS512 (sharkl5pro) based boards:
 * the Unisoc UMS512-1H10 reference board and the Aidata ADT1061
 * tablet.
 */

#ifndef __UMS512_1H10_H
#define __UMS512_1H10_H

#include <linux/sizes.h>

/* Physical memory, as handed to us already-trained by the preloader */
#define CFG_SYS_SDRAM_BASE		0x80000000

/*
 * Boot from the on-board eMMC (device 0, see the "&sdio3" override in
 * arch/arm/dts/adt1061.dts / drivers/mmc/sprd_sdhci.c), now that a
 * DM_MMC driver exists for this SoC (porting stage 4).
 */
#define BOOT_TARGET_DEVICES(func) \
	func(MMC, mmc, 0)

#include <config_distro_bootcmd.h>

#define CFG_EXTRA_ENV_SETTINGS \
	"scriptaddr=0x90000000\0" \
	"fdt_addr_r=0x91000000\0" \
	"kernel_addr_r=0x92000000\0" \
	"ramdisk_addr_r=0x94000000\0" \
	BOOTENV

#endif /* __UMS512_1H10_H */
