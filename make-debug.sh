#!/bin/bash

exec > >(tee build-log.txt) 2>&1

if [ "$1" = "bsp" ]
then
    make V=1 -f BspUBoot.mk
    exit
fi

if [ "$1" = "" ] || [ "$2" = "" ]
then
    echo "Pleast input command : make 'arch_name' 'board_name'"
    echo "for examle: make x86 iwhale2"
    exit
fi

export KBUILD_OUTPUT=./out
export UBOOT_DEBUG_FLAG=-DDEBUG

if [ "$1" = "arm" ];then
    export PATH=$PATH:$(pwd)/../prebuilts/gcc/linux-x86/arm/arm-eabi-4.8/bin
    export CROSS_COMPILE=arm-eabi-
    make V=1 distclean
    make V=1 ARCH=arm "$2"_defconfig
    make V=1 ARCH=arm DEVICE_TREE="$3" -j4

elif [ "$1" = "arm64" ]; then
    export PATH=$PATH:$(pwd)/../prebuilts/gcc/linaro-x86/aarch64/gcc-linaro-4.9.4-2017.01-x86_64_aarch64-linux-gnu/bin
    export CROSS_COMPILE=aarch64-linux-gnu-
    make V=1 distclean
    make V=1 ARCH=arm "$2"_defconfig
    make V=1 ARCH=arm DEVICE_TREE="$3" -j4

elif [ "$1" = "x86" ]; then
    export PATH=$PATH:$(pwd)/../prebuilts/gcc/linux-x86/x86/x86_64-linux-android-4.9/bin
    export CROSS_COMPILE=x86_64-linux-android-
    make V=1 distclean
    make V=1 ARCH=x86 "$2"_defconfig
    make V=1 ARCH=x86 DEVICE_TREE="$3" -j4
else
    echo "not suppported arch"
fi

if [ $? -eq 0 ]; then
	echo "Signing U-Boot binary..."
	python3 scripts/signer/v2/sign_uboot.py out/u-boot-dtb.bin out/u-boot-sign.bin
else
	echo "Build failed. Skipping signing."
	exit 1
fi
