// SPDX-License-Identifier: GPL-2.0+
/*
 * Unisoc/Spreadtrum sharkl5pro (UMS512) MUSB "glue layer" - gadget-only.
 *
 * This is a brand-new, standards-conformant MUSB platform glue for the
 * ADT1061 (UMS512/sharkl5pro) port. It deliberately does NOT reuse the
 * vendor adt1061_u-boot 2015.07 tree's drivers/usb/musb-new/
 * sprd_musb2_driver.c: that file registers a musb_hdrc_platform_data
 * with musb_register() too, but its actual "USB driver" is Spreadtrum's
 * own proprietary bare-metal FDL (flash-download/diagnostic) protocol -
 * hand-rolled USB descriptors and a manual EP0 setup-packet handler
 * poking Mentor_USB_CSR0L/EP0_FIFO registers directly, with its
 * musb_platform_ops.enable/.disable left as empty no-op stubs and the
 * standard gadget endpoint-request-queue API never actually used. None
 * of that is reusable for Android Fastboot, which needs a conformant
 * glue that lets upstream's generic musb_gadget.c/musb_core.c run the
 * real USB descriptor/endpoint machinery, with f_fastboot.c on top.
 *
 * The one genuinely reusable piece of vendor USB code is the PHY
 * bring-up register sequence from drivers/usb/musb-new/
 * sharkl5pro_usb_phy.c's usb_phy_init() - reproduced below as
 * sprd_usb_phy_init(), with the exact same AON_APB/ANLG_PHY_G2 register
 * writes (addresses/bits taken verbatim from the vendor's
 * arch/arm/include/asm/arch-sharkl5pro/chip_sharkl5pro/{aon_apb.h,
 * anlg_phy_g2.h}), now called from a real musb_platform_ops.init().
 *
 * This glue is registered the same (non-DM) way the vendor tree did it -
 * musb_register(&plat, NULL, (void *)SPRD_USB_BASE) from board_usb_init()
 * - which is still a fully supported path in upstream U-Boot for
 * CONFIG_USB_MUSB_GADGET + !CONFIG_IS_ENABLED(DM_USB_GADGET) (see
 * drivers/usb/musb-new/musb_uboot.c). No board/ C file exists for this
 * target (it's DT+arch/arm/mach-sprd only), so board_usb_init() lives
 * here instead, next to the controller it brings up.
 *
 * musb_hdrc_config (num_eps/ram_bits/multipoint/dyn_fifo) mirrors the
 * vendor's own sprd_musb static config in sprd_musb2_driver.c, since
 * those fields describe the real sharkl5pro MUSB hardware instance
 * (7 endpoints incl. EP0, 2KB-granularity dynamic FIFO) rather than
 * anything protocol-specific.
 */

#include <log.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/usb/musb.h>
#include <asm/io.h>
#include <usb.h>
#include "linux-compat.h"
#include "musb_core.h"
#include "musb_uboot.h"
#ifdef CONFIG_USB_SPRD_DMA
#include "sprd_musbhsdma.h"
#endif

/* ---- MUSB controller physical base (chip_sharkl5pro/hardware.h) ---- */
#define SPRD_USB_BASE			0x5FFF0000

/* ---- AON_APB block (chip_sharkl5pro/aon_apb.h) ---- */
#define CTL_AON_APB_BASE		0x327D0000
#define REG_AON_APB_APB_EB1		(CTL_AON_APB_BASE + 0x0004)
#define REG_AON_APB_APB_RST1		(CTL_AON_APB_BASE + 0x0010)
#define REG_AON_APB_OTG_PHY_TEST	(CTL_AON_APB_BASE + 0x0204)
#define REG_AON_APB_OTG_PHY_CTRL	(CTL_AON_APB_BASE + 0x0208)

#define BIT_AON_APB_ANA_EB			BIT(12)
#define BIT_AON_APB_OTG_UTMI_EB		BIT(8)	/* aka vendor's BIT_USB_EB */
#define BIT_AON_APB_OTG_PHY_SOFT_RST		BIT(9)
#define BIT_AON_APB_OTG_UTMI_SOFT_RST		BIT(8)
#define BIT_AON_APB_OTG_VBUS_VALID_PHYREG	BIT(24)
#define BIT_AON_APB_UTMI_WIDTH_SEL		BIT(30)

/* ---- Analog PHY group2: USB2.0 UTMI control (chip_sharkl5pro/anlg_phy_g2.h) ---- */
#define CTL_ANLG_PHY_G2_BASE				0x323B0000
#define REG_ANLG_PHY_G2_ANALOG_USB20_USB20_UTMI_CTL1	(CTL_ANLG_PHY_G2_BASE + 0x0058)

