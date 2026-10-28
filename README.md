# Aidata Alim 10 (ADT1061) Upstream U-Boot

Upstream Das U-Boot `2026.10` tabanı ve Vendor `2015.07` U-Boot kaynağı referans alınarak bu depo oluşturulmuştur ve test edilmiştir.

## Cihaz

| | |
|---|---|
| Model | Aidata ADT1061 tablet |
| SoC | Unisoc/Spreadtrum UMS512 (sharkl5pro) |
| CPU | ARM Cortex-A55, AArch64 |
| RAM | 3 GiB LPDDR4X, chip PS101-053BT |
| eMMC | Android GPT, 42 partition |
| Ekran | MIPI-DSI, tv101wum_ll2_wuxga, 1200×1920 |
| PMIC | Spreadtrum SC2730 |
| Şarj IC | BQ2560x |
| USB | Spreadtrum MUSB |
| Chip ID | 0x6B4C3550 0x53686172 0x00000003 |

## U-Boot portunun özellikleri

### Çekirdek platform
- `arch/arm/mach-sprd/`: `arch_cpu_init()`, `enable_caches()`, `reset_cpu()`
  (PSCI SYSTEM_RESET çalışmıyor, SC2730 PMIC watchdog üzerinden reset),
  `poweroff` komutu.

### Sürücüler
- Clock: `drivers/clk/sprd/`
- Pin-mux: `board/spreadtrum/ums512_1h10/pinmap.c`
- GPIO: `drivers/gpio/sprd_gpio.c`
- ADI/SPI bus: `drivers/spi/sprd_adi.c`
- PMIC + regülatörler, fuel gauge, RTC, watchdog, titreşim: `drivers/power/`
- I2C: `drivers/i2c/`
- Şarj: BQ2560x charger sürücüsü
- eMMC: `drivers/mmc/sprd_sdhci.c`, `CONFIG_EFI_PARTITION` ile GPT okuma
- Video: DPU/DSI/D-PHY MIPI yığını, `DM_VIDEO`, PWM arka ışık
- USB: MUSB gadget + SPRD donanım DMA
- Seri konsol: `SPRD_SERIAL`, uart1, 115200n8

### Açılış akışı / komutlar
- Standart distro boot: eMMC GPT'den boot partition'ını okuyup
  `abootimg`/`bootm` ile Android boot image başlatıyor, başarısız olursa
  Fastboot USB moduna düşüyor.
- `CONFIG_CMD_BCB` ile Boot Control Block desteği, `ABOOTIMG`/`ADTIMG` ile
  Android boot image ayrıştırma.
- Fastboot: `getvar`, `flash`, ürün/platform/seri env değişkenleri otomatik.
- Splash ekranı `logo` partition'ından okunuyor.
- `miscdata` partition'ından fabrika seri numarası okunuyor.
- `/hwfeature` devicetree düğümü, `BOND_OPT0` fuse değerinden çip
  varyantını otomatik tespit edip devicetree'ye yazıyor.
- Watchdog otomatik başlıyor, PMIC'in 39 adımlık başlatma tablosu
  uygulanıyor.

## Derleme

```sh
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
make adt1061_defconfig
make
```

veya otomatik script ile

```sh
./make.sh
```

`configs/adt1061_defconfig` üzerinde yapılan her değişiklik sonrası
`make adt1061_defconfig` tekrar çalıştırılmalıdır.

## Gerçek donanımda tam açılış - UART log

