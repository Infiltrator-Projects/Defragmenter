#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FAT_WORKER=${1:-"$ROOT/build/linux-defragger-fat-worker"}
BUILD_DIR=$(CDPATH= cd -- "$(dirname -- "$FAT_WORKER")" && pwd)
HFS_ANALYSER="$BUILD_DIR/hfs_analyser"
XFS_WORKER="$BUILD_DIR/linux-defragger-xfs-worker"
XFS_NATIVE_TEST="$BUILD_DIR/linux-defragger-xfs-native-test"
XFS_METADATA_TEST="$BUILD_DIR/linux-defragger-xfs-metadata-test"
EXPECTED_VERSION=$(tr -d '\r\n' <"$ROOT/VERSION")
WORK=$(mktemp -d "${TMPDIR:-/tmp}/linux-defragger-tests.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

export PYTHONDONTWRITEBYTECODE=1
export PYTHONPATH="$ROOT/gui"
export LINUX_DEFRAGGER_BUILD_DIR="$BUILD_DIR"
bash -n "$ROOT/tests/destructive/run_dm_log_writes_replay.sh"

fail() {
    echo "TEST FAILURE: $*" >&2
    exit 1
}

[[ -x "$FAT_WORKER" ]] || fail "FAT worker is not executable: $FAT_WORKER"
[[ -x "$HFS_ANALYSER" ]] || fail "HFS analyser is not executable: $HFS_ANALYSER"
[[ -x "$XFS_WORKER" ]] || fail "XFS worker is not executable: $XFS_WORKER"
export LINUX_DEFRAGGER_XFS_WORKER="$XFS_WORKER"

version=$($FAT_WORKER --version)
[[ "$version" == "linux-defragger-fat-worker $EXPECTED_VERSION" ]] || \
    fail "unexpected FAT worker version: $version"

python3 -m compileall -q "$ROOT/gui"
"$ROOT/tests/test_release_artifacts.sh"
"$ROOT/tests/run_typecheck.sh"
python3 "$ROOT/tests/test_no_external_fs_tools.py"
python3 "$ROOT/tests/test_architecture.py"
python3 "$ROOT/tests/test_analysis_plugins.py"
python3 "$ROOT/tests/test_ext_catalog_fuzz.py"
PYTHONPATH="$ROOT/gui:$ROOT/tests" python3 "$ROOT/tests/test_planner_properties.py"
python3 "$ROOT/tests/test_gui_analysis_start.py"
python3 "$ROOT/tests/test_gui_models.py"
python3 "$ROOT/tests/test_gui_services.py"
python3 "$ROOT/tests/test_range_helpers.py"
python3 "$ROOT/tests/test_safety.py"
python3 "$ROOT/tests/test_worker_entrypoints.py"
LINUX_DEFRAGGER_BUILD_DIR="$BUILD_DIR" PYTHONPATH="$ROOT/gui:$ROOT/tests" python3 "$ROOT/tests/test_native_top3.py"
LINUX_DEFRAGGER_BUILD_DIR="$BUILD_DIR" PYTHONPATH="$ROOT/gui:$ROOT/tests" python3 "$ROOT/tests/test_affs_native.py"
LINUX_DEFRAGGER_BUILD_DIR="$BUILD_DIR" PYTHONPATH="$ROOT/gui:$ROOT/tests" python3 "$ROOT/tests/test_hfsplus_native.py"
LINUX_DEFRAGGER_BUILD_DIR="$BUILD_DIR" PYTHONPATH="$ROOT/gui:$ROOT/tests" python3 "$ROOT/tests/test_hfs_native.py"
python3 "$ROOT/tests/test_xfs_writer.py"
if [[ -x "$XFS_NATIVE_TEST" ]]; then "$XFS_NATIVE_TEST"; fi
if [[ -x "$XFS_METADATA_TEST" ]]; then "$XFS_METADATA_TEST"; fi

# FAT32: a fragmented file must become contiguous and occupy the first legal
# data clusters directly after the root directory.
python3 "$ROOT/tests/make_fragmented_image.py" "$WORK/fragmented.img" >/dev/null
"$FAT_WORKER" defrag "$WORK/fragmented.img" \
    --write --confirm "$WORK/fragmented.img" --journal "$WORK/fragmented.journal" \
    --live-map-cells 1024 >"$WORK/fragmented.log" 2>&1
python3 "$ROOT/tests/verify_defragged_image.py" "$WORK/fragmented.img"
grep -q '@@LIVE_MAP ' "$WORK/fragmented.log" || fail "Defragment emitted no live map records"
grep -q 'no free clusters below the final allocation' "$WORK/fragmented.log" || \
    fail "Defragment did not verify the zero-gap invariant"

# FAT32: fragmented root/subdirectory/file chains must all be rebuilt into one
# packed allocation prefix, with directory references corrected.
python3 "$ROOT/tests/make_fragmented_directory_image.py" "$WORK/directories.img" >/dev/null
"$FAT_WORKER" defrag "$WORK/directories.img" \
    --write --confirm "$WORK/directories.img" --journal "$WORK/directories.journal" \
    >"$WORK/directories.log" 2>&1
python3 "$ROOT/tests/verify_directory_defrag.py" "$WORK/directories.img"

# FAT32: even zero-fragment input must be rewritten when physical holes remain.
python3 "$ROOT/tests/make_gapped_contiguous_image.py" "$WORK/gapped.img" >/dev/null
"$FAT_WORKER" defrag "$WORK/gapped.img" \
    --write --confirm "$WORK/gapped.img" --journal "$WORK/gapped.journal" \
    >"$WORK/gapped.log" 2>&1
python3 "$ROOT/tests/verify_gapped_contiguous.py" "$WORK/gapped.img"
"$FAT_WORKER" defrag "$WORK/gapped.img" \
    --write --confirm "$WORK/gapped.img" --journal "$WORK/gapped-second.journal" \
    >"$WORK/gapped-second.log" 2>&1
grep -q 'Not needed; canonical packed layout verified' "$WORK/gapped-second.log" || \
    fail "second Defragment pass was not idempotent"
grep -q '@@RESULT {"operation":"defrag","status":"not-needed"' "$WORK/gapped-second.log" || \
    fail "FAT did not emit a typed not-needed result"

# FAT32 Growth Defrag: exact ten-percent gaps, contiguous chains and payloads.
python3 "$ROOT/tests/make_growth_defrag_image.py" "$WORK/growth.img" >/dev/null
"$FAT_WORKER" growth-defrag "$WORK/growth.img" \
    --write --confirm "$WORK/growth.img" --journal "$WORK/growth.journal" \
    --growth-percent 10 --live-map-cells 1024 >"$WORK/growth.log" 2>&1
python3 "$ROOT/tests/verify_growth_defrag.py" "$WORK/growth.img"
grep -q '@@LIVE_MAP ' "$WORK/growth.log" || fail "Growth Defrag emitted no live map records"
grep -q '@@RESULT {"operation":"growth-defrag","status":"completed"' "$WORK/growth.log" || \
    fail "FAT did not emit a typed completion result"
if "$FAT_WORKER" growth-defrag "$WORK/growth.img" \
    --write --confirm "$WORK/growth.img" --journal "$WORK/bad-growth.journal" \
    --growth-percent 5 >"$WORK/bad-growth.log" 2>&1; then
    fail "Growth Defrag accepted a reserve other than ten percent"
fi

# FAT12 and FAT16 use exactly the same native relayout engine.  The reserve
# percentage is policy: zero for packed Defrag, ten for Growth Defrag.
for kind in fat12 fat16; do
    python3 "$ROOT/tests/make_fat12_16_image.py" "$kind" "$WORK/$kind-defrag.img" fragmented >/dev/null
    "$FAT_WORKER" map "$WORK/$kind-defrag.img" --cells 100000 \
        >"$WORK/$kind-fragment-map.json"
    python3 "$ROOT/tests/verify_fragment_map.py" "$WORK/$kind-fragment-map.json"
    "$FAT_WORKER" defrag "$WORK/$kind-defrag.img" \
        --write --confirm "$WORK/$kind-defrag.img" --journal "$WORK/$kind-defrag.journal" \
        >"$WORK/$kind-defrag.log" 2>&1
    python3 "$ROOT/tests/verify_growth_fat12_16.py" "$kind" "$WORK/$kind-defrag.img"

    python3 "$ROOT/tests/make_fat12_16_image.py" "$kind" "$WORK/$kind-growth.img" fragmented >/dev/null
    "$FAT_WORKER" growth-defrag "$WORK/$kind-growth.img" \
        --write --confirm "$WORK/$kind-growth.img" --journal "$WORK/$kind-growth.journal" \
        --growth-percent 10 >"$WORK/$kind-growth.log" 2>&1
    python3 "$ROOT/tests/verify_growth_fat12_16.py" "$kind" "$WORK/$kind-growth.img"

    if grep -q 'Growth Defrag layout staged one' "$WORK/$kind-growth.log"; then
        fail "$kind Growth Defrag fell back to pathological one-object staging"
    fi
    growth_transactions=$(sed -n \
        's/^Growth Defrag layout I\/O:.* in \([0-9][0-9]*\) transaction.*$/\1/p' \
        "$WORK/$kind-growth.log" | tail -n 1)
    [[ -n "$growth_transactions" && "$growth_transactions" -le 8 ]] || \
        fail "$kind Growth Defrag exceeded eight layout transactions: ${growth_transactions:-missing}"
done

# FAT16: terminal safety-workspace preparation may expand beyond the largest
# single object when RAM and spare tail capacity permit it, but the whole
# preparation must remain one bounded journal transaction rather than chasing
# low holes object by object.
python3 "$ROOT/tests/make_fat16_workspace_image.py" "$WORK/fat16-workspace.img" >/dev/null
"$FAT_WORKER" defrag "$WORK/fat16-workspace.img" \
    --write --confirm "$WORK/fat16-workspace.img" \
    --journal "$WORK/fat16-workspace.journal" \
    >"$WORK/fat16-workspace.log" 2>&1
prep_transactions=$(sed -n \
    's/^Defragment preparation complete:.* in \([0-9][0-9]*\) transaction.*$/\1/p' \
    "$WORK/fat16-workspace.log" | tail -n 1)
[[ -n "$prep_transactions" && "$prep_transactions" -le 1 ]] || \
    fail "FAT16 preparation exceeded one bounded transaction: ${prep_transactions:-missing}"
if grep -q 'Defragment layout staged one' "$WORK/fat16-workspace.log"; then
    fail "FAT16 Defragment regressed to one-object staging"
fi
python3 "$ROOT/tests/verify_fat16_workspace_image.py" "$WORK/fat16-workspace.img"

# FAT16: collapsing a verified 10% Growth layout back to zero-gap packing must
# use the same relayout engine.  A leftward target plan should choose forward
# dependency batches automatically; the command name does not choose direction.
"$FAT_WORKER" growth-defrag "$WORK/fat16-workspace.img" \
    --write --confirm "$WORK/fat16-workspace.img" \
    --journal "$WORK/fat16-workspace-growth.journal" --growth-percent 10 \
    >"$WORK/fat16-workspace-growth.log" 2>&1
grep -q 'canonical 10% growth gaps verified' \
    "$WORK/fat16-workspace-growth.log" || \
    fail "FAT16 Growth layout was not independently verified"
"$FAT_WORKER" defrag "$WORK/fat16-workspace.img" \
    --write --confirm "$WORK/fat16-workspace.img" \
    --journal "$WORK/fat16-growth-to-packed.journal" --live-map-cells 2048 \
    >"$WORK/fat16-growth-to-packed.log" 2>&1
grep -q 'Defragment forward layout batch:' \
    "$WORK/fat16-growth-to-packed.log" || \
    fail "FAT16 Growth-to-Defragment did not choose forward dependency batches"
if grep -q 'Growth layout cleared\|Defragment layout staged one' \
    "$WORK/fat16-growth-to-packed.log"; then
    fail "FAT16 Growth-to-Defragment still bounced individual objects"
fi
transactions=$(sed -n \
    's/^Defragment layout I\/O:.* in \([0-9][0-9]*\) transactions$/\1/p' \
    "$WORK/fat16-growth-to-packed.log")
[[ -n "$transactions" && "$transactions" -le 10 ]] || \
    fail "FAT16 Growth-to-Defragment exceeded ten layout transactions: ${transactions:-missing}"
forward_batches=$(grep -c '^Defragment forward layout batch:' \
    "$WORK/fat16-growth-to-packed.log")
live_updates=$(grep -c '^@@LIVE_MAP ' "$WORK/fat16-growth-to-packed.log")
[[ "$live_updates" -eq "$forward_batches" ]] || \
    fail "FAT16 live map exposed transient staging allocations: ${live_updates} updates for ${forward_batches} completed batches"
python3 "$ROOT/tests/verify_fat16_workspace_image.py" "$WORK/fat16-workspace.img"

# Write confirmation is mandatory. Classic HFS has its own dedicated mutation
# regression above; an invalid target must still fail closed here.
if "$FAT_WORKER" defrag "$WORK/gapped.img" --write --confirm wrong \
    --journal "$WORK/wrong-confirm.journal" >"$WORK/wrong-confirm.log" 2>&1; then
    fail "engine accepted an incorrect write confirmation"
fi
if "$HFS_ANALYSER" defrag /dev/null >"$WORK/hfs-write.log" 2>&1; then
    fail "classic HFS worker accepted an invalid write target"
fi

printf 'Linux Defragger %s focused tests passed.\n' "$EXPECTED_VERSION"
