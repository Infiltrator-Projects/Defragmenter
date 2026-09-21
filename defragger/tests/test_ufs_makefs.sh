#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

[ "$#" -eq 1 ] || {
    printf 'Usage: %s UFS_WORKER\n' "$0" >&2
    exit 2
}

UFS_WORKER=$1
command -v makefs >/dev/null 2>&1 || exit 77
[ -x "$UFS_WORKER" ] || exit 2

WORK=$(mktemp -d "${TMPDIR:-/tmp}/linux-defragger-ufs-makefs.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
mkdir -p "$WORK/source/Defragmenter-TestData/fragmented-files"
printf '%s\n' 'Defragmenter genuine UFS2 makefs integration test' \
    >"$WORK/source/Defragmenter-TestData/fragmented-files/probe.txt"

echo '+ makefs -t ffs -B little -s 64m -o version=2,bsize=8192,fsize=1024,minfree=5'
makefs -t ffs -B little -s 64m \
    -o version=2,bsize=8192,fsize=1024,minfree=5 \
    "$WORK/ufs2.img" "$WORK/source" >/dev/null

IDENTIFY=$($UFS_WORKER identify "$WORK/ufs2.img")
printf '%s\n' "$IDENTIFY" | grep -q '^{"filesystem":"ufs","variant":"ufs2-le","version":2,"byte_order":"little"}$'
ANALYSE=$($UFS_WORKER analyse-json "$WORK/ufs2.img")
printf '%s\n' "$ANALYSE" \
    | grep -q '"filesystem":"ufs","variant":"ufs2-le","version":2,"byte_order":"little"'
printf '%s\n' "$ANALYSE" | grep -q '"map_accuracy":"exact"'

MAP=$($UFS_WORKER map "$WORK/ufs2.img" --cells 8)
python3 - "$MAP" <<'PY'
import json
import sys

payload = json.loads(sys.argv[1])
assert payload["filesystem"] == "ufs"
assert payload["map_accuracy"] == "exact"
assert payload["details"]["allocation_basis"] == "validated UFS1/UFS2 cylinder-group fragment bitmaps"
assert payload["filesystem_bytes"] > 0
assert payload["free_bytes"] > 0
assert payload["used_bytes"] > 0
assert payload["free_bytes"] + payload["used_bytes"] == payload["filesystem_bytes"]
assert payload["unknown_bytes"] == payload["total_bytes"] - payload["filesystem_bytes"]
assert sum(cell["free"] for cell in payload["cells"]) * payload["unit_size"] == payload["free_bytes"]
assert sum(cell["used"] for cell in payload["cells"]) * payload["unit_size"] == payload["used_bytes"]
assert sum(cell["unknown"] for cell in payload["cells"]) * payload["unit_size"] == payload["unknown_bytes"]
PY

printf '%s\n' 'makefs UFS2 image accepted with exact allocation and inode-tree fragmentation mapping.'
