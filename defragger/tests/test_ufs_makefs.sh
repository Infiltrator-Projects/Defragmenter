#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

[ "$#" -eq 2 ] || {
    printf 'Usage: %s UFS_CREATOR UFS_WORKER\n' "$0" >&2
    exit 2
}

UFS_CREATOR=$1
UFS_WORKER=$2
[ -x "$UFS_CREATOR" ] || exit 2
[ -x "$UFS_WORKER" ] || exit 2

WORK=$(mktemp -d "${TMPDIR:-/tmp}/linux-defragger-ufs-native.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
mkdir -p "$WORK/source/Defragmenter-TestData/fragmented-files"

# Four creator blocks are enough to prove that the private creator emits real
# non-contiguous UFS data extents and that the production writer can compact
# the exact fixture it manufactures.
python3 - "$WORK/source/Defragmenter-TestData/fragmented-files/probe.bin" <<'PY'
import pathlib
import sys
pathlib.Path(sys.argv[1]).write_bytes(bytes(range(256)) * 1024)
PY

IMAGE="$WORK/ufs2.img"
echo "+ first-party UFS2 creator -t ffs -B little -s 64m"
"$UFS_CREATOR" -t ffs -B little -s 64m \
    -o version=2,bsize=8192,fsize=1024,minfree=5 \
    "$IMAGE" "$WORK/source"

IDENTIFY=$($UFS_WORKER identify "$IMAGE")
EXPECTED='{"filesystem":"ufs","variant":"ufs2-le","version":2,"byte_order":"little"}'
[ "$IDENTIFY" = "$EXPECTED" ] || {
    printf '%s\n' "unexpected UFS2 identity: $IDENTIFY" >&2
    exit 1
}

ANALYSE=$($UFS_WORKER analyse-json "$IMAGE")
printf '%s\n' "$ANALYSE" \
    | grep -q '"filesystem":"ufs","variant":"ufs2-le","version":2,"byte_order":"little"'
printf '%s\n' "$ANALYSE" | grep -q '"map_accuracy":"exact"'

MAP=$($UFS_WORKER map "$IMAGE" --cells 8)
python3 - "$MAP" "$ANALYSE" <<'PY'
import json
import sys

payload = json.loads(sys.argv[1])
analysis = json.loads(sys.argv[2])
assert payload["filesystem"] == "ufs"
assert payload["map_accuracy"] == "exact"
assert payload["details"]["version"] == 2
assert payload["details"]["allocation_basis"] == "validated UFS1/UFS2 cylinder-group fragment bitmaps"
assert payload["filesystem_bytes"] > 0
assert payload["free_bytes"] > 0
assert payload["used_bytes"] > 0
assert payload["free_bytes"] + payload["used_bytes"] == payload["filesystem_bytes"]
assert payload["unknown_bytes"] == 0
assert payload["outside_bytes"] == payload["total_bytes"] - payload["filesystem_bytes"]
assert sum(cell["free"] for cell in payload["cells"]) * payload["unit_size"] == payload["free_bytes"]
assert sum(cell["used"] for cell in payload["cells"]) * payload["unit_size"] == payload["used_bytes"]
assert sum(cell["unknown"] for cell in payload["cells"]) == 0
assert sum(cell.get("outside", 0) for cell in payload["cells"]) * payload["unit_size"] == payload["outside_bytes"]
assert analysis["fragmented_files"] >= 1
PY

JOURNAL="$WORK/ufs2.journal"
OUTPUT=$($UFS_WORKER defrag "$IMAGE" \
    --write --confirm "$IMAGE" --journal "$JOURNAL")
printf '%s\n' "$OUTPUT" | grep -q '@@RESULT'
[ ! -e "$JOURNAL" ] || {
    printf '%s\n' 'UFS2 Defragment unexpectedly left a recovery journal' >&2
    exit 1
}

AFTER=$($UFS_WORKER analyse-json "$IMAGE")
python3 - "$AFTER" <<'PY'
import json
import sys
analysis = json.loads(sys.argv[1])
assert analysis["fragmented_files"] == 0
assert analysis["fragmented_directories"] == 0
PY

printf '%s\n' 'First-party UFS2 creator accepted with exact allocation mapping, real fragmentation, and qualified native Defragment compaction.'
