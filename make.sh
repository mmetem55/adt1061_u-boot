#!/bin/bash
#
# One-shot build + auto-sign script for the ADT1061 (Unisoc UMS512/
# sharkl5pro) U-Boot port on upstream Das U-Boot.
#
# Mirrors the vendor adt1061_u-boot 2015.07 tree's own top-level
# make.sh (build, then on success immediately run
# scripts/signer/v2/sign_uboot.py over the resulting image) - that
# signer script and its adt1061-uboot.db header/signature blob are
# copied verbatim from the vendor tree, unchanged, since they are a
# device-specific image-repackaging tool, not U-Boot source.
#
# Usage:
#   ./make.sh                  # defconfig + build + sign, auto job count
#   ./make.sh -j8               # defconfig + build + sign, 8 build jobs
#   ./make.sh clean              # distclean only
#
# Env overrides:
#   CROSS_COMPILE   (default: aarch64-linux-gnu-)
#   DEFCONFIG       (default: adt1061_defconfig)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

: "${CROSS_COMPILE:=aarch64-linux-gnu-}"
: "${DEFCONFIG:=adt1061_defconfig}"
export CROSS_COMPILE

JOBS="-j$(nproc 2>/dev/null || echo 4)"

if [ "$1" = "clean" ]; then
	make distclean
	exit 0
fi

# Allow a custom -jN to override the auto-detected job count.
for arg in "$@"; do
	case "$arg" in
		-j*) JOBS="$arg" ;;
	esac
done

echo "== Building $DEFCONFIG (CROSS_COMPILE=$CROSS_COMPILE, $JOBS) =="
make "$DEFCONFIG"
make $JOBS

UBOOT_BIN="u-boot-dtb.bin"
SIGNED_BIN="u-boot-sign.bin"
SIGNER="scripts/signer/v2/sign_uboot.py"

if [ ! -f "$UBOOT_BIN" ]; then
	echo "Build did not produce $UBOOT_BIN - skipping signing." >&2
	exit 1
fi

echo "== Signing $UBOOT_BIN -> $SIGNED_BIN =="
python3 "$SIGNER" "$UBOOT_BIN" "$SIGNED_BIN"

echo
echo "Done:"
echo "  unsigned: $SCRIPT_DIR/$UBOOT_BIN"
echo "  signed:   $SCRIPT_DIR/$SIGNED_BIN   (flash this one to the device)"
