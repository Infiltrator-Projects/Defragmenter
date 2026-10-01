#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FAT_WORKER=${1:-"$ROOT/build/linux-defragger-fat-worker"}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/linux-defragger-fat-relayout.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

export PYTHONDONTWRITEBYTECODE=1

fail() {
    echo "FAT RELAYOUT TEST FAILURE: $*" >&2
    exit 1
}

[[ -x "$FAT_WORKER" ]] || fail "FAT worker is not executable: $FAT_WORKER"

# FAT32 fragmented data, directory chains, zero-gap compaction and Growth
# Defrag all use the same reserve-driven native relayout engine.
python3 "$ROOT/tests/make_fragmented_image.py" "$WORK/fragmented.img" >/dev/null
"$FAT_WORKER" defrag "$WORK/fragmented.img" \
    --write --confirm "$WORK/fragmented.img" --journal "$WORK/fragmented.journal" \
    >"$WORK/fragmented.log" 2>&1
python3 "$ROOT/tests/verify_defragged_image.py" "$WORK/fragmented.img"
grep -Eq '^Defragment engine started: +[0-9]{4}-[0-9]{2}-[0-9]{2} ' \
    "$WORK/fragmented.log" || fail "Defragment did not record its engine start time"
grep -Eq '^Defragment engine finished: +[0-9]{4}-[0-9]{2}-[0-9]{2} ' \
    "$WORK/fragmented.log" || fail "Defragment did not record its engine finish time"
grep -q '^Defragment phase timings:$' "$WORK/fragmented.log" || \
    fail "Defragment did not record phase timings"
grep -Eq '^Defragment engine elapsed: +[0-9]+\.[0-9]{3} s$' \
    "$WORK/fragmented.log" || fail "Defragment did not record total engine duration"
grep -Eq '^Buffered read throughput: +[0-9]+\.[0-9] MiB/s$' \
    "$WORK/fragmented.log" || fail "Defragment did not record read throughput"
grep -Eq '^Buffered write throughput: +[0-9]+\.[0-9] MiB/s$' \
    "$WORK/fragmented.log" || fail "Defragment did not record write throughput"

python3 "$ROOT/tests/make_fragmented_directory_image.py" "$WORK/directories.img" >/dev/null
"$FAT_WORKER" defrag "$WORK/directories.img" \
    --write --confirm "$WORK/directories.img" --journal "$WORK/directories.journal" \
    >"$WORK/directories.log" 2>&1
python3 "$ROOT/tests/verify_directory_defrag.py" "$WORK/directories.img"

python3 "$ROOT/tests/make_gapped_contiguous_image.py" "$WORK/gapped.img" >/dev/null
"$FAT_WORKER" defrag "$WORK/gapped.img" \
    --write --confirm "$WORK/gapped.img" --journal "$WORK/gapped.journal" \
    >"$WORK/gapped.log" 2>&1
python3 "$ROOT/tests/verify_gapped_contiguous.py" "$WORK/gapped.img"

python3 "$ROOT/tests/make_growth_defrag_image.py" "$WORK/growth.img" >/dev/null
"$FAT_WORKER" growth-defrag "$WORK/growth.img" \
    --write --confirm "$WORK/growth.img" --journal "$WORK/growth.journal" \
    --growth-percent 10 >"$WORK/growth.log" 2>&1
python3 "$ROOT/tests/verify_growth_defrag.py" "$WORK/growth.img"
grep -q '^Growth Defrag phase timings:$' "$WORK/growth.log" || \
    fail "Growth Defrag did not record phase timings"
grep -Eq '^Growth Defrag engine elapsed: +[0-9]+\.[0-9]{3} s$' \
    "$WORK/growth.log" || fail "Growth Defrag did not record total engine duration"

# FAT12 and FAT16 are not separate relocation implementations.  For the small
# generated images the RAM/workspace budget must resolve Growth Defrag without
# degrading into one-object blocker shuffling.
for kind in fat12 fat16; do
    python3 "$ROOT/tests/make_fat12_16_image.py" \
        "$kind" "$WORK/$kind-defrag.img" fragmented >/dev/null
    "$FAT_WORKER" defrag "$WORK/$kind-defrag.img" \
        --write --confirm "$WORK/$kind-defrag.img" \
        --journal "$WORK/$kind-defrag.journal" \
        >"$WORK/$kind-defrag.log" 2>&1
    python3 "$ROOT/tests/verify_growth_fat12_16.py" \
        "$kind" "$WORK/$kind-defrag.img"

    python3 "$ROOT/tests/make_fat12_16_image.py" \
        "$kind" "$WORK/$kind-growth.img" fragmented >/dev/null
    "$FAT_WORKER" growth-defrag "$WORK/$kind-growth.img" \
        --write --confirm "$WORK/$kind-growth.img" \
        --journal "$WORK/$kind-growth.journal" --growth-percent 10 \
        >"$WORK/$kind-growth.log" 2>&1
    python3 "$ROOT/tests/verify_growth_fat12_16.py" \
        "$kind" "$WORK/$kind-growth.img"

    if grep -q 'Growth Defrag layout staged one' "$WORK/$kind-growth.log"; then
        cat "$WORK/$kind-growth.log" >&2
        fail "$kind Growth Defrag fell back to pathological one-object staging"
    fi
    transactions=$(sed -n \
        's/^Growth Defrag layout I\/O:.* in \([0-9][0-9]*\) transaction.*$/\1/p' \
        "$WORK/$kind-growth.log" | tail -n 1)
    if [[ -z "$transactions" || "$transactions" -gt 8 ]]; then
        cat "$WORK/$kind-growth.log" >&2
        fail "$kind Growth Defrag used ${transactions:-unknown} layout transactions"
    fi
    echo "$kind Growth Defrag layout transactions: $transactions"
