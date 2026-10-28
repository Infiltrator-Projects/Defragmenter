#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run only with the test-only host, which can stop after a durable journal write.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
worker=${1:?pass the test-only FAT module host}
work=$(mktemp -d "${TMPDIR:-/tmp}/fat-module-recovery.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
export PYTHONDONTWRITEBYTECODE=1

for kind in fat12 fat16 fat32; do
    for stage in 1 2 3 4; do
        image="$work/$kind-$stage.img"
        journal="$work/$kind-$stage.journal"
        if [[ $kind == fat32 ]]; then
            python3 "$root/tests/make_fragmented_image.py" "$image" >/dev/null
        else
            python3 "$root/tests/make_fat12_16_image.py" "$kind" "$image" fragmented >/dev/null
        fi
        status=0
        IFS_TEST_CRASH_AFTER_JOURNAL=$stage "$worker" defrag "$image" \
            --write --confirm "$image" --journal "$journal" \
            >"$work/crash.log" 2>&1 || status=$?
        if [[ $status != 86 || ! -f $journal ]]; then
            cat "$work/crash.log" >&2
            echo "$kind did not stop after durable journal stage $stage" >&2
            exit 1
        fi
        # A different inode must never consume another volume's journal.
        cp "$image" "$work/other.img"
        before=$(sha256sum "$work/other.img")
        if "$worker" recover "$work/other.img" --write --confirm "$work/other.img" \
                --journal "$journal" >"$work/wrong-target.log" 2>&1; then
            echo "$kind stage $stage accepted a different recovery target" >&2
            exit 1
        fi
        [[ $(sha256sum "$work/other.img") == "$before" && -f $journal ]] || {
            echo "$kind stage $stage changed the rejected recovery target" >&2
            exit 1
        }
        "$worker" recover "$image" --write --confirm "$image" --journal "$journal" \
            >"$work/recover.log" 2>&1 || { cat "$work/recover.log" >&2; exit 1; }
        [[ ! -e $journal ]] || { echo "recovery left the journal behind" >&2; exit 1; }
        "$worker" defrag "$image" --write --confirm "$image" --journal "$journal" \
            >"$work/finish.log" 2>&1 || { cat "$work/finish.log" >&2; exit 1; }
        if [[ $kind == fat32 ]]; then
            python3 "$root/tests/verify_defragged_image.py" "$image"
        else
            python3 "$root/tests/verify_growth_fat12_16.py" "$kind" "$image"
        fi
        rm "$work/other.img"
        echo "$kind journal stage $stage recovered safely"
    done
done

# Stop after actual target writes, including writes between journal stages.
# This is a process interruption test; it does not simulate lost flushes.
for kind in fat12 fat16 fat32; do
    exercised=0
    for write in $(seq 1 12); do
        image="$work/$kind-write-$write.img"
        journal="$work/$kind-write-$write.journal"
        if [[ $kind == fat32 ]]; then
            python3 "$root/tests/make_fragmented_image.py" "$image" >/dev/null
            cp "$image" "$work/fat32-before.img"
        else
            python3 "$root/tests/make_fat12_16_image.py" "$kind" "$image" fragmented >/dev/null
        fi
        status=0
        IFS_TEST_CRASH_AFTER_DEVICE_WRITE=$write "$worker" defrag "$image" \
            --write --confirm "$image" --journal "$journal" --workers 1 \
            >"$work/crash-write.log" 2>&1 || status=$?
        if [[ $status == 0 ]]; then break; fi
        if [[ $status != 87 ]]; then
            cat "$work/crash-write.log" >&2
            echo "$kind device-write interruption $write did not stop as expected" >&2
            exit 1
        fi
        if [[ -f $journal ]]; then
            "$worker" recover "$image" --write --confirm "$image" --journal "$journal" \
                --workers 1 >"$work/recover-write.log" 2>&1 || {
                cat "$work/recover-write.log" >&2
                echo "$kind device-write interruption $write could not recover" >&2
                exit 1
            }
        else
            # FAT32 may refresh its free-cluster hint before starting a
            # transaction. It is safe without a journal only when no bytes
            # other than the primary/backup hint changed.
            [[ $kind == fat32 ]] || { echo "$kind lost its journal" >&2; exit 1; }
            python3 - "$work/fat32-before.img" "$image" <<'PY'
import struct, sys
from pathlib import Path
before = Path(sys.argv[1]).read_bytes()
after = bytearray(Path(sys.argv[2]).read_bytes())
sector = struct.unpack_from('<H', before, 11)[0]
info = struct.unpack_from('<H', before, 48)[0]
backup = struct.unpack_from('<H', before, 50)[0]
for number in (info, backup + info):
    start = number * sector + 492
    after[start:start + 4] = before[start:start + 4]
assert after == before, 'unjournalled FAT32 change exceeded the FSInfo hints'
PY
        fi
        "$worker" defrag "$image" --write --confirm "$image" --journal "$journal" \
            --workers 1 >"$work/finish-write.log" 2>&1 || {
            cat "$work/finish-write.log" >&2; exit 1;
        }
        if [[ $kind == fat32 ]]; then
            python3 "$root/tests/verify_defragged_image.py" "$image" >/dev/null
        else
            python3 "$root/tests/verify_growth_fat12_16.py" "$kind" "$image" >/dev/null
        fi
        exercised=$((exercised + 1))
    done
    [[ $exercised -gt 0 ]] || { echo "$kind had no tested target writes" >&2; exit 1; }
    echo "$kind recovered after $exercised interrupted target writes"
done
