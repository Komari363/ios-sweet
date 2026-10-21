#!/bin/bash
# Build the NABU first-light boot image on a Linux host.
#
#   tools/build-nabu-firstlight.sh KERNEL OUTDIR [PRODUCT_ADT]
#
# KERNEL       kernel.development.sweet from CI (with 0084 applied)
# OUTDIR       output directory (e.g. output/a4.2/first-light)
# PRODUCT_ADT  real D22AP ADT; default ../../../output/a1/d22ap.adt
#
# Required environment:
#   MKBOOTIMG      path to mkbootimg.py (vendored AOSP Python)
#   UNPACK_BOOTIMG path to unpack_bootimg.py
# Optional:
#   CMDLINE        override the full boot-args string
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
KERNEL=${1:?usage: $0 KERNEL OUTDIR [PRODUCT_ADT]}
OUTDIR=${2:?usage: $0 KERNEL OUTDIR [PRODUCT_ADT]}
PRODUCT=${3:-$(cd "$ROOT/../.." && pwd)/output/a1/d22ap.adt}
MKBOOTIMG=${MKBOOTIMG:?set MKBOOTIMG to mkbootimg.py}
UNPACK_BOOTIMG=${UNPACK_BOOTIMG:?set UNPACK_BOOTIMG to unpack_bootimg.py}

CMDLINE=${CMDLINE:-"debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress rd=ufsroot0 ufsroot=iosroot ufsrw=1 rootrw=1 launchd_unsecure_cache=1 unentitled_ios_sim_launch=1 ignition_level=1 wdt=-1 wdt_test=no_user_checkin sweetfb_noconsole=1 sweetfb_hz=30"}

KERNEL=$(readlink -f "$KERNEL")
PRODUCT=$(readlink -f "$PRODUCT")
mkdir -p "$OUTDIR"
OUTDIR=$(cd "$OUTDIR" && pwd)
ADT="$OUTDIR/nabu.adt"
PAYLOAD="$OUTDIR/payload.bin"
BOOTIMG="$OUTDIR/shim-boot.img"

python3 "$ROOT/tools/mkadt.py" "$ADT" --nabu --product "$PRODUCT"
python3 "$ROOT/tools/mkpayload.py" "$KERNEL" "$ADT" "$PAYLOAD" --cmdline "$CMDLINE"
BOARD=nabu HEADER_VERSION=3 MKBOOTIMG="$MKBOOTIMG" \
	"$ROOT/shim/build.sh" "$PAYLOAD"

cp "$ROOT/shim/out/shim-boot.img" "$BOOTIMG"
cp "$ROOT/shim/out/Image" "$OUTDIR/shim-Image"
rm -rf "$OUTDIR/unpacked"
python3 "$UNPACK_BOOTIMG" --boot_img "$BOOTIMG" --out "$OUTDIR/unpacked"
cmp "$OUTDIR/unpacked/ramdisk" "$PAYLOAD"
python3 - "$PAYLOAD" <<'PY'
import sys
assert open(sys.argv[1], 'rb').read(8) == b'SWEETXNU', 'payload magic missing'
PY

sha256sum "$BOOTIMG" "$PAYLOAD" "$ADT" "$OUTDIR/shim-Image"
echo "first-light image: $BOOTIMG"
