// SPDX-License-Identifier: GPL-2.0
/*
 * sprd-musbhsdmac - Spreadtrum/Unisoc MUSB hardware-DMA glue layer.
 *
 * Copyright (c) 2016 Spreadtrum Co., Ltd.
 *
 * Ported verbatim (register layout and algorithm unchanged) from the
 * vendor adt1061_u-boot 2015.07 tree onto upstream musb_dma.h's
 * dma_controller/dma_channel API - see the comment in
 * sprd_musbhsdma.h for why this board needs real DMA instead of PIO
 * for its MUSB bulk endpoints.
 */

#include <cpu_func.h>
#include <linux/delay.h>
#include "musb_core.h"
#include "sprd_musbhsdma.h"

static struct linklist_node_s linklist_tx[MUSB_LINKLIST_NODES]
	__attribute__((aligned(64)));
static struct linklist_node_s linklist_rx[MUSB_LINKLIST_NODES]
	__attribute__((aligned(64)));

static void sprd_dma_channel_release(struct dma_channel *channel);

static void sprd_dma_controller_stop(struct sprd_musb_dma_controller
				      *controller)
{
	struct dma_channel *channel;
	u32 bit;

	if (controller->used_channels != 0) {
		pr_err("sprd musbhsdma: stopping DMA controller while channel active\n");

		for (bit = 0; bit < MUSB_DMA_CHANNELS; bit++) {
			if (controller->used_channels & (1 << bit)) {
				channel = &controller->channel[bit].channel;
				sprd_dma_channel_release(channel);

				if (!controller->used_channels)
					break;
			}
		}
	}
}

/* Allocate a DMA channel number */
static struct dma_channel *sprd_dma_channel_allocate(struct dma_controller *c,
						      struct musb_hw_ep *hw_ep,
						      u8 transmit)
{
	struct sprd_musb_dma_controller *controller = container_of(c,
			struct sprd_musb_dma_controller, controller);
	struct sprd_musb_dma_channel *musb_channel;
	struct dma_channel *channel;
	u8 bit;
	u16 csr;

	bit = hw_ep->epnum;

	if (transmit) {
		musb_channel = &controller->channel[bit];
		controller->used_channels |= (1 << bit);
		musb_channel->idx = bit;
		/* Configure DMA mode */
		csr = musb_readw(hw_ep->regs, MUSB_TXCSR);
		csr |= MUSB_TXCSR_DMAMODE | MUSB_TXCSR_DMAENAB |
			MUSB_TXCSR_AUTOSET;
		musb_writew(hw_ep->regs, MUSB_TXCSR, csr);
	} else {
		musb_channel = &controller->channel[bit + 15];
		controller->used_channels |= (1 << (bit + 15));
		musb_channel->idx = bit + 15;
		/* Configure DMA mode */
		csr = musb_readw(hw_ep->regs, MUSB_RXCSR);
		csr |= MUSB_RXCSR_DMAMODE | MUSB_RXCSR_DMAENAB |
			MUSB_RXCSR_AUTOCLEAR;
		musb_writew(hw_ep->regs, MUSB_RXCSR, csr);
	}

	musb_channel->controller = controller;
	musb_channel->ep_num = bit;
	musb_channel->transmit = transmit;
	channel = &musb_channel->channel;
	channel->private_data = musb_channel;
	channel->status = MUSB_DMA_STATUS_FREE;
	channel->max_len = 0xffff;

	/* Tx => mode 1; Rx => mode 0 */
	channel->desired_mode = transmit;
	channel->actual_len = 0;

	return channel;
}

static void sprd_dma_channel_release(struct dma_channel *channel)
{
	struct sprd_musb_dma_channel *musb_channel = channel->private_data;

	channel->actual_len = 0;

	musb_channel->controller->used_channels &=
		~(1 << musb_channel->idx);

	channel->status = MUSB_DMA_STATUS_UNKNOWN;
}

