// SPDX-License-Identifier: GPL-2.0+
/*
 * Unisoc/Spreadtrum sharkl5pro DM_VIDEO top-level glue.
 *
 * The vendor adt1061_u-boot 2015.07 tree's own top-level glue
 * (drivers/video/sprd/sprd_main.c) is written against U-Boot's old
 * pre-driver-model "legacy LCD" framework (lcd_ctrl_init(),
 * lcd_enable(), the global panel_info.vl_row/vl_col/vl_bpix from
 * common/lcd.c, BMP/gzipped-logo loading from a raw flash partition,
 * ...) which no longer exists in current upstream U-Boot (DM_VIDEO/
 * UCLASS_VIDEO only) - so none of that file is reused. What *is*
 * reused, nearly verbatim, is everything underneath it: the
 * sprd_panel_probe()/sprd_dispc_probe() entry points (sprd_panel.c,
 * sprd_dispc.c) that detect the panel, bring up the DSI/D-PHY/DPU
 * hardware and leave it free-running (the dummy panel uses DPI
 * continuous-streaming mode, not per-frame command-mode flips), and
 * sprd_dispc_flip()'s hardware-layer configuration (sprd_dispc.c ->
 * dpu_r4p0.c's dpu_layer()) to point the one hardware layer at the
 * DM_VIDEO-allocated framebuffer. This file is the only genuinely new
 * code in the whole display-stack port: it is the minimal DM_VIDEO
 * driver needed to request a framebuffer from the uclass and kick
 * off that existing probe chain.
 *
 * The /lcds node in adt1061.dts (arch/arm/dts/aidata-lcd.dtsi) lists
 * six real candidate panels this board was built with (vendor
 * hardware revisions sharing no reliable ID strap - see
 * sprd_panel.c's force-attach path), all of them WUXGA (1920x1200 /
 * 1200x1920) - *not* the 720x1280 "dummy" placeholder panel this
 * constant was originally sized for before the real panel list was
 * ported in. The framebuffer size requested in bind() is a fixed,
 * generously-rounded constant rather than something read back from DT
 * at bind time, so it must always be >= the real panel geometry read
 * from /lcds by sprd_panel_probe() in probe() - undersizing it here
 * does not fail cleanly, it lets sprd_video_flip_fb() write past the
 * allocated framebuffer into whatever follows it in DRAM (confirmed
 * on real hardware: a synchronous abort / translation fault once the
 * write crossed the 4 GiB physical-address boundary just above the
 * framebuffer).
 */

#include <dm.h>
#include <log.h>
#include <malloc.h>
#include <video.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <dm/device_compat.h>
#include "sprd_dispc.h"
#include "sprd_panel.h"

/* 1920x1200 (WUXGA) @ 32bpp (ARGB8888/XRGB8888), rounded up to a 4K page. */
#define SPRD_VIDEO_FB_SIZE	ALIGN(1920 * 1200 * 4, SZ_4K)

static int sprd_video_bind(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);

	plat->size = SPRD_VIDEO_FB_SIZE;
	plat->align = SZ_1M;

	return 0;
}

static int sprd_video_flip_fb(ulong fb_addr, struct panel_info *panel)
{
	struct sprd_restruct_config *config;
	int ret;

	config = malloc(sizeof(*config) + sizeof(config->hwlayers[0]));
	if (!config)
		return -ENOMEM;

	memset(config, 0, sizeof(*config) + sizeof(config->hwlayers[0]));

	config->number_hwlayer = 1;
	config->hwlayers[0].hwlayer_id = 0;
	config->hwlayers[0].n_planes = 1;
	config->hwlayers[0].iova_plane[0] = fb_addr;
	config->hwlayers[0].pitch[0] = panel->width * 4;
	config->hwlayers[0].alpha = 0xff;
	config->hwlayers[0].dst_x = 0;
	config->hwlayers[0].dst_y = 0;
	config->hwlayers[0].dst_w = panel->width;
	config->hwlayers[0].dst_h = panel->height;
	config->hwlayers[0].start_x = 0;
	config->hwlayers[0].start_y = 0;
	config->hwlayers[0].start_w = 0;
	config->hwlayers[0].start_h = 0;
	config->hwlayers[0].blending = HWC_BLENDING_NONE;
	config->hwlayers[0].compression = 0;
	config->hwlayers[0].format = DRM_FORMAT_XRGB8888;

	ret = sprd_dispc_flip(&dispc_device, config);

	free(config);

	return ret;
}

static int sprd_video_probe(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	struct panel_info *panel;
	int ret;

	ret = sprd_panel_probe();
	if (ret) {
		dev_err(dev, "no SPRD MIPI panel attached (%d)\n", ret);
		return -ENODEV;
	}

	ret = sprd_dispc_probe();
	if (ret) {
		dev_err(dev, "dpu/dispc init failed (%d)\n", ret);
		return -EIO;
	}

	panel = panel_info_attach();
	if (!panel) {
		dev_err(dev, "no panel info after probe\n");
		return -ENODEV;
	}

	uc_priv->xsize = panel->width;
	uc_priv->ysize = panel->height;
	uc_priv->bpix = VIDEO_BPP32;
	uc_priv->format = VIDEO_X8R8G8B8;
	uc_priv->line_length = panel->width * 4;

	ret = sprd_video_flip_fb(plat->base, panel);
	if (ret) {
		dev_err(dev, "dpu flip failed (%d)\n", ret);
		return ret;
	}

	dev_info(dev, "SPRD panel '%s' %ux%u up (fb @ 0x%lx)\n",
		 panel->lcd_name, panel->width, panel->height, plat->base);

	return 0;
}

static const struct udevice_id sprd_video_ids[] = {
	{ .compatible = "sprd,sharkl5pro-dpu" },
	{ }
};

U_BOOT_DRIVER(video_sprd) = {
	.name	= "video_sprd",
	.id	= UCLASS_VIDEO,
	.of_match = sprd_video_ids,
	.bind	= sprd_video_bind,
	.probe	= sprd_video_probe,
};
