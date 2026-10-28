/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __ADT1061_HWFEATURE_H
#define __ADT1061_HWFEATURE_H

/*
 * Reads this one chip's BOND_OPT0/AON_VER_ID efuse bits once and
 * prints the result (same log line layout the user asked to see:
 * "sprd-gpu-pd: BOND_OPT0 = 0x........ -> code N -> \"T6xx\"" is the
 * GPU driver's own copy of this same read; this is the board-level
 * one that also feeds the kernel's /hwfeature devicetree node and
 * cmdline). Safe to call more than once - cheap, no side effects
 * beyond the printed line.
 *
 * Call before env_set()'ing "hwfeature_bootargs" (adt1061_hwfeature_set_bootargs())
 * and before ft_board_setup() applies the devicetree fixup
 * (adt1061_hwfeature_fixup_fdt()), so both agree with what was logged.
 */
void adt1061_hwfeature_detect(void);

/*
 * Sets the "hwfeature_bootargs" environment variable to the
 * androidboot.auto.efuse=/androidboot.auto.chipid=/androidboot.lwfq.type=
 * triplet CONFIG_BOOTCOMMAND folds into the kernel cmdline - the same
 * three keys stock U-Boot's own auto/efuse, auto/chipid and lwfq/type
 * /hwfeature properties are mirrored onto its cmdline as.
 */
void adt1061_hwfeature_set_bootargs(void);

/*
 * Adds the /hwfeature/auto (efuse, chipid) and /hwfeature/lwfq (type)
 * devicetree properties stock U-Boot adds, so kernel-side code that
 * reads /hwfeature directly (rather than only the cmdline) still
 * finds it. Returns 0 on success, or a negative libfdt error code.
 * Meant to be called from ft_board_setup().
 */
int adt1061_hwfeature_fixup_fdt(void *blob);

#endif /* __ADT1061_HWFEATURE_H */
