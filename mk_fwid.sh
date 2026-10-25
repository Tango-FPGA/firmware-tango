#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 the Tango authors
# mk_fwid.sh -- prints tango_fwid.h, the firmware's build id, for build.sh to put next to tango_fw.c in its scratch
# build tree (never into the source tree). One line:
#     #define TANGO_FW_ID "g<git short hash>[-dirty] <YYYY-MM-DD> <HH:MM>"
# System > About shows it (tango_about.h: "Tango FPGA v<yymmdd> (g<hash>[-dirty])"; the launcher's System Settings shows
# the whole id as "Firmware <id>"), so the firmware on a board can be told from the screen. Build from a committed,
# clean tree and the id is the commit the image came from.
#
# The firmware folder (default: this script's own folder) is either the root of its repository or the bl616/ folder of
# the menu repository; both layouts give the same id for the same commit and state.
#   g<hash>  git rev-parse --short=7 HEAD of the repository holding the firmware folder (7 characters); "gnogit" if the
#            folder is not a git checkout (an export), or sits inside some other repository at any other place
#   -dirty   the firmware folder (the firmware's sources and this build) differs from HEAD or has untracked files;
#            the rest of the repository does not go into the image and does not count
#   date     the build's own, local time; SOURCE_DATE_EPOCH (seconds) pins it for a reproducible rebuild, in UTC (as GCC
#            pins __DATE__/__TIME__ from it), whatever TZ the build machine has -- a release names no time zone
# tango_about_version parses this format (the date after the first space, as YYYY-MM-DD); keep it, test_tango checks the parse.
#   usage: mk_fwid.sh [firmware-folder]
#          (a folder that holds bl616/mk_fwid.sh -- the menu repository's root -- means its bl616/ folder)
set -eu
DIR=${1:-$(cd "$(dirname "$0")" && pwd)}
[ -f "$DIR/bl616/mk_fwid.sh" ] && DIR=$DIR/bl616
sha=nogit dirty=
if prefix=$(git -C "$DIR" rev-parse --show-prefix 2>/dev/null) && { [ -z "$prefix" ] || [ "$prefix" = bl616/ ]; } &&
   s=$(git -C "$DIR" rev-parse --short=7 HEAD 2>/dev/null); then
    sha=$s
    [ -n "$(git -C "$DIR" status --porcelain -- . 2>/dev/null)" ] && dirty=-dirty
fi
if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then when=$(date -u -d "@$SOURCE_DATE_EPOCH" '+%Y-%m-%d %H:%M')
else when=$(date '+%Y-%m-%d %H:%M'); fi
printf '#define TANGO_FW_ID "g%s%s %s"\n' "$sha" "$dirty" "$when"