static void sprd_configure_channel(struct dma_channel *channel,
				    u8 transmit)
{
	struct sprd_musb_dma_channel *musb_channel = channel->private_data;
	struct sprd_musb_dma_controller *controller = musb_channel->controller;
	void __iomem *mbase = controller->base;
	u8 bchannel = musb_channel->idx;
	u32 csr;

	/*
	 * MUSB_DMA_CHN_INTR(n) packs the enable bits we want to set here
	 * (CHN_*_INT_EN) in the very same 32-bit word as the write-1-to-clear
	 * CHN_*_INT_CLR bits that sprd_dma_interrupt() just used, for this
	 * exact channel, to ack the PREVIOUS transfer's completion a few
	 * instructions ago (this function runs from inside
	 * sprd_musb_dma_completion(), called synchronously out of that same
	 * interrupt, to immediately re-arm the channel for a queued follow-up
	 * transfer - see the restart comment there).
	 *
	 * Confirmed on real hardware: a plain read-modify-write here (read
	 * the register, OR in the EN bit, write it back) intermittently hangs
	 * forever on the very next multi-line "fastboot getvar all" response,
	 * but ONLY once the CPU-bound delay of this driver's own debug
	 * printf()s is removed - i.e. this is a hardware synchronizer race,
	 * not a software ordering bug. The CLR bits are a write-only clear
	 * strobe; this SoC's internal synchronizer needs a handful of cycles
	 * before a just-written CLR bit stops reading back as still set. When
	 * we reconfigure the channel for the new transfer fast enough, our
	 * readback here can still see that stale CLR bit, and OR'ing it back
	 * into the register we write re-asserts the clear strobe a second
	 * time - right as the new transfer is starting. If that second pulse
	 * lands after the new transfer's own completion status bit has
	 * already been set, it silently wipes that status back out before
	 * sprd_dma_interrupt() ever gets to see it, and nothing is left to
	 * raise the interrupt that would otherwise notice the transfer
	 * finished. Always mask the CLR bits out of what we read back, so we
	 * only ever write the plain enable bits we actually intend to set.
	 */
	if (transmit) {
		/* enable linklist end interrupt */
		csr = musb_readl(mbase, MUSB_DMA_CHN_INTR(bchannel));
		csr &= ~(CHN_LLIST_INT_CLR | CHN_START_INT_CLR |
			 CHN_USBRX_LAST_INT_CLR);
		csr |= CHN_LLIST_INT_EN;
		musb_writel(mbase, MUSB_DMA_CHN_INTR(bchannel), csr);

		/* set linklist pointer */
		musb_writel(mbase, MUSB_DMA_CHN_LLIST_PTR(bchannel),
			    (u32)(uintptr_t)linklist_tx);

		/* enable channel and trigger the DMA transfer */
		csr = musb_readl(mbase, MUSB_DMA_CHN_CFG(bchannel));
		csr |= CHN_EN;
		musb_writel(mbase, MUSB_DMA_CHN_CFG(bchannel), csr);
	} else {
		/* enable linklist end and rx last interrupt */
		csr = musb_readl(mbase, MUSB_DMA_CHN_INTR(bchannel));
		csr &= ~(CHN_LLIST_INT_CLR | CHN_START_INT_CLR |
			 CHN_USBRX_LAST_INT_CLR);
		csr |= CHN_USBRX_INT_EN | CHN_LLIST_INT_EN;
		musb_writel(mbase, MUSB_DMA_CHN_INTR(bchannel), csr);

		/* set linklist pointer */
		musb_writel(mbase, MUSB_DMA_CHN_LLIST_PTR(bchannel),
			    (u32)(uintptr_t)linklist_rx);
		/* enable channel and trigger the DMA transfer */
		csr = musb_readl(mbase, MUSB_DMA_CHN_CFG(bchannel));
		csr |= CHN_EN;
		musb_writel(mbase, MUSB_DMA_CHN_CFG(bchannel), csr);
	}
}

static void prepare_linklist_node(struct dma_channel *channel, int index,
				   u32 list_end, dma_addr_t dma_addr, u32 len)
{
	struct sprd_musb_dma_channel *musb_channel = channel->private_data;
	struct linklist_node_s *node = musb_channel->transmit ?
		&linklist_tx[index] : &linklist_rx[index];

	node->frag_len = 32;
	node->blk_len = len;
	node->ioc = list_end;
	node->list_end = list_end;
	node->sp = 0;
	node->addr = (u32)dma_addr;
}

