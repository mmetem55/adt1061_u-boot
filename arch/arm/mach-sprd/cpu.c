// SPDX-License-Identifier: GPL-2.0+
/*
 * Unisoc (Spreadtrum) SoC common code.
 *
 * U-Boot proper is loaded directly into DRAM by the vendor FDL1/FDL2
 * preloader, which also performs DRAM training/sizing. There is no
 * SPL stage and no DRAM controller init to do here.
 */

#include <command.h>
#include <cpu_func.h>
#include <init.h>
#include <log.h>
#include <stdio.h>
#include <linux/compiler.h>
#include <asm/global_data.h>
#include <asm/system.h>
#include <asm/armv8/mmu.h>

DECLARE_GLOBAL_DATA_PTR;

extern struct mm_region sprd_mem_map[];
extern const unsigned int sprd_mem_map_count;

int arch_cpu_init(void)
{
	icache_enable();

	return 0;
}

/*
 * This board's eMMC (sprd_sdhci.c, generic drivers/mmc/sdhci.c SDMA path)
 * only ever programs a 32-bit DMA address register (SDHCI_DMA_ADDRESS /
 * SDHCI_V4_DMA_ADDRESS - both a single sdhci_writel(), see sdhci.c's
 * sdhci_prepare_dma()) - there is no ADMA64 descriptor use here, so any
 * buffer above 4 GiB gets a silently truncated/wrong DMA target.
 *
 * With more than ~2 GiB of DRAM declared, U-Boot's own relocated working
 * set (its stack, malloc pool, and the relocated image itself, all placed
 * near gd->ram_top by board_f.c) ends up above the 4 GiB mark - and the
 * small on-stack buffers mmc_startup() uses for the EXT_CSD/CID/CSD data
 * reads are exactly such a buffer. That is what was behind the
 * "mmc_init: -22" / failed EXT_CSD read seen once 3 GiB was declared and
 * the earlier MMU-table bug (see mem_map.c) stopped actually hanging
 * things before this point could even be reached.
 *
 * This does NOT affect how much RAM Linux sees: gd->ram_size (and the
 * devicetree /memory node it came from) are untouched - only U-Boot's
 * own relocation target is kept inside the first 4 GiB, which is the
 * same technique board_f.c's own generic board_get_usable_ram_top()
 * already uses for CFG_SYS_SDRAM_BASE boards whose RAM runs past the
 * 32-bit address space.
 */
phys_addr_t board_get_usable_ram_top(phys_size_t total_size)
{
	const phys_addr_t dma_safe_limit = 0x100000000ULL; /* 4 GiB */

	if (gd->ram_top > dma_safe_limit)
		return dma_safe_limit;

	return gd->ram_top;
}

void enable_caches(void)
{
	int ret;

	/*
	 * mem_map (arch/arm/mach-sprd/mem_map.c) only has the low-2GiB
	 * MMIO entry filled in statically. Fill in the DRAM entry/entries
	 * now, from gd->dram[] (the REAL, devicetree-declared runtime
	 * size), before the MMU is turned on below - otherwise, if DRAM
	 * is ever declared larger than whatever used to be hardcoded
	 * there, U-Boot (already relocated near the top of that real
	 * DRAM) would enable the MMU with a table that doesn't cover its
	 * own relocated code/stack, and hang silently right here with no
	 * way to even print an exception.
	 */
	ret = mem_map_from_dram_banks(1, sprd_mem_map_count,
				       PTE_BLOCK_MEMTYPE(MT_NORMAL) |
				       PTE_BLOCK_INNER_SHARE);
	if (ret)
		log_err("enable_caches: mem_map_from_dram_banks failed (%d)\n",
			ret);

	/* D-cache is safe to enable once DRAM is up (done by the preloader) */
	dcache_enable();
}

int print_cpuinfo(void)
{
	printf("CPU:   Unisoc UMS512 (sharkl5pro)\n");

	return 0;
}

/*
 * drivers/power/pmic/pmic_sc2730.c - triggers a real hardware reset via
 * the PMIC's own watchdog block. See that function's header comment:
 * this platform's PSCI firmware doesn't actually implement
 * SYSTEM_RESET (confirmed on real hardware - reset_misc()'s SMC call,
 * just above in do_reset()'s call sequence, returns without resetting),
 * so this is what actually does the job.
 */
#if CONFIG_IS_ENABLED(PMIC_SC2730)
int sc2730_pmic_reset(void);
#endif

void reset_cpu(void)
{
#if CONFIG_IS_ENABLED(PMIC_SC2730)
	int ret;

	ret = sc2730_pmic_reset();
	if (ret)
		printf("Unisoc reset_cpu(): sc2730 watchdog reset setup failed (%d), halting.\n",
		       ret);
#else
	printf("Unisoc reset_cpu(): no PMIC watchdog driver for this board, halting.\n");
#endif

	/*
	 * Either the watchdog is now about to fire (a few ms away), or
	 * setting it up failed outright - either way there's nothing left
	 * to do here but wait.
	 *
	 * Deliberately a plain busy-loop, NOT wfi(): vendor's own
	 * reset_cpu() (drivers/misc/reset.c, built for CONFIG_SPRD_ADI -
	 * exactly our platform) does the same after arming the watchdog
	 * (start_watchdog(5); printf("reset cpu\n"); while (1) ;) and never
	 * uses wfi() here. Confirmed on real hardware that wfi() leaves the
	 * board in a state where the watchdog reset never visibly completes
	 * (no UART output at all afterwards) - suspected to be the CPU
	 * sitting in a low-power/sleep state that the chip-level reset
	 * signal doesn't cleanly pull it out of on this platform.
	 */
	while (1)
		;
}

/*
 * "poweroff" command - drivers/power/pmic/pmic_sc2730.c arms the
 * PMIC's own hardware power-down sequencer (BIT_PWR_OFF_SEQ_EN). See
 * that function's header comment for which register this is and why
 * it's expected to actually cut power rather than just halting like
 * reset_cpu() above has to without it.
 */
#if CONFIG_IS_ENABLED(PMIC_SC2730)
int sc2730_pmic_poweroff(void);
#endif

int do_poweroff(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
#if CONFIG_IS_ENABLED(PMIC_SC2730)
	int ret;

	ret = sc2730_pmic_poweroff();
	if (ret)
		printf("Unisoc do_poweroff(): sc2730 power-down sequence setup failed (%d), halting instead.\n",
		       ret);
#else
	printf("Unisoc do_poweroff(): no PMIC driver for this board, halting instead.\n");
#endif

	/*
	 * Same reasoning as reset_cpu()'s trailing loop: either the PMIC
	 * is about to cut power, or arming it failed outright - either
	 * way there's nothing left to do here but wait, and a plain
	 * busy-loop rather than wfi() to match reset_cpu()'s own
	 * hardware-confirmed choice above.
	 */
	while (1)
		;

	return 0;
}
