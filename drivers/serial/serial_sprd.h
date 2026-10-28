/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Register layout for the Unisoc/Spreadtrum UART IP (sc9836/sc9860/
 * ums512/sharkl family), taken from the vendor 2015.07 U-Boot tree
 * (drivers/serial/serial_sprd.h).
 */

#ifndef __SERIAL_SPRD_H
#define __SERIAL_SPRD_H

struct sprd_uart_regs {
	u32	txd;		/* 0x00 */
	u32	rxd;		/* 0x04 */
	u32	status0;	/* 0x08 */
	u32	status1;	/* 0x0c: rx fifo cnt [3:0], tx fifo cnt [8:4] */
	u32	int_en;		/* 0x10 */
	u32	int_clr;	/* 0x14 */
	u32	ctrl0;		/* 0x18 */
	u32	ctrl1;		/* 0x1c */
	u32	ctrl2;		/* 0x20 */
	u32	clkd0;		/* 0x24: clock divisor */
	u32	reserved;	/* 0x28 */
	u32	status2;	/* 0x2c */
	u32	dsp_wait;	/* 0x30 */
	u32	sw_flow_ctrl;	/* 0x34 */
	u32	xcmd;		/* 0x38 */
	u32	escape;		/* 0x3c */
};

/*
 * These match the exact masks used by the vendor driver's getc/putc/
 * pending implementations (not the RX_FIFO_CNT_MSK/TX_FIFO_CNT_MSK
 * documentation macros elsewhere in the vendor tree, which are unused
 * by the actual code there).
 */
#define SPRD_UART_STS1_RXF_CNT_MASK		0xff
#define SPRD_UART_STS1_TXF_CNT_SHIFT		8
#define SPRD_UART_STS1_TXF_CNT_MASK		0x1ff

#endif /* __SERIAL_SPRD_H */
