#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 the Tango authors
# build.sh -- build the Tango BL616 firmware (the FPGA Companion fork) for one board.
#
#   ./build.sh console138k        # or console60k, primer25k
#   ./build.sh console138k clean  # clean first
#
# Works from the firmware folder wherever it is: the root of its own repository, or the bl616/ folder of the menu
# repository (menu-tango). Every path is relative to this script's folder.
#
# Needs (see companion/UPSTREAM.md and README.md), all in one folder, BL616_SDK (set it; see the default below):
#   bouffalo_sdk (MiSTle-Dev), toolchain_gcc_t-head_linux, and the FPGA-Companion clone (for its submodules;
#   FPGA_COMPANION overrides its place, default $BL616_SDK/FPGA-Companion).
#
# The build runs in a scratch tree (TANGO_BUILD_TREE, default $BL616_SDK/tango-build), never inside this repo:
#   our fork (companion/) is copied there, the three submodules our repo omits are copied in
#   from the upstream clone, and the SDK recipe runs with two fixes that change no source:
#   - the SDK's post_build step calls a bare `cmake`, so its bundled cmake goes on PATH;
#   - the SDK hard-codes an inner `make -j8`, so the recipe's steps are run by hand at -j2, niced,
#     so that a machine running FPGA builds and simulations at the same time stays responsive.
# Output: <build tree>/src/bl616/build/build_out/fpga_companion_bl616.bin
#
# Build id (System > About shows "Tango FPGA v<yymmdd> (g<hash>)", the launcher's System Settings "Firmware <id>"): mk_fwid.sh writes tango_fwid.h -- git short hash, -dirty if this folder
# differs from HEAD, build date and time -- next to tango_fw.c in the scratch tree, after the rsync (its --delete would
# remove it). tango_fw.c is touched so it is recompiled every build, and the finished image is searched for the id: a
# stale object cannot ship an old one (the image is renamed *.FWID-CHECK-FAILED). Build from a committed, clean tree and
# the id is the commit the image came from.
#
# Release build (a published image): from a committed, clean tree, always clean, with the date pinned --
#   SOURCE_DATE_EPOCH=$(date -u -d '<YYYY-MM-DD> 12:00' +%s) ./build.sh console138k clean
# SOURCE_DATE_EPOCH goes into the make environment: GCC pins every __DATE__/__TIME__ to it, in UTC (the SDK's
# "Build:" banner, bsp/board/bl616dk/board.c, and its .verinfo stamp, drivers/lhal/src/bflb_common.c), and mk_fwid.sh
# pins the id's date, also in UTC. Only a clean build pins them all: an incremental one keeps the stamps of objects it
# does not recompile. Unset (a dev build), all of them are the local build time. Paths: every compile gets
# -ffile-prefix-map (companion/src/bl616/CMakeLists.txt), so __FILE__ strings name tango-build/... or ./components/...,
# never the build machine's directories. Prebuilt vendor libraries keep their own version strings (e.g. phyrf's).
#
# RAM check (after the build; it fails the build): every FreeRTOS task stack, TCB, queue and semaphore comes from the
# heap (heap_3 -> malloc -> the SDK's TLSF heaps: OCRAM = what static RAM leaves of the 320 KB, then the ~62 KB WRAM
# heap). A build with 91 KB more static buffers left the OCRAM heap at 3184 bytes, under TLSF's own 3264-byte
# overhead, so it held nothing; the boot's stacks (~66 KB: shell 16 KB, tcpip 4, USB hub 2, runner 8, FPGA com 16,
# launcher 16) did not fit the WRAM heap alone, the launcher task's stack was refused (vApplicationMallocFailedHook ->
# abort) and the launcher never appeared. The check wants an OCRAM heap of at least TANGO_HEAP_MIN_KB (default 75,
# comfortably above the ~66 KB of boot-time task stacks it protects; the 64-character file-name budget brought the
# heap to 79 KB): the boot's stacks fit it alone, the WRAM heap stays for USB devices and a core's video processing.
# A build under it is renamed *.RAM-CHECK-FAILED.
set -eu
BOARD=${1:?usage: build.sh <console138k|console60k|primer25k> [clean]}
HERE=$(cd "$(dirname "$0")" && pwd)          # the firmware folder (a repository's root, or the menu repository's bl616/)
SDK=${BL616_SDK:-$HOME/projects/bl616-sdk}
UPSTREAM=${FPGA_COMPANION:-$SDK/FPGA-Companion}
TREE=${TANGO_BUILD_TREE:-$SDK/tango-build}   # two builds at the same time each need their own tree (TANGO_BUILD_TREE)
for need in "$SDK/bouffalo_sdk" "$SDK/toolchain_gcc_t-head_linux/bin" "$UPSTREAM/src/u8g2" "$UPSTREAM/src/FreeRTOS-Kernel" \
            "$UPSTREAM/src/tusb_xinput"; do
    [ -d "$need" ] || { echo "build.sh: $need is missing (BL616_SDK=$SDK; see companion/UPSTREAM.md)"; exit 1; }
