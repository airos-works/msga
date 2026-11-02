#!/usr/bin/env bash
# Cross-builds msga for air/OS from Linux with an airos-ci SDK, and stages
# the package the way airos-ci's build_msga (apps/build-app-packages.sh)
# does. Usage:
#
#   tools/haiku/build.sh [x86_64|arm64] [--package]
#
#   HAIKU_SDK   the SDK (default /data1/airos/sdk/ARCH; x86_64-r1beta6 for a
#               stock Haiku R1/beta6, e.g. the test VM in tools/haiku/vm)
#   BUILD       build directory (default build-haiku-ARCH)
#
# Output: BUILD/MSGA (stripped, with its resources), and with --package
# BUILD/msga-VERSION-ARCH.hpkg made with the SDK's host package tool, for a quick
# test (no attributes from the resources: airos-ci's package has them).
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
ARCH=${1:-x86_64}
PACKAGE=0
[[ ${2:-} == --package ]] && PACKAGE=1
SDK=${HAIKU_SDK:-/data1/airos/sdk/$ARCH}
BUILD=${BUILD:-$ROOT/build-haiku-$ARCH}
[[ -f $SDK/env.sh ]] || { echo "no SDK at $SDK" >&2; exit 1; }
# CROSS, TOOLS (host rc/xres/resattr/mimeset/package), MIMEDB
. "$SDK/env.sh"
RC=${HOST_TOOLS:+$HOST_TOOLS/rc}
RC=${RC:-$TOOLS/rc/rc}
XRES=${HOST_TOOLS:+$HOST_TOOLS/xres}
XRES=${XRES:-$TOOLS/xres}

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel \
	-DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/haiku/toolchain.cmake" \
	-DHAIKU_ARCH="$ARCH" -DHAIKU_SDK="$SDK" >/dev/null
cmake --build "$BUILD" --target msga

python3 "$ROOT/tools/haiku/make-rdef.py" "$BUILD/msga.rdef"
"$RC" -o "$BUILD/msga.rsrc" "$BUILD/msga.rdef"
"${CROSS}strip" --strip-debug -o "$BUILD/MSGA" "$BUILD/msga"
# GNU strip drops appended Haiku resources: add them after stripping.
"$XRES" -o "$BUILD/MSGA" "$BUILD/msga.rsrc"
echo "built $BUILD/MSGA"

[[ $PACKAGE == 1 ]] || exit 0
STAGE=$BUILD/stage
rm -rf "$STAGE"
mkdir -p "$STAGE/apps" "$STAGE/data/deskbar/menu/Office" "$STAGE/data/licenses" \
	"$STAGE/documentation/packages/msga"
cp "$BUILD/MSGA" "$STAGE/apps/MSGA"
# The tray icon's Deskbar add-on (src/plat/src/haiku/deskbar_tray.cpp).
mkdir -p "$STAGE/add-ons/MSGA"
"${CROSS}strip" --strip-debug -o "$STAGE/add-ons/MSGA/MSGA Deskbar item" "$BUILD/msga_deskbar.so"
ln -s ../../../../apps/MSGA "$STAGE/data/deskbar/menu/Office/MSGA"
cp -R "$ROOT/README.md" "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_LICENSES" "$ROOT/docs" \
	"$STAGE/documentation/packages/msga/"
cp "$BUILD/_deps/msga_freetype-src/docs/FTL.TXT" "$STAGE/data/licenses/FTL"
sed "s/^architecture[ \t].*/architecture	$ARCH/" "$ROOT/resources/haiku/msga.PackageInfo" \
	> "$STAGE/.PackageInfo"
version=$(awk '$1 == "version" { print $2 }' "$STAGE/.PackageInfo")
rm -f "$BUILD"/msga-*.hpkg
"$TOOLS/package/package" create -q -C "$STAGE" "$BUILD/msga-$version-$ARCH.hpkg"
echo "package $BUILD/msga-$version-$ARCH.hpkg"
