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