static void prepare_multi_linklist_node(struct dma_channel *channel,
					 dma_addr_t dma_addr, u32 len)
{
	struct sprd_musb_dma_channel *musb_channel = channel->private_data;
	u32 nodes, transfer_len, i, length;
	u32 buffer = (u32)dma_addr;
	unsigned long cache_start, cache_end;
	unsigned int total_len = 0;

	if (len % MUSB_MAX_BLOCK_LEN)
		nodes = len / MUSB_MAX_BLOCK_LEN + 1;
	else
		nodes = len / MUSB_MAX_BLOCK_LEN;
	if (!nodes)
		nodes = 1;

	channel->status = MUSB_DMA_STATUS_BUSY;
	channel->actual_len = 0;

	for (i = 0; i < nodes - 1; i++) {
		transfer_len = MUSB_MAX_BLOCK_LEN;
		prepare_linklist_node(channel, i, 0, buffer, transfer_len);
		buffer += MUSB_MAX_BLOCK_LEN;
		total_len += transfer_len;
	}
	transfer_len = len - total_len;
	prepare_linklist_node(channel, nodes - 1, 1, buffer, transfer_len);

	if (musb_channel->transmit) {
		length = sizeof(linklist_tx);
		cache_start = (unsigned long)linklist_tx;
		cache_end = cache_start + length;
		flush_dcache_range(cache_start, cache_end);
	} else {
		length = sizeof(linklist_rx);
		cache_start = (unsigned long)linklist_rx;
		cache_end = cache_start + length;
		invalidate_dcache_range(cache_start, cache_end);
	}
}

static int sprd_dma_channel_program(struct dma_channel *channel,
				     u16 packet_sz, u8 mode,
				     dma_addr_t dma_addr, u32 len)
{
	struct sprd_musb_dma_channel *musb_channel = channel->private_data;
	unsigned long cache_start, cache_end;
	u32 length;

	if (channel->status == MUSB_DMA_STATUS_UNKNOWN ||
	    channel->status == MUSB_DMA_STATUS_BUSY)
		return -EINVAL;

	if (len % 64)
		length = len - (len % 64) + 64;
	else
		length = len;
	cache_start = (unsigned long)dma_addr;
	cache_end = cache_start + length;

	if (musb_channel->transmit)
		flush_dcache_range(cache_start, cache_end);
	else
		invalidate_dcache_range(cache_start, cache_end);

	prepare_multi_linklist_node(channel, dma_addr, len);

	musb_channel->blk_len = len;
	musb_channel->ioc = 1;
	musb_channel->list_end = 1;
	musb_channel->sp = 0;
	musb_channel->addr = (u32)dma_addr;
	musb_channel->frag_len = 32;
	musb_channel->max_packet_sz = packet_sz;

	sprd_configure_channel(channel, musb_channel->transmit);

	/*
	 * musb_gadget.c's txstate()/rxstate() treat a nonzero return as
	 * "DMA was armed for this transfer, skip the PIO fallback" (see
	 * the CONFIG_USB_UX500_DMA/CONFIG_USB_TUSB_OMAP_DMA branches there
	 * for the same convention) - this controller's channel_program()
	 * never fails, so always report success.
	 */
	return 1;
}

