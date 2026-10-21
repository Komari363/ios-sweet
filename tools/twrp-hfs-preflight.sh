#!/bin/bash
# Host-side TWRP HFS+ preflight.
#
#   tools/twrp-hfs-preflight.sh RECOVERY_IMG [OUTDIR]
#
# Unpacks the boot image, decompresses the kernel and ramdisk, and looks for
# HFS+ support evidence (hfsplus symbols, CONFIG_HFSPLUS_FS, or a .ko).
# This is only a warning, not proof: a stripped kernel can still mount HFS+.
# The authoritative test is TWRP's Mount UI / mount(8) on the device.
set -euo pipefail

IMG=${1:?usage: $0 RECOVERY_IMG [OUTDIR]}
OUT=${2:-"$(dirname "$IMG")/twrp-hfs-preflight"}
UNPACK_BOOTIMG=${UNPACK_BOOTIMG:?set UNPACK_BOOTIMG to unpack_bootimg.py}

rm -rf "$OUT"
mkdir -p "$OUT"
python3 "$UNPACK_BOOTIMG" --boot_img "$IMG" --out "$OUT"

kernel="$OUT/kernel"
ramdisk="$OUT/ramdisk"
found=0
if [ -f "$kernel" ]; then
	if gzip -t "$kernel" 2>/dev/null; then
		gzip -dc "$kernel" > "$OUT/kernel.raw"
	else
		cp "$kernel" "$OUT/kernel.raw"
	fi
	if grep -aEq 'hfsplus|CONFIG_HFSPLUS_FS|CONFIG_HFS_FS' "$OUT/kernel.raw"; then
		echo "kernel: HFS+ strings found"
		grep -aoE 'hfsplus|CONFIG_HFSPLUS_FS|CONFIG_HFS_FS' "$OUT/kernel.raw" | sort -u
		found=1
	else
		echo "kernel: no HFS+ strings"
	fi
fi
if [ -f "$ramdisk" ]; then
	if gzip -t "$ramdisk" 2>/dev/null; then
		gzip -dc "$ramdisk" > "$OUT/ramdisk.cpio"
	else
		cp "$ramdisk" "$OUT/ramdisk.cpio"
	fi
	if grep -aEq 'hfsplus|CONFIG_HFSPLUS_FS' "$OUT/ramdisk.cpio"; then
		# blkid/file/probe binaries carry the string even when the kernel
		# has no HFS+ driver; do not count this as mount support.
		echo "ramdisk: HFS+ userland probe strings (not kernel support)"
	else
		echo "ramdisk: no HFS+ strings"
	fi
fi
if [ "$found" = 0 ]; then
	echo "result: no kernel HFS+ evidence in this recovery image"
	echo "         expect the Mount UI to omit iosroot; confirm on the device"
	exit 2
fi
echo "result: kernel HFS+ evidence present; still confirm mount on the device"