#define BIT_ANLG_PHY_G2_ANALOG_USB20_USB20_DATABUS16_8	BIT(28)
#define BIT_ANLG_PHY_G2_ANALOG_USB20_USB20_VBUSVLDEXT	BIT(16)

static inline void sprd_reg_or(u32 addr, u32 bits)
{
	writel(readl((void __iomem *)(uintptr_t)addr) | bits,
	       (void __iomem *)(uintptr_t)addr);
}

static inline void sprd_reg_and(u32 addr, u32 bits)
{
	writel(readl((void __iomem *)(uintptr_t)addr) & bits,
	       (void __iomem *)(uintptr_t)addr);
}

/*
 * Verbatim port of vendor's sharkl5pro_usb_phy.c usb_phy_init(): enable
 * the USB module + analog block, force VBUS valid, select 16-bit UTMI
 * bus width, then soft-reset the PHY and UTMI interface.
 */
static void sprd_usb_phy_init(void)
{
	/* enable usb module */
	sprd_reg_or(REG_AON_APB_APB_EB1,
		    BIT_AON_APB_OTG_UTMI_EB | BIT_AON_APB_ANA_EB);

	/* PHY bus valid */
	sprd_reg_or(REG_AON_APB_OTG_PHY_TEST,
		    BIT_AON_APB_OTG_VBUS_VALID_PHYREG);
	sprd_reg_or(REG_ANLG_PHY_G2_ANALOG_USB20_USB20_UTMI_CTL1,
		    BIT_ANLG_PHY_G2_ANALOG_USB20_USB20_VBUSVLDEXT);

	/* PHY width: 16 bit */
	sprd_reg_or(REG_AON_APB_OTG_PHY_CTRL, BIT_AON_APB_UTMI_WIDTH_SEL);
	sprd_reg_or(REG_ANLG_PHY_G2_ANALOG_USB20_USB20_UTMI_CTL1,
		    BIT_ANLG_PHY_G2_ANALOG_USB20_USB20_DATABUS16_8);

	/* soft reset PHY */
	sprd_reg_or(REG_AON_APB_APB_RST1,
		    BIT_AON_APB_OTG_PHY_SOFT_RST |
		    BIT_AON_APB_OTG_UTMI_SOFT_RST);
	udelay(5000);
	sprd_reg_and(REG_AON_APB_APB_RST1,
		     (u32)~(BIT_AON_APB_OTG_PHY_SOFT_RST |
			    BIT_AON_APB_OTG_UTMI_SOFT_RST));
}

/*
 * musb_core.c's own "generic_interrupt" (the obvious name for a plain
 * register-polling ISR body) only exists for OMAP2430/OMAP3430/U8500
 * (#if defined(CONFIG_SOC_OMAP2430) || ... #else #define
 * generic_interrupt NULL #endif) - on every other platform, including
 * ours, it is literally the macro NULL, and each platform glue is
 * expected to set musb->isr to its own equivalent from its .init()
 * callback (compare da8xx.c's "musb->isr = da8xx_musb_interrupt;" and
 * pic32.c's "musb->isr = pic32_interrupt;" inside their own
 * platform_ops.init()).
 *
 * sprd_musb_init() below didn't do this, so musb->isr stayed NULL
 * after musb_platform_init() returned, and musb_init_controller()'s
 * "if (!musb->isr) { status = -ENODEV; ... }" check failed every time
 * (confirmed on real ADT1061 hardware via temporary debug prints:
 * musb->isr was genuinely 0x0 going into musb_platform_init() with
 * generic_interrupt itself resolving to NULL, not merely unassigned).
 *
 * This board has no real IRQ line wired up for USB at all (U-Boot's
 * MUSB gadget mode doesn't use one either - drivers/usb/musb-new/
 * musb_uboot.c's dm_usb_gadget_handle_interrupts() just calls
 * musb->isr(0, musb) directly from a software polling loop), so the
 * body below is the exact same register-polling logic as the
 * unavailable generic_interrupt(), just under our own name so it
 * compiles on sharkl5pro too.
 */