static int sprd_dma_channel_abort(struct dma_channel *channel)
{
	struct sprd_musb_dma_channel *musb_channel = channel->private_data;
	struct sprd_musb_dma_controller *controller = musb_channel->controller;
	void __iomem *mbase = controller->base;
	u8 bchannel = musb_channel->idx;
	int offset;
	u16 csr;

	if (channel->status == MUSB_DMA_STATUS_BUSY) {
		if (musb_channel->transmit) {
			offset = 0x100 + musb_channel->ep_num * 0x10 +
				 MUSB_TXCSR;

			/*
			 * The programming guide says that we must clear
			 * the DMAENAB bit before the DMAMODE bit...
			 */
			csr = musb_readw(mbase, offset);
			csr &= ~(MUSB_TXCSR_AUTOSET | MUSB_TXCSR_DMAENAB);
			musb_writew(mbase, offset, csr);
			csr &= ~MUSB_TXCSR_DMAMODE;
			musb_writew(mbase, offset, csr);
		} else {
			offset = 0x100 + musb_channel->ep_num * 0x10 +
				 MUSB_RXCSR;

			csr = musb_readw(mbase, offset);
			csr &= ~(MUSB_RXCSR_AUTOCLEAR |
				 MUSB_RXCSR_DMAENAB |
				 MUSB_RXCSR_DMAMODE);
			musb_writew(mbase, offset, csr);
		}

		musb_writel(mbase, MUSB_DMA_CHN_LLIST_PTR(bchannel), 0);
		musb_write_dma_addr(mbase, bchannel, 0);
		channel->status = MUSB_DMA_STATUS_FREE;
	}

	return 0;
}

static void sprd_musb_dma_completion(struct musb *musb, u8 epnum, u8 transmit)
{
	struct musb_ep *musb_ep;
	struct musb_request *musb_req;
	struct usb_request *request;
	struct list_head *queue;
	u32 blk_len;

	if (transmit)
		musb_ep = &musb->endpoints[epnum].ep_in;
	else
		musb_ep = &musb->endpoints[epnum].ep_out;

	queue = &musb_ep->req_list;

	if (list_empty(queue))
		return;
	musb_req = container_of(queue->next, struct musb_request, list);
	request = &musb_req->request;

	if (!transmit) {
		blk_len = musb_readl(musb->mregs, MUSB_DMA_CHN_LEN(epnum + 15));
		blk_len = (blk_len & 0xffff0000) >> 16;
		request->actual = request->length - blk_len;
	} else {
		request->actual = request->length;
	}

	musb_ep->dma->status = MUSB_DMA_STATUS_FREE;
	musb_g_giveback(musb_ep, request, 0);

	/*
	 * musb_g_giveback() forces ep->busy=1 for the duration of the
	 * request->complete() callback it just made above, specifically so
	 * that a musb_gadget_queue() call made from *inside* that callback
	 * ("!musb_ep->busy && ..." check) does NOT call musb_ep_restart()
	 * there and then - it just appends to the list and waits for a
	 * later real TX/RX-complete interrupt to notice and start it, the
	 * way musb_g_tx()/musb_g_rx() do after an ordinary (non-DMA)
	 * completion.
	 *
	 * That's exactly what f_fastboot.c does on every single command:
	 * rx_handler_command() re-queues the OUT endpoint for the next
	 * command, and multiresponse_on_complete() (used by e.g.
	 * "getvar:all", which streams one INFO line per variable) queues
	 * the IN endpoint again for the next line - both from inside the
	 * completion callback we just invoked via musb_g_giveback() above.
	 *
	 * But this controller's DMA engine never raises that ordinary
	 * per-endpoint interrupt in the first place (confirmed: int_tx and
	 * int_rx read back 0 on every single one of these DMA-completion
	 * interrupts) - this function, called from its own completion
	 * interrupt, is the only place that ever notices anything finished.
	 * So if the callback above left a fresh request sitting unstarted
	 * at the head of the queue, nothing else is ever going to start it
	 * - kick it off right here instead.
	 */
	if (!list_empty(queue)) {
		struct musb_request *next_req = container_of(queue->next,
				struct musb_request, list);

		if (dma_channel_status(musb_ep->dma) != MUSB_DMA_STATUS_BUSY) {
			/*
			 * Empirically required on real hardware: this
			 * channel's completion path (ack the old transfer's
			 * CHN_LLIST_INT, then immediately reprogram the very
			 * same channel for the next queued transfer) is
			 * reliable whenever something slow happens to run in
			 * between - it ran perfectly over and over with debug
			 * printf()s on this path (UART output is slow enough
			 * to add tens to hundreds of microseconds here) - but
			 * hangs forever, intermittently, the instant that
			 * slack disappears (confirmed: still hangs even after
			 * masking the stale-CHN_LLIST_INT_CLR readback race in
			 * sprd_configure_channel(), so that wasn't the whole
			 * story). This channel's internal state machine isn't
			 * done settling back to idle the moment its completion
			 * status bit is visible to us; give it a short, fixed
			 * delay before touching its registers again instead of
			 * depending on incidental slowness elsewhere.
			 */
			udelay(100);
			musb_ep_restart(musb, next_req);
		}
	}
}

