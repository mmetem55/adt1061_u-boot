// SPDX-License-Identifier: GPL-2.0+
/*
 * MMU table for Unisoc UMS512 (sharkl5pro) boards.
 *
 * Entry 0 covers the low 2 GiB MMIO/peripheral space (device memory).
 * This is intentionally coarse-grained for the initial bring-up; it
 * can be refined once more of the SoC address map is ported.
 *
 * The DRAM entry/entries are NOT filled in statically here - they used
 * to be hardcoded to a fixed 2 GiB, which silently diverged from
 * whatever size the devicetree actually declared (CONFIG_NR_DRAM_BANKS
 * banks' real runtime extent, in gd->dram[]). When the devicetree later
 * declared 3 GiB, relocation correctly placed U-Boot near the top of
 * the real 3 GiB, but this table still only mapped the bottom 2 GiB -
 * so the moment enable_caches() turned the MMU on with this table, the
 * CPU's own PC/stack/relocated code were all running from unmapped
 * memory, producing a silent hang (no exception could even be printed,
 * since the exception vectors themselves weren't guaranteed mapped
 * either). The array is sized with room for CONFIG_NR_DRAM_BANKS
 * entries plus the terminator, and mach-sprd's enable_caches() (see
 * cpu.c) fills the DRAM entry/entries in dynamically from gd->dram[]
 * via mem_map_from_dram_banks() before the MMU is enabled, so this
 * always matches the real, current devicetree-declared size.
 */

#include <asm/armv8/mmu.h>
#include <linux/kernel.h>

struct mm_region sprd_mem_map[CONFIG_NR_DRAM_BANKS + 2] = {
	{
		/* MMIO / peripherals: 0x0 - 0x8000_0000 */
		.virt = 0x00000000UL,
		.phys = 0x00000000UL,
		.size = 0x80000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN,
	}
	/* DRAM entry/entries and the terminator are filled in at runtime
	 * by mem_map_from_dram_banks() - see mach-sprd's enable_caches().
	 */
};

struct mm_region *mem_map = sprd_mem_map;
const unsigned int sprd_mem_map_count = ARRAY_SIZE(sprd_mem_map);