static irqreturn_t sprd_musb_interrupt(int irq, void *__hci)
{
	struct musb *musb = __hci;
	irqreturn_t retval = IRQ_NONE;
#ifdef CONFIG_USB_SPRD_DMA
	u32 reg_dma;
#endif

	musb->int_usb = musb_readb(musb->mregs, MUSB_INTRUSB);
	musb->int_tx = musb_readw(musb->mregs, MUSB_INTRTX);
	musb->int_rx = musb_readw(musb->mregs, MUSB_INTRRX);
#ifdef CONFIG_USB_SPRD_DMA
	musb->int_listend = musb_readw(musb->mregs, MUSB_LISTEND_INT_STS);
	reg_dma = musb_readl(musb->mregs, MUSB_DMA_INTR_MASK_STATUS);

	/*
	 * This controller's bulk endpoints run through the vendor's own
	 * descriptor-linklist DMA engine (sprd_musbhsdma.c), not PIO - see
	 * the comment in sprd_musbhsdma.h. A pending DMA completion takes
	 * priority: musb_interrupt() must not run the normal TX/RX
	 * endpoint-service path on a transfer that DMA, not the CPU, is
	 * still moving.
	 *
	 * NOTE: this used to only look at int_usb and ep0's tx bit, which
	 * silently dropped every RX/TX interrupt from any endpoint above
	 * ep0 (e.g. the fastboot bulk ep1) whenever it fired on its own -
	 * musb_g_rx()/rxstate() (and so sprd_dma_channel_program()) never
	 * got called, so an incoming bulk OUT packet would just sit in the
	 * FIFO forever with nothing arming the DMA engine to pull it out.
	 * Mirror the plain (non-DMA) branch below and look at all three.
	 */
	if (musb->int_usb || musb->int_tx || musb->int_rx) {
		if (!reg_dma)
			retval = musb_interrupt(musb);
	}
	if (reg_dma)
		retval = sprd_dma_interrupt(musb, reg_dma);
#else
	if (musb->int_usb || musb->int_tx || musb->int_rx)
		retval = musb_interrupt(musb);
#endif

	return retval;
}

static int sprd_musb_init(struct musb *musb)
{
	sprd_usb_phy_init();
	musb->isr = sprd_musb_interrupt;

	return 0;
}

static int sprd_musb_enable(struct musb *musb)
{
	return 0;
}

static void sprd_musb_disable(struct musb *musb)
{
}

static const struct musb_platform_ops sprd_musb_ops = {
	.init		= sprd_musb_init,
	.enable		= sprd_musb_enable,
	.disable	= sprd_musb_disable,
};

/*
 * EP0 + 6 data endpoints, single-buffered 512-byte FIFOs.
 *
 * musb_core.c's fifo_setup() enforces a hard RAM budget of
 * 1 << (ram_bits + 2) bytes (see its "offset + (maxpacket << dpb) >
 * budget -> -EMSGSIZE" check) - with ram_bits = 11 that is 8192 bytes.
 * An earlier version of this table used MUSB_EP_FIFO_DOUBLE (2x the
 * bytes per endpoint) for all 6 endpoints, which needs 12288 bytes
 * and blows that budget well before reaching ep 6 - this was never
 * exercised on real hardware until the musb->isr fix above let probing
 * get this far. Single-buffered 512 bytes * 2 directions * 6 endpoints
 * = 6144 bytes, comfortably inside the 8192 byte budget (plus ep0's
 * fixed 64 bytes). This also matches the vendor sprd_musb2_driver.c's
 * own fifo table for this exact hardware (MUSB_EP_FIFO_SINGLE, not
 * _DOUBLE, whenever CONFIG_USB_PHY_SHARKL5 - a different, older chip
 * - is not defined, which is our case).
 */
static struct musb_fifo_cfg sprd_musb_fifo_cfg[] = {
	MUSB_EP_FIFO_SINGLE(1, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(1, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(2, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(2, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(3, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(3, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(4, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(4, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(5, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(5, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(6, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(6, FIFO_RX, 512),
};

static struct musb_hdrc_config sprd_musb_config = {
	.fifo_cfg	= sprd_musb_fifo_cfg,
	.fifo_cfg_size	= ARRAY_SIZE(sprd_musb_fifo_cfg),
	.multipoint	= 1,
	.dyn_fifo	= 1,
	.num_eps	= 7,
	.ram_bits	= 11,
};

static struct musb_hdrc_platform_data sprd_musb_plat = {
	.mode		= MUSB_PERIPHERAL,
	.config		= &sprd_musb_config,
	.power		= 250,
	.platform_ops	= &sprd_musb_ops,
};

int board_usb_init(int index, enum usb_init_type init)
{
	struct musb *musb;

	if (init != USB_INIT_DEVICE)
		return -ENOTSUPP;

	musb = musb_register(&sprd_musb_plat, NULL, (void *)SPRD_USB_BASE);
	if (IS_ERR_OR_NULL(musb)) {
		pr_err("sprd musb register failed\n");
		return -ENODEV;
	}

	return 0;
}

int board_usb_cleanup(int index, enum usb_init_type init)
{
	return 0;
}
