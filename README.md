# Aidata ADT1061 U-Boot

Das U-Boot **2015.07-g7d8ebb8** source code for the **Aidata ADT1061** tablet.

## Device Information ##

| Item | Value |
|------|------|
| Device | Aidata ADT1061 |
| U-Boot Version | 2015.07-g7d8ebb8 |
| Platform | Unisoc UMS512 |

## Building ##

Use Ubuntu 18.04 LTS for the best result

### Clone repository
```bash
git clone https://github.com/mmetem55/adt1061_u-boot.git && cd adt1061_u-boot
```

### Download and extract the prebuilt toolchain
```bash
git clone https://github.com/sukrut-kelkar/gcc-linaro-4.9.git prebuilts/gcc/linaro-x86/aarch64/gcc-linaro-4.9.4-2017.01-x86_64_aarch64-linux-gnu && git clone https://github.com/sayedather/arm-eabi-4.8-toolchain.git prebuilts/gcc/linux-x86/arm/arm-eabi-4.8
```

### Script permission configuration ###
```bash
chmod +x make.sh && chmod +x make-debug.sh
```
### Build the U-Boot ###
```bash
./make.sh
```

### Build the U-Boot with Debug ###
```bash
./make-debug.sh
```

## Status ##

🟢 Active development

## Credits ##

- Das U-Boot Developers
- Spreadtrum/Unisoc Develop
- Veynamer/bsp_bootloader_u-boot15_sharkl5pro
- mmetem55

## License ##

This project follows the original Das U-Boot (GPL-2.0) licensing.