done

rsync -a --delete --exclude='src/bl616/build/' "$HERE/companion/" "$TREE/"
rsync -a --delete --exclude='test/' --exclude='cosim/obj_*/' "$HERE/tango/" "$TREE/src/tango/"   # Tango launcher + OSD backend
for d in src/u8g2 src/FreeRTOS-Kernel src/tusb_xinput; do
    rsync -a "$UPSTREAM/$d/" "$TREE/$d/"
done
"$HERE/mk_fwid.sh" "$HERE" > "$TREE/src/tango/tango_fwid.h"              # after the rsync: its --delete would remove it
touch "$TREE/src/tango/tango_fw.c"                                       # always recompiled: never a stale id
FWID=$(sed -n 's/^#define TANGO_FW_ID "\(.*\)"$/\1/p' "$TREE/src/tango/tango_fwid.h")
[ -n "$FWID" ] || { echo "no firmware id from mk_fwid.sh"; exit 1; }
echo "firmware build id: $FWID"
if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
    export SOURCE_DATE_EPOCH                                             # GCC: __DATE__/__TIME__ pinned, in UTC
    echo "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH ($(date -u -d "@$SOURCE_DATE_EPOCH" '+%Y-%m-%d %H:%M:%S UTC'))"
fi

cd "$TREE/src/bl616"
export CROSS_COMPILE=$SDK/toolchain_gcc_t-head_linux/bin/riscv64-unknown-elf-
export BL_SDK_BASE=$SDK/bouffalo_sdk
export PATH=$BL_SDK_BASE/tools/cmake/bin:$PATH
[ "${2:-}" = clean ] && make clean

start=$(date +%s)
nice -n 10 make TANG_BOARD="$BOARD" cmake_cache
nice -n 10 make -C build pre_build
nice -n 10 make -C build -j2
nice -n 10 make -C build post_build
nice -n 10 make -C build combine
OUT=$TREE/src/bl616/build/build_out
if ! grep -aqF "$FWID" "$OUT/fpga_companion_bl616.bin"; then
    mv "$OUT/fpga_companion_bl616.bin" "$OUT/fpga_companion_bl616.bin.FWID-CHECK-FAILED"
    echo "FIRMWARE ID CHECK FAILED: the image does not contain \"$FWID\" (a stale tango_fw.c object?)"
    exit 1
fi
sym() { "${CROSS_COMPILE}nm" "$OUT/fpga_companion_bl616.elf" | awk -v s="$1" '$3 == s { print $1 }'; }
OCRAM=$(( 0x$(sym __HeapLimit) - 0x$(sym __HeapBase) )); WRAM=$(( 0x$(sym _heap_wifi_size) ))
MIN_KB=${TANGO_HEAP_MIN_KB:-75}
echo "RAM: OCRAM heap $OCRAM bytes ($(( OCRAM / 1024 )) KB), WRAM heap $WRAM bytes ($(( WRAM / 1024 )) KB); OCRAM floor $MIN_KB KB"
if [ "$OCRAM" -lt $(( MIN_KB * 1024 )) ]; then
    mv "$OUT/fpga_companion_bl616.bin" "$OUT/fpga_companion_bl616.bin.RAM-CHECK-FAILED"
    echo "RAM CHECK FAILED: the OCRAM heap is $OCRAM bytes, under $MIN_KB KB -- static RAM grew (the largest:)"
    "${CROSS_COMPILE}nm" -S --size-sort -r "$OUT/fpga_companion_bl616.elf" | awk '$3 ~ /^[bBdD]$/' | head -8
    exit 1
fi
echo "BUILD OK: $BOARD, firmware $FWID, in $(( $(date +%s) - start )) s -> $OUT/fpga_companion_bl616.bin"
