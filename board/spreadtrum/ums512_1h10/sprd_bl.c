// SPDX-License-Identifier: GPL-2.0+
/*
 * LCD backlight control for the Unisoc UMS512 (sharkl5pro) ADT1061 board.
 *
 * Ported nearly verbatim from the vendor adt1061_u-boot 2015.07 tree's
 * board/spreadtrum/ums512_1h10/sprd_bl.c. On this hardware the backlight
 * is a plain PWM output (channel 2) straight off the SoC's own PWM
 * controller - *not* anything driven through the panel's power-gpios
 * sequence (drivers/video/sprd/sprd_panel.c), and not an I2C backlight
 * chip either. The devicetree's "/backlight" node (adt1061.dts,
 * compatible "sprd,sharkl5pro-pwm-backlight") documents the same four
 * numbers used here (pwm-index=2, pwm-scale=3, pwm-en-bit=27,
 * pwm-en-reg=0x327d0008) but nothing actually parses that node in this
 * tree - same as the vendor source, where the registers below are
 * poked directly with fixed addresses rather than read back from DT.
 * Confirmed on real hardware: without this, the panel is genuinely
 * fully initialized (DSI/DPU running, framebuffer live) but the screen
 * stays completely black, because nothing ever turns the backlight on
 * - the vendor's own U-Boot only ever lights it via this function,
 * called from logo_display() (drivers/video/sprd/sprd_main.c) in a
 * splash-screen/charging-logo subsystem this tree does not carry over;
 * board_late_init() calls it directly instead once the panel is up.
 *
 * Register addresses below are taken from the vendor tree's
 * arch/arm/include/asm/arch-sharkl5pro/chip_sharkl5pro/{hardware.h,
 * aon_apb.h,aon_clk_core.h} (SPRD_PWM_PHYS=0x32260000,
 * CTL_AON_APB_BASE=0x327d0000, CTL_AON_CLK_CORE_BASE=0x32080200) -
 * there is no DM PWM driver bound to this controller in this tree, so
 * this pokes the hardware directly exactly as the vendor code does,
 * rather than going through <pwm.h>.
 */

#include <asm/io.h>
#include <linux/types.h>

/* SPRD_PWM_PHYS: one PWM channel's register block is 0x20 bytes, channel N at +N*0x20 */
#define SPRD_PWM_BASE		0x32260000
#define PWM_CHANNEL_SIZE	0x20

#define PWM_PRESCALE		0x0000
#define PWM_MOD			0x0004
#define PWM_DUTY		0x0008
#define PWM_DIV			0x000c
#define PWM_PAT_LOW		0x0010
#define PWM_PAT_HIGH		0x0014
#define PWM_ENABLE		0x0018

/* Backlight is wired to PWM channel 2 on this board (sprd,lcd-backlight pwm-index) */
#define BACKLIGHT_PWM_INDEX	2
#define BACKLIGHT_PWM_SCALE	0x3
#define PWM_MOD_MAX		0xff
#define PWM_REG_MSK		0xffff

/*
 * REG_AON_CLK_CORE_CGM_PWM0_CFG = CTL_AON_CLK_CORE_BASE + 0x0038; one
 * selector register per PWM channel, spaced 4 bytes apart starting
 * there - bit 0 selects the ext_26m clock source for that channel.
 */
#define REG_AON_CLK_CORE_CGM_PWM0_CFG	0x32080238

/*
 * REG_AON_APB_APB_EB2 = CTL_AON_APB_BASE + 0x0008 - the same AON APB
 * clock-enable register block used for CLK_GPIO_EB elsewhere in this
 * port (see drivers/gpio/sprd_gpio.c), bit (1 << (25 + index)) gates
 * PWM channel <index>'s own bus clock.
 */
#define REG_AON_APB_APB_EB2	0x327d0008

static inline void pwm_write(int index, u32 value, u32 reg)
{
	writel(value, (void __iomem *)(ulong)(SPRD_PWM_BASE + index * PWM_CHANNEL_SIZE + reg));
}

static inline void reg_set_bits(ulong addr, u32 bits)
{
	void __iomem *a = (void __iomem *)addr;

	writel(readl(a) | bits, a);
}

/*
 * brightness: 0 turns the backlight off; otherwise a PWM duty value in
 * [1, PWM_MOD_MAX] (255) - the vendor source's own BACKLIGHT_ON is 200.
 */
void set_backlight(u32 brightness)
{
	int index = BACKLIGHT_PWM_INDEX;

	/* ext_26m clock source select for this PWM channel */
	reg_set_bits(REG_AON_CLK_CORE_CGM_PWM0_CFG + index * 4, 0x1 << 0);
	/* enable this PWM channel's own AON APB bus clock gate */
	reg_set_bits(REG_AON_APB_APB_EB2, (1 << index) << 25);

	if (brightness == 0) {
		pwm_write(index, 0, PWM_ENABLE);
		printf("sprd backlight power off. pwm_index=%d brightness=%d\n",
		       index, brightness);
	} else {
		pwm_write(index, BACKLIGHT_PWM_SCALE, PWM_PRESCALE);
		pwm_write(index, PWM_MOD_MAX, PWM_MOD);
		pwm_write(index, brightness, PWM_DUTY);
		pwm_write(index, PWM_REG_MSK, PWM_PAT_LOW);
		pwm_write(index, PWM_REG_MSK, PWM_PAT_HIGH);
		pwm_write(index, 1, PWM_ENABLE);
		printf("sprd backlight power on. pwm_index=%d brightness=%d\n",
		       index, brightness);
	}
}
