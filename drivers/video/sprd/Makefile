# SPDX-License-Identifier: GPL-2.0+
#
# Unisoc/Spreadtrum sharkl5pro display stack (DPU/DSI/D-PHY/panel),
# ported from the vendor adt1061_u-boot 2015.07 tree's
# drivers/video/sprd/. Only the backends this board actually uses are
# kept: DPU_R4P0, the mipi_dsi_r1p0 DSI host core, and the
# megacores_sharkl5 D-PHY PLL (see the vendor's legacy board header
# include/configs/ums512_1h10.h for CONFIG_DPU_R4P0/
# CONFIG_DSIH_SPRD_CTRL_RXP0/CONFIG_DPHY_SPRD_SHARKL5PRO, and the
# per-subsystem vendor Makefiles that map those to these exact files).
#
# subdir-ccflags-y mirrors the vendor tree's own build setup (same
# line, same purpose): every file below refers to its sibling headers
# by their vendor-relative name (e.g. "sprd_dsi.h", "dsi/mipi_dsi_api.h")
# regardless of which subdirectory it itself lives in, so the whole
# drivers/video/sprd/ tree needs to be on the include path.
subdir-ccflags-y += -I$(src)

obj-y += sprd_dispc.o sprd_dsi.o sprd_dphy.o sprd_panel.o sprd_video.o

obj-y += dispc/
obj-y += dsi/
obj-y += dphy/
obj-y += panel-if/
obj-y += sharkl5pro/
