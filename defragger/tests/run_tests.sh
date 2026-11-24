#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FAT_WORKER=${1:-"$ROOT/build/defragmenter-fat-worker"}
BUILD_DIR=$(CDPATH= cd -- "$(dirname -- "$FAT_WORKER")" && pwd)
HFS_ANALYSER=${DEFRAGMENTER_HFS_WORKER:-"$BUILD_DIR/hfs_analyser"}
XFS_WORKER=${DEFRAGMENTER_XFS_WORKER:-"$BUILD_DIR/defragmenter-xfs-worker"}
XFS_NATIVE_TEST="$BUILD_DIR/defragmenter-xfs-native-test"
XFS_METADATA_TEST="$BUILD_DIR/defragmenter-xfs-metadata-test"
EXPECTED_VERSION=$(tr -d '\r\n' <"$ROOT/VERSION")
WORK=$(mktemp -d "${TMPDIR:-/tmp}/defragmenter-tests.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

export PYTHONDONTWRITEBYTECODE=1
export DEFRAGMENTER_BUILD_DIR="$BUILD_DIR"
# Validate the complete release packaging shell before running expensive fixtures.
# This catches stray duplicated tails and other syntax errors at the start.
sh -n "$ROOT/tests/test_release_artifacts.sh"
bash -n "$ROOT/tests/destructive/run_dm_log_writes_replay.sh"

fail() {
    echo "TEST FAILURE: $*" >&2
    exit 1
}

[[ -x "$FAT_WORKER" ]] || fail "FAT worker is not executable: $FAT_WORKER"
[[ -x "$HFS_ANALYSER" ]] || fail "HFS analyser is not executable: $HFS_ANALYSER"
[[ -x "$XFS_WORKER" ]] || fail "XFS worker is not executable: $XFS_WORKER"
export DEFRAGMENTER_XFS_WORKER="$XFS_WORKER"

version=$($FAT_WORKER --version)
[[ "$version" == "defragmenter-fat-worker $EXPECTED_VERSION" ]] || \
    fail "unexpected FAT worker version: $version"

"$ROOT/tests/test_release_artifacts.sh"
c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
    "$ROOT/tests/test_no_external_fs_tools.cpp" -o "$WORK/no-external-fs-tools"
"$WORK/no-external-fs-tools" "$ROOT"
python3 "$ROOT/tests/test_architecture.py"
python3 "$ROOT/tests/test_registry_mapper.py"
# Build once and exercise the production worker launch protocol without Python.
c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
    "$ROOT/tests/native_worker_smoke.cpp" -o "$WORK/native-worker-smoke"
"$WORK/native-worker-smoke" ext-catalog-fuzz
"$WORK/native-worker-smoke" worker-entrypoints
DEFRAGMENTER_BUILD_DIR="$BUILD_DIR" PYTHONPATH="$ROOT/tests" python3 "$ROOT/tests/test_native_top3.py"
DEFRAGMENTER_BUILD_DIR="$BUILD_DIR" PYTHONPATH="$ROOT/tests" python3 "$ROOT/tests/test_affs_native.py"
DEFRAGMENTER_BUILD_DIR="$BUILD_DIR" PYTHONPATH="$ROOT/tests" python3 "$ROOT/tests/test_hfsplus_native.py"
DEFRAGMENTER_BUILD_DIR="$BUILD_DIR" PYTHONPATH="$ROOT/tests" python3 "$ROOT/tests/test_hfs_native.py"
python3 "$ROOT/tests/test_xfs_writer.py"
if [[ -x "$XFS_NATIVE_TEST" ]]; then "$XFS_NATIVE_TEST"; fi
if [[ -x "$XFS_METADATA_TEST" ]]; then "$XFS_METADATA_TEST"; fi

# FAT12, FAT16 and FAT32 must be dispatched through trusted Common V2
# providers only. The filesystem repositories own allocation/mutation tests.
bash "$ROOT/tests/test_fat_relayout_engine.sh" "$FAT_WORKER" "$BUILD_DIR/defragmenter-test-media-raw-test"

# An invalid HFS target still fails closed.
if "$HFS_ANALYSER" defrag /dev/null >"$WORK/hfs-write.log" 2>&1; then
    fail "classic HFS worker accepted an invalid write target"
fi

printf 'Defragmenter %s focused tests passed.\n' "$EXPECTED_VERSION"