done

# A many-object FAT16 workload must use RAM-sized dependency batches rather than
# the old arbitrary object cap and repeated one-file staging fallback.
python3 "$ROOT/tests/make_fat16_workspace_image.py" "$WORK/fat16-workspace.img" >/dev/null
"$FAT_WORKER" defrag "$WORK/fat16-workspace.img" \
    --write --confirm "$WORK/fat16-workspace.img" \
    --journal "$WORK/fat16-workspace.journal" \
    >"$WORK/fat16-workspace.log" 2>&1
python3 "$ROOT/tests/verify_fat16_workspace_image.py" "$WORK/fat16-workspace.img"
if grep -q 'Defragment layout staged one' "$WORK/fat16-workspace.log"; then
    cat "$WORK/fat16-workspace.log" >&2
    fail "FAT16 Defragment fell back to one-object staging"
fi

# Reproduce the large FAT16 failure mode seen on physical Test Media: the live
# set is larger than the terminal workspace and one fragmented early file owns
# clusters inside hundreds of later Growth targets.  The adaptive scheduler
# must resolve the dependencies with bounded journalled batches.  Whole-object
# staging remains valid when it fits, while the rolling cluster path is expected
# when free target clusters can be released incrementally.  Neither path may
# degrade to the legacy one-object/three-transaction fallback.
python3 "$ROOT/tests/make_fat16_dependency_blocker_image.py" \
    "$WORK/fat16-dependencies.img" >/dev/null
"$FAT_WORKER" growth-defrag "$WORK/fat16-dependencies.img" \
    --write --confirm "$WORK/fat16-dependencies.img" \
    --journal "$WORK/fat16-dependencies.journal" --growth-percent 10 \
    --ram-buffer 128M >"$WORK/fat16-dependencies.log" 2>&1
python3 "$ROOT/tests/verify_fat16_dependency_blocker_image.py" \
    "$WORK/fat16-dependencies.img"
if grep -q 'Growth Defrag layout staged one' "$WORK/fat16-dependencies.log"; then
    cat "$WORK/fat16-dependencies.log" >&2
    fail "FAT16 adaptive dependency test fell back to one-object staging"
fi
grep -q 'adaptive dependency transaction budget:' "$WORK/fat16-dependencies.log" || {
    cat "$WORK/fat16-dependencies.log" >&2
    fail "FAT16 adaptive dependency workload did not report its Stop-safe transaction budget"
}
grep -q 'rolling cluster placement:' "$WORK/fat16-dependencies.log" || {
    cat "$WORK/fat16-dependencies.log" >&2
    fail "FAT16 dependency blocker workload did not exercise rolling cluster placement"
}
if grep -q 'adaptive dependency scheduler exhausted the reusable workspace' \
    "$WORK/fat16-dependencies.log"; then
    cat "$WORK/fat16-dependencies.log" >&2
    fail "FAT16 adaptive dependency scheduler exhausted its reusable workspace"
fi
dependency_transactions=$(sed -n \
    's/^Growth Defrag layout I\/O:.* in \([0-9][0-9]*\) transaction.*$/\1/p' \
    "$WORK/fat16-dependencies.log" | tail -n 1)
if [[ -z "$dependency_transactions" || "$dependency_transactions" -gt 48 ]]; then
    cat "$WORK/fat16-dependencies.log" >&2
    fail "FAT16 adaptive dependency workload used ${dependency_transactions:-unknown} layout transactions"
fi
echo "FAT16 adaptive dependency layout transactions: $dependency_transactions"

# Direction is selected from the target dependencies, not from the operation
# name: build a 10% layout, then collapse the same volume to a zero-gap layout.
"$FAT_WORKER" growth-defrag "$WORK/fat16-workspace.img" \
    --write --confirm "$WORK/fat16-workspace.img" \
    --journal "$WORK/fat16-workspace-growth.journal" --growth-percent 10 \
    >"$WORK/fat16-workspace-growth.log" 2>&1
grep -q 'canonical 10% growth gaps verified' \
    "$WORK/fat16-workspace-growth.log" || {
        cat "$WORK/fat16-workspace-growth.log" >&2
        fail "FAT16 Growth layout was not independently verified"
    }

"$FAT_WORKER" defrag "$WORK/fat16-workspace.img" \
    --write --confirm "$WORK/fat16-workspace.img" \
    --journal "$WORK/fat16-growth-to-packed.journal" \
    >"$WORK/fat16-growth-to-packed.log" 2>&1
python3 "$ROOT/tests/verify_fat16_workspace_image.py" "$WORK/fat16-workspace.img"
if grep -q 'Defragment layout staged one' "$WORK/fat16-growth-to-packed.log"; then
    cat "$WORK/fat16-growth-to-packed.log" >&2
    fail "FAT16 Growth-to-packed Defragment fell back to one-object staging"
fi

printf 'Unified FAT12/FAT16/FAT32 relayout integration tests passed.\n'
