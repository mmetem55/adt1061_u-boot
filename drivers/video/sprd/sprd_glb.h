/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Small sci_glb_*() compat shim + the handful of Unisoc sharkl5pro
 * global AHB/PMU/clock register addresses needed by the ported
 * drivers/video/sprd/sharkl5pro/global_{dispc,dsi,dphy}.c glue files.
 *
 * The vendor adt1061_u-boot 2015.07 tree has a whole legacy
 * "sci_glb_*" global-register framework (drivers/misc/sprd_glb.c +
 * include/sprd_glb.h, plus per-SoC <asm/arch/sprd_reg.h> headers with
 * hundreds of REG_* and BIT_* macros for every IP block on the chip). That
 * framework itself is tiny - sci_glb_read/write/set/clr are nothing
 * more than readl/writel wrappers with no locking or clock-framework
 * dependency (see the vendor's drivers/misc/sprd_glb.c) - so rather
 * than porting the whole per-SoC register-header framework (which
 * covers IP blocks far outside the scope of this port), this
 * reimplements just those four helpers plus the exact register
 * addresses the three global_dispc.c/global_dsi.c/global_dphy.c glue
 * files above actually use, taken verbatim from the vendor's
 * arch/arm/include/asm/arch-sharkl5pro/chip_sharkl5pro/{hardware.h,
 * ap_ahb.h, pmu_apb.h, ap_clk_core.h, anlg_phy_g1.h}.
 */

#ifndef _SPRD_GLB_COMPAT_H_
#define _SPRD_GLB_COMPAT_H_

#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/types.h>

/* ---- DPU / DSI controller physical base addresses ---- */
#define SPRD_DPU_PHYS				0x20300000
#define SPRD_DSI_PHYS				0x20400000

/* ---- AP AHB block enable/reset/clock-gate registers ---- */
#define REG_AP_AHB_AHB_EB			0x20100000
#define BIT_AP_AHB_DISPC_EB			BIT(1)
#define BIT_AP_AHB_DSI_EB			BIT(0)

#define REG_AP_AHB_AHB_RST			0x20100004
#define BIT_AP_AHB_DISPC_SOFT_RST		BIT(1)
#define BIT_AP_AHB_DSI_SOFT_RST		BIT(0)

#define REG_AP_AHB_MISC_CKG_EN			0x20100040
#define BIT_AP_AHB_DPHY_REF_CKG_EN		BIT(1)
#define BIT_AP_AHB_DPHY_CFG_CKG_EN		BIT(0)

/* ---- PMU analog-PHY power-down control ---- */
#define REG_PMU_APB_ANALOG_PHY_PD_CFG		0x327E035C
#define BIT_PMU_APB_DSI_PD_REG			BIT(3)

/* ---- AP clock-core DISPC clock mux/div registers ---- */
#define REG_AP_CLK_CORE_CGM_DISPC0_CFG		0x2020009C
#define REG_AP_CLK_CORE_CGM_DISPC0_DPI_CFG	0x202000A0

/* ---- Analog PHY group1: MIPI-DSI 4-lane ISO switch ---- */
#define REG_ANLG_PHY_G1_ANALOG_MIPI_DSI_4LANE_DSI_ISO_SW	0x323A0034

static inline u32 sci_glb_read(u32 reg, u32 msk)
{
	return readl((void __iomem *)(uintptr_t)reg) & msk;
}

static inline int sci_glb_write(u32 reg, u32 val, u32 msk)
{
	void __iomem *addr = (void __iomem *)(uintptr_t)reg;

	writel((readl(addr) & ~msk) | val, addr);

	return 0;
}

static inline int sci_glb_set(u32 reg, u32 bit)
{
	void __iomem *addr = (void __iomem *)(uintptr_t)reg;

	writel(readl(addr) | bit, addr);

	return 0;
}

static inline int sci_glb_clr(u32 reg, u32 bit)
{
	void __iomem *addr = (void __iomem *)(uintptr_t)reg;

	writel(readl(addr) & ~bit, addr);

	return 0;
}

#endif /* _SPRD_GLB_COMPAT_H_ */
