#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 the Tango authors
# check_fwid.sh -- mk_fwid.sh makes the firmware build id that System > About shows (tango_about.h, as "Tango FPGA
# v<yymmdd> (g<hash>)"; the launcher's System Settings: "Firmware <id>"). Checks its format (the shape tango_about_version
# parses), the dirty flag (the firmware folder only), both layouts (the firmware folder as a repository's root, or as the
# menu repository's bl616/) and the pinned date, in throwaway git repos, and the shape of what it prints for this checkout.
#   check_fwid.sh        (make test runs it)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
MK=$HERE/../../mk_fwid.sh
checks=0; fails=0
check() { checks=$((checks + 1)); if ! eval "$2"; then fails=$((fails + 1)); echo "FAIL check_fwid: $1"; fi; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
export TZ=UTC SOURCE_DATE_EPOCH=1791800000                 # 2026-10-12 10:13 UTC
FMT='^#define TANGO_FW_ID "g[0-9a-f]{7}(-dirty)? 2026-10-12 10:13"$'

# a throwaway repo with the firmware in bl616/: only bl616/ counts for -dirty
R=$T/repo; mkdir -p "$R/bl616" "$R/docs"; echo a > "$R/bl616/x.c"; echo a > "$R/docs/d.md"; echo : > "$R/bl616/mk_fwid.sh"
git -C "$R" init -q 2>/dev/null
git -C "$R" add -A
git -C "$R" -c user.name=fwid-test -c user.email=fwid-test@localhost commit -q -m t
sha=$(git -C "$R" rev-parse --short=7 HEAD)
out=$("$MK" "$R/bl616")
check "clean tree: one line, g<7 hex> 2026-10-12 10:13 ($out)" '[ "$(echo "$out" | wc -l)" = 1 ] && echo "$out" | grep -Eq "$FMT"'
check "clean tree: the hash is HEAD's (g$sha)" '[ "$out" = "#define TANGO_FW_ID \"g$sha 2026-10-12 10:13\"" ]'
check "the repository's root names its bl616/ folder" '[ "$("$MK" "$R")" = "$out" ]'
echo b >> "$R/bl616/x.c"; out=$("$MK" "$R/bl616")
check "a modified file in bl616/ is -dirty ($out)" '[ "$out" = "#define TANGO_FW_ID \"g$sha-dirty 2026-10-12 10:13\"" ]'
check "  and so from the repository's root" '[ "$("$MK" "$R")" = "$out" ]'
git -C "$R" checkout -q -- bl616/x.c; out=$("$MK" "$R/bl616")
check "restored: clean again" '[ "$out" = "#define TANGO_FW_ID \"g$sha 2026-10-12 10:13\"" ]'
echo n > "$R/bl616/new.c"; out=$("$MK" "$R/bl616")
check "an untracked file in bl616/ is -dirty" 'echo "$out" | grep -q -- "-dirty"'
rm "$R/bl616/new.c"; echo b >> "$R/docs/d.md"; out=$("$MK" "$R/bl616")
check "a change outside bl616/ is not" '! echo "$out" | grep -q -- "-dirty"'
git -C "$R" -c user.name=fwid-test -c user.email=fwid-test@localhost commit -q -am t2; out=$("$MK" "$R/bl616")
check "a new commit is a new hash" '[ "$out" != "#define TANGO_FW_ID \"g$sha 2026-10-12 10:13\"" ] && echo "$out" | grep -Eq "$FMT"'

# a throwaway repo that IS the firmware folder (the firmware's own repository): every file counts
P=$T/fwrepo; mkdir -p "$P/tango"; echo a > "$P/tango/x.c"; echo a > "$P/README.md"
git -C "$P" init -q 2>/dev/null
git -C "$P" add -A
git -C "$P" -c user.name=fwid-test -c user.email=fwid-test@localhost commit -q -m t
psha=$(git -C "$P" rev-parse --short=7 HEAD)
out=$("$MK" "$P")
check "root layout: the hash is HEAD's (g$psha)" '[ "$out" = "#define TANGO_FW_ID \"g$psha 2026-10-12 10:13\"" ]'
echo b >> "$P/README.md"; out=$("$MK" "$P")
check "root layout: any modified file is -dirty ($out)" '[ "$out" = "#define TANGO_FW_ID \"g$psha-dirty 2026-10-12 10:13\"" ]'
git -C "$P" checkout -q -- README.md; echo n > "$P/tango/new.c"; out=$("$MK" "$P")
check "root layout: an untracked file is -dirty" 'echo "$out" | grep -q -- "-dirty"'
rm "$P/tango/new.c"

# a copy of the firmware folder inside some other repository (not its root, not bl616/): not that repository's hash
mkdir -p "$P/vendor/fw"; out=$("$MK" "$P/vendor/fw")
check "a folder elsewhere in another repository: gnogit ($out)" '[ "$out" = "#define TANGO_FW_ID \"gnogit 2026-10-12 10:13\"" ]'
rm -rf "$P/vendor"

# not a git checkout (an export): "gnogit", the build still gets an id
mkdir "$T/export"; out=$("$MK" "$T/export")
check "no git: gnogit ($out)" '[ "$out" = "#define TANGO_FW_ID \"gnogit 2026-10-12 10:13\"" ]'

# a pinned date is UTC whatever the build machine's zone (GCC pins __DATE__/__TIME__ in UTC too)
for tz in America/Los_Angeles Asia/Tokyo; do
    out=$(TZ=$tz "$MK" "$T/export")
    check "pinned date under TZ=$tz is UTC ($out)" '[ "$out" = "#define TANGO_FW_ID \"gnogit 2026-10-12 10:13\"" ]'
done

# this checkout, the date not pinned: the shape About is sized for
unset SOURCE_DATE_EPOCH TZ
out=$("$MK")
check "this checkout: $out" 'echo "$out" | grep -Eq "^#define TANGO_FW_ID \"g([0-9a-f]{7}|nogit)(-dirty)? [0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}\"$"'
check "the id stays under 56 characters (a label holds 63, minus \"Firmware \")" '[ "$(echo "$out" | sed "s/^[^\"]*\"\(.*\)\"$/\1/" | wc -c)" -le 56 ]'

echo "check_fwid: $checks checks, $fails failed"
[ "$fails" = 0 ]