irqreturn_t sprd_dma_interrupt(struct musb *musb, u32 int_hsdma)
{
	void __iomem *mbase = musb->mregs;
	u8 bchannel = 0;
	u32 intr, int_dma;
	int i;

	int_dma = musb_readl(musb->mregs, MUSB_DMA_INTR_MASK_STATUS);
	for (i = 0; i < MUSB_DMA_CHANNELS; i++) {
		bchannel++;
		if ((int_hsdma & 1) != 1) {
			int_hsdma >>= 1;
			continue;
		}
		int_hsdma >>= 1;

		intr = musb_readl(mbase, MUSB_DMA_CHN_INTR(bchannel));

		if (intr & CHN_START_INT_MASK_STATUS) {
			/* clear interrupt */
			intr |= CHN_START_INT_CLR;
			musb_writel(mbase, MUSB_DMA_CHN_INTR(bchannel), intr);
		}
		if (bchannel > 15) {
			if (intr & CHN_LLIST_INT_MASK_STATUS) {
				/* clear interrupt */
				intr |= CHN_LLIST_INT_CLR |
					CHN_USBRX_LAST_INT_CLR;
				musb_writel(mbase, MUSB_DMA_CHN_INTR(bchannel),
					    intr);
				/* callback to give complete */
				sprd_musb_dma_completion(musb,
					(bchannel - 15), 0);
			}
		} else {
			/*
			 * Originally ported as "(musb->int_tx |
			 * musb->int_listend) & (1 << bchannel)" - but on
			 * real hardware neither of those ever gets set for
			 * a TX DMA completion (confirmed: int_tx=0000,
			 * int_listend=0000 on every single one of these
			 * interrupts), while this channel's own per-channel
			 * status register (intr, just read above) DOES show
			 * CHN_LLIST_INT_MASK_STATUS set. That old condition
			 * was never true, so the completion below never ran,
			 * the LLIST status bit in MUSB_DMA_CHN_INTR() never
			 * got cleared, MUSB_DMA_INTR_MASK_STATUS (reg_dma)
			 * stayed permanently nonzero, and this ISR re-fired
			 * on every single polling iteration forever - the
			 * UART spam the user saw with no transfer ever
			 * finishing. Mirror the RX branch above and check
			 * the channel's own status bit instead.
			 */
			if (intr & CHN_LLIST_INT_MASK_STATUS) {
				/* clear interrupt */
				intr |= CHN_LLIST_INT_CLR;
				musb_writel(mbase, MUSB_DMA_CHN_INTR(bchannel),
					    intr);
				/* callback to give complete */
				sprd_musb_dma_completion(musb, bchannel, 1);
			}
		}
	}
	(void)int_dma;

	return IRQ_HANDLED;
}

void dma_controller_destroy(struct dma_controller *c)
{
	struct sprd_musb_dma_controller *controller = container_of(c,
			struct sprd_musb_dma_controller, controller);

	sprd_dma_controller_stop(controller);

	kfree(controller);
}

struct dma_controller *dma_controller_create(struct musb *musb,
					      void __iomem *base)
{
	struct sprd_musb_dma_controller *controller;

	controller = kzalloc(sizeof(*controller), GFP_KERNEL);
	if (!controller)
		return NULL;

	controller->private_data = musb;
	controller->base = base;

	controller->controller.channel_alloc = sprd_dma_channel_allocate;
	controller->controller.channel_release = sprd_dma_channel_release;
	controller->controller.channel_program = sprd_dma_channel_program;
	controller->controller.channel_abort = sprd_dma_channel_abort;

	return &controller->controller;
}
