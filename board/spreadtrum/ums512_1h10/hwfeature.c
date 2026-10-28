// SPDX-License-Identifier: GPL-2.0+
/*
 * UMS512/sharkl5pro chip-variant ("bonding option") detection, ported
 * from stock U-Boot's arch/armv8/sharkl5pro/misc.c get_chip_bonding()
 * and the androidboot.auto.efuse=/auto.chipid=/lwfq.type= cmdline
 * properties stock derives from it (confirmed against a string found
 * in a disassembled stock u-boot-sign.bin:
 * "auto.efuse=auto.efuse,1:T610,:T618" - i.e. bonding code 1 is
 * reported as "T610", and *every other* code - including the T700
 * pattern below - is folded into "T618"). This mirrors that same
 * folding rather than inventing a third "T700" string stock itself
 * never emits, since kernel-side consumers of /hwfeature (DVFS
 * tables, the sprd-gpu-pd driver's own independent copy of this same
 * read) only ever look for "T610" or "T618".
 *
 * AON_APB_BOND_OPT0's bit7/16/18/20/21 pattern per variant, exactly
 * as given:
 *   T610: bit16=1 bit18=1 bit20=1 bit21=0 bit7=0
 *   T618: bit16=0 bit18=0 bit20=0 bit21=0
 *   T700: bit16=1 bit18=0 bit20=0 bit21=0
 *
 * chipid ("UMS512-AA"/"-AB"/"-AC") comes from a *different* efuse
 * register, AON_APB_AON_VER_ID (0x327d00f8) - already read elsewhere
 * in this board port for an unrelated purpose, vddsram's silicon-
 * revision-dependent target voltage (see sprd_regulator_voltage_init()
 * in ums512_1h10.c, UMS512_AON_APB_AON_VER_ID). That existing code
 * only distinguishes raw values 0x00/0x01 (AA/AB voltage tuning); this
 * file re-reads the same physical register for the separate "UMS512-AA/
 * AB/AC" chipid string stock's auto/chipid property reports, which
 * additionally recognizes raw value 3 as "AC" (anything else, AA).
 */

#include <env.h>
#include <log.h>
#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/kernel.h>

#include <fdt_support.h>
#include <linux/libfdt.h>

#include "hwfeature.h"

#define UMS512_AON_APB_BOND_OPT0	0x327d083cUL
#define BOND_OPT0_BIT(n)		BIT(n)

#define UMS512_AON_APB_AON_VER_ID	0x327d00f8UL
#define AON_VER_ID_MASK			0xffU
#define AON_VER_ID_AB			1U
#define AON_VER_ID_AC			3U

enum adt1061_hw_variant {
	ADT1061_HW_VARIANT_T618 = 0,
	ADT1061_HW_VARIANT_T610 = 1,
	ADT1061_HW_VARIANT_T700 = 2,	/* diagnostic only - reported as T618 */
};

static enum adt1061_hw_variant s_variant;
static u32 s_bond_opt0;
static const char *s_chipid;
static bool s_detected;

static enum adt1061_hw_variant adt1061_decode_bond_opt0(u32 v)
{
	bool b7  = !!(v & BOND_OPT0_BIT(7));
	bool b16 = !!(v & BOND_OPT0_BIT(16));
	bool b18 = !!(v & BOND_OPT0_BIT(18));
	bool b20 = !!(v & BOND_OPT0_BIT(20));
	bool b21 = !!(v & BOND_OPT0_BIT(21));

	if (b16 && b18 && b20 && !b21 && !b7)
		return ADT1061_HW_VARIANT_T610;
	if (b16 && !b18 && !b20 && !b21)
		return ADT1061_HW_VARIANT_T700;
	return ADT1061_HW_VARIANT_T618;
}

static const char *adt1061_decode_chipid(void)
{
	u32 ver = readl((void __iomem *)(ulong)UMS512_AON_APB_AON_VER_ID) &
		  AON_VER_ID_MASK;

	if (ver == AON_VER_ID_AB)
		return "UMS512-AB";
	if (ver == AON_VER_ID_AC)
		return "UMS512-AC";
	return "UMS512-AA";
}

void adt1061_hwfeature_detect(void)
{
	static const char * const variant_name[] = {
		[ADT1061_HW_VARIANT_T618] = "T618",
		[ADT1061_HW_VARIANT_T610] = "T610",
		[ADT1061_HW_VARIANT_T700] = "T700",
	};

	if (s_detected)
		return;

	s_bond_opt0 = readl((void __iomem *)(ulong)UMS512_AON_APB_BOND_OPT0);
	s_variant = adt1061_decode_bond_opt0(s_bond_opt0);
	s_chipid = adt1061_decode_chipid();
	s_detected = true;

	/*
	 * Same layout the user asked to be able to read off the log:
	 * raw register, decoded bonding code, and the real (possibly
	 * T700-but-reported-as-T618) variant name.
	 */
	printf("hwfeature: BOND_OPT0 = 0x%08x -> code %d -> \"%s\"%s\n",
	       s_bond_opt0, (int)s_variant, variant_name[s_variant],
	       s_variant == ADT1061_HW_VARIANT_T700 ?
	       " (reported to Android as T618, same as stock)" : "");
	printf("hwfeature: chipid = \"%s\"\n", s_chipid);
}

static const char *adt1061_hwfeature_efuse_str(void)
{
	return s_variant == ADT1061_HW_VARIANT_T610 ? "T610" : "T618";
}

static const char *adt1061_hwfeature_lwfq_type_str(void)
{
	return s_variant == ADT1061_HW_VARIANT_T610 ? "1" : "0";
}

void adt1061_hwfeature_set_bootargs(void)
{
	char buf[160];

	adt1061_hwfeature_detect();

	snprintf(buf, sizeof(buf),
		 "androidboot.auto.efuse=%s androidboot.auto.chipid=%s androidboot.lwfq.type=%s",
		 adt1061_hwfeature_efuse_str(), s_chipid,
		 adt1061_hwfeature_lwfq_type_str());
	env_set("hwfeature_bootargs", buf);
}

int adt1061_hwfeature_fixup_fdt(void *blob)
{
	int root, auto_node, lwfq_node;

	adt1061_hwfeature_detect();

	root = fdt_path_offset(blob, "/");
	if (root < 0)
		return root;

	auto_node = fdt_find_or_add_subnode(blob, root, "hwfeature");
	if (auto_node < 0)
		return auto_node;

	lwfq_node = auto_node;

	auto_node = fdt_find_or_add_subnode(blob, lwfq_node, "auto");
	if (auto_node < 0)
		return auto_node;

	fdt_setprop_string(blob, auto_node, "efuse", adt1061_hwfeature_efuse_str());
	fdt_setprop_string(blob, auto_node, "chipid", s_chipid);

	lwfq_node = fdt_find_or_add_subnode(blob, lwfq_node, "lwfq");
	if (lwfq_node < 0)
		return lwfq_node;

	fdt_setprop_string(blob, lwfq_node, "type", adt1061_hwfeature_lwfq_type_str());

	printf("hwfeature: /hwfeature/auto efuse=\"%s\" chipid=\"%s\", /hwfeature/lwfq type=\"%s\" written to devicetree\n",
	       adt1061_hwfeature_efuse_str(), s_chipid,
	       adt1061_hwfeature_lwfq_type_str());

	return 0;
}