```
U-Boot 2026.10-g59e3bcfdd6ef-dirty (Oct 28 2026 - 18:08:41 +0300)

CPU:   Unisoc UMS512 (sharkl5pro)
Model: Aidata ADT1061 tablet
DRAM:  3 GiB
pmic stock-table: applied stock U-Boot's full 39-step PMIC table (DCXO_XTL_EN=0xd000 XTL_WAIT_CTRL0=0x3f32)
Core:  97 devices, 18 uclasses, devicetree: separate
WDT:   Started watchdog@40 with servicing every 1000ms (60s timeout)
MMC:   mmc@1400000: 0
Loading Environment from nowhere... OK
In:    serial
Out:   serial
Err:   serial
hwfeature: BOND_OPT0 = 0x00150421 -> code 1 -> "T610"
hwfeature: chipid = "UMS512-AC"
i2c_sprd i2c@70700000: sprd_i2c_of_to_plat: sprd_i2c-0: base: 0x70700000, clock_frequency: 400000
i2c_sprd i2c@70700000: i2c-0 base address: 0x70700000
i2c_sprd i2c@70700000: i2c-0 didn't have hardware channel, or didn't config it.
bq2560x_charger charger@6b: bq2560x charger enabled (ichg=2400mA, ilim=500mA, iprechg=360mA, iterm=360mA, vreg=4208mV)
sc2730_pmic pmic@0: fgu: using chip efuse calibration (4200mv_adc=2898)
[sprdfb][of_parse_panel] panel: hx8279_wd101fxm40ab_c7_wuxga_inx101
[sprdfb][of_parse_panel] Can't get sprd,dual_dsi_en, set default value
[sprdfb][of_parse_panel] Can't get sprd,lcd-id-tokernel
[sprdfb][mipi_dsi_init] escape clk div = 7
[sprdfb][sprd_dsi_resume] dsi init OK
[sprdfb][mipi_dphy_configure] lanes : 4
[sprdfb][mipi_dphy_configure] freq : 1105000
[sprdfb][sprd_dphy_resume] dphy init OK
[sprdfb][panel_readid] There is no lcd-id-register config!
[sprdfb][sprd_dphy_suspend] dphy uninit OK
[sprdfb][sprd_dsi_suspend] dsi uninit OK
[sprdfb][sprd_panel_probe] Try to attach panel 0x0 failed, try next...
[sprdfb][of_parse_panel] panel: hx8279_k101_mm2qa01_a_wuxga_inx101
[sprdfb][of_parse_panel] Can't get sprd,dual_dsi_en, set default value
[sprdfb][of_parse_panel] Can't get sprd,lcd-id-tokernel
[sprdfb][mipi_dsi_init] escape clk div = 7
[sprdfb][sprd_dsi_resume] dsi init OK
[sprdfb][mipi_dphy_configure] lanes : 4
[sprdfb][mipi_dphy_configure] freq : 1105000
[sprdfb][sprd_dphy_resume] dphy init OK
[sprdfb][panel_readid] There is no lcd-id-register config!
[sprdfb][sprd_dphy_suspend] dphy uninit OK
[sprdfb][sprd_dsi_suspend] dsi uninit OK
[sprdfb][sprd_panel_probe] Try to attach panel 0x0 failed, try next...
[sprdfb][of_parse_panel] panel: ek79208_sl101pm32d1595_a00_wuxga
[sprdfb][of_parse_panel] Can't get sprd,dual_dsi_en, set default value
[sprdfb][of_parse_panel] Can't get sprd,lcd-id-tokernel
[sprdfb][mipi_dsi_init] escape clk div = 7
[sprdfb][sprd_dsi_resume] dsi init OK
[sprdfb][mipi_dphy_configure] lanes : 4
[sprdfb][mipi_dphy_configure] freq : 1105000
[sprdfb][sprd_dphy_resume] dphy init OK
[sprdfb][panel_readid] There is no lcd-id-register config!
[sprdfb][sprd_dphy_suspend] dphy uninit OK
[sprdfb][sprd_dsi_suspend] dsi uninit OK
[sprdfb][sprd_panel_probe] Try to attach panel 0x0 failed, try next...
[sprdfb][of_parse_panel] panel: hx8279_hsx101bf40a_m32a_wuxga_boe101
[sprdfb][of_parse_panel] Can't get sprd,dual_dsi_en, set default value
[sprdfb][of_parse_panel] Can't get sprd,lcd-id-tokernel
[sprdfb][mipi_dsi_init] escape clk div = 7
[sprdfb][sprd_dsi_resume] dsi init OK
[sprdfb][mipi_dphy_configure] lanes : 4
[sprdfb][mipi_dphy_configure] freq : 1105000
[sprdfb][sprd_dphy_resume] dphy init OK
[sprdfb][panel_readid] There is no lcd-id-register config!
[sprdfb][sprd_dphy_suspend] dphy uninit OK
[sprdfb][sprd_dsi_suspend] dsi uninit OK
[sprdfb][sprd_panel_probe] Try to attach panel 0x0 failed, try next...
[sprdfb][of_parse_panel] panel: tv101wum_ll2_wuxga
[sprdfb][of_parse_panel] Can't get sprd,dual_dsi_en, set default value
[sprdfb][mipi_dsi_init] escape clk div = 7
[sprdfb][sprd_dsi_resume] dsi init OK
[sprdfb][mipi_dphy_configure] lanes : 4
[sprdfb][mipi_dphy_configure] freq : 1105000
[sprdfb][sprd_dphy_resume] dphy init OK
[sprdfb][panel_readid] There is no lcd-id-register config!
[sprdfb][panel_readid] force-attach panel 'tv101wum_ll2_wuxga' by tokernel=0x8006
[sprdfb][sprd_panel_probe] attach panel 0x8006 success
[sprdfb][dump_panel_info] ----Success attached lcd name:tv101wum_ll2_wuxga, id: 0x8006-----
[sprdfb][dump_panel_info] lcd: type:2, bpp:24, width:1200, height:1920
[sprdfb][dump_panel_info] lcd: gpio_reset: 50
[sprdfb][dump_panel_info] lcd: gpio_tp3v3en: 0
[sprdfb][dump_panel_info] lcd: gpio_lcm1v8en: 0
[sprdfb][dump_panel_info] ========sprd,reset-on-sequence ========
[sprdfb][dump_panel_info] lcd: index:0, level: 1, delay: 5
[sprdfb][dump_panel_info] lcd: index:1, level: 0, delay: 5
[sprdfb][dump_panel_info] lcd: index:2, level: 1, delay: 80
[sprdfb][dump_panel_info] ========sprd,reset-off-sequence ========
[sprdfb][dump_panel_info] lcd: index:0, level: 0, delay: 5
[sprdfb][dump_panel_info] ---------------------------------------
[sprdfb][panel_init] Download Panel Init Data Success!
[sprdfb][dispc_clk_init] DPI_CLK_SRC = 153600000
[sprdfb][dispc_clk_init] DPU_CORE_CLK = 384000000, DPI_CLK_SRC = 153600000
[sprdfb][dispc_clk_update] the actual dpi_clk = 153600000
[sprdfb][dpu_run] dpu run
[sprdfb][sprd_dispc_resume] dispc init OK
video_sprd dpu@20300000: SPRD panel 'tv101wum_ll2_wuxga' 1200x1920 up (fb @ 0xff700000)
sprd backlight power on. pwm_index=2 brightness=200
Net:   No ethernet found.

Hit any key to stop autoboot: 0
switch to partitions #0, OK
mmc0(part 0) is current device
MMC read: dev # 0, block # 360448, count 71680 ... 71680 blocks read: OK
## Booting Android Image at 0x90000000 ...
Kernel load addr 0x80080000 size 17010 KiB
Kernel command line: console=ttyS1,115200n8 earlycon=sprd_serial,0x70100000,115200n8 ignore_loglevel earlyprintk rdinit=/init androidboot.selinux=permissive
RAM disk load addr 0x88000000 size 4171 KiB
## Flattened Device Tree blob at a2000000
   Booting using the fdt blob at 0xa2000000
Working FDT set to a2000000
   Uncompressing Kernel Image to 80080000
   Loading Ramdisk to fde11000, end fe223a1d ... OK
ERROR: reserving fdt memory region failed (addr=fff80000 size=40000 flags=4): -17
ERROR: reserving fdt memory region failed (addr=fd708000 size=1e5a000 flags=4): -17
ERROR: reserving fdt memory region failed (addr=ff562000 size=a1e000 flags=4): -17
   Loading Device Tree to 00000000fddea000, end 00000000fde10589 ... OK
Working FDT set to fddea000
hwfeature: /hwfeature/auto efuse="T610" chipid="UMS512-AC", /hwfeature/lwfq type="1" written to devicetree

Starting kernel ...

Timer summary in microseconds (13 records):
       Mark    Elapsed  Stage
          0          0  reset
      8,385      8,385  board_init_f
    427,082    418,697  board_init_r
  2,740,079  2,312,997  eth_common_init
  2,741,854      1,775  main_loop
  5,712,466  2,970,612  bootm_start
  5,712,467          1  boot_get_kernel
  6,153,664    441,197  bootm_load_os
  6,153,688         24  boot_ramdisk_high
  6,270,323    116,635  boot_jump_linux
  6,272,318      1,995  start_kernel

Accumulated time:
               191,179  dm_f
                 6,775  dm_r
```

> Otomatik boot süresi 2 saniyedir UART üzerinden herhangi bir tuşa basılıp kesilip U-Boot shelle düşülebilir.

## Dizin yapısı

```
arch/arm/mach-sprd/            platform kaynak kodu
arch/arm/dts/adt1061.dts       devicetree
board/spreadtrum/ums512_1h10/  board yapılandırması
drivers/clk/sprd/              clock sürücüsü
drivers/gpio/sprd_gpio.c       GPIO
drivers/spi/sprd_adi.c         ADI/SPI 
drivers/i2c/                   I2C
drivers/power/                 SC2730 PMIC, regülatör, BQ2560x charger
drivers/mmc/sprd_sdhci.c       eMMC (SDHCI DM glue)
drivers/video/sprd/            DPU/DSI/D-PHY video yığını
drivers/usb/musb-new/sprd*     MUSB gadget glue + donanım DMA
configs/adt1061_defconfig      board defconfig
scripts/signer/v1/, v2/        imzalama scriptleri
make.sh                        derleme + imzalama scripti
```
