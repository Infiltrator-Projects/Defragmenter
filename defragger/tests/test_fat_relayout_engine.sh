#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Strict Common V2 FAT module integration using disposable, provider-created
# FAT12/16/32 images. No V1 worker, hidden native parser or legacy journal.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FAT_WORKER=${1:-"$ROOT/build/defragmenter-fat-worker"}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/fat-common-v2.XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT
FIXTURE=${2:-"$(dirname -- "$FAT_WORKER")/defragmenter-test-media-raw-test"}
MAPPER=${3:-"$(dirname -- "$FAT_WORKER")/defragmenter-mapper"}
export DEFRAGMENTER_FAT_WORKER="$FAT_WORKER"

fail() { echo "FAT COMMON V2 TEST FAILURE: $*" >&2; exit 1; }
[[ -x "$FAT_WORKER" ]] || fail "missing FAT Common V2 dispatcher"

[[ -x "$FIXTURE" ]] || fail "missing Common V2 test-media consumer"
[[ -x "$MAPPER" ]] || fail "missing production mapper"
command -v jq >/dev/null || fail "missing JSON test assertion utility"
for bits in 12 16 32; do
    "$FIXTURE" create "fat$bits" "$WORK/fat$bits.img" || fail "FAT$bits provider could not create media"
    "$FIXTURE" verify "fat$bits" "$WORK/fat$bits.img" || fail "FAT$bits provider payload verification failed"
done

for bits in 12 16 32; do
    image="$WORK/fat$bits.img"
    before=$(sha256sum "$image" | cut -d' ' -f1)
    "$FAT_WORKER" volume-info "$image" >"$WORK/fat$bits-info.json" ||
        fail "FAT$bits Common V2 CORE+VOLUME query failed"
    grep -Fq "\"filesystem\":\"fat$bits\"" "$WORK/fat$bits-info.json" ||
        fail "FAT$bits provider returned the wrong format identity"
    grep -Fq '"abi":2' "$WORK/fat$bits-info.json" ||
        fail "FAT$bits did not run through strict Common V2"

    # ANALYSE relies on DEFRAG.MAP; the provider must walk its native FAT
    # allocation chain. Defragmenter may paginate but never interpret FAT.
    "$FAT_WORKER" analyse "$image" >"$WORK/fat$bits-map.log" ||
        fail "FAT$bits native Common V2 allocation MAP failed"

    # Same mapper command used by the production Test Media creator.
    "$MAPPER" "$image" --fstype "fat$bits" --cells 1 >"$WORK/fat$bits-map.json" ||
        fail "FAT$bits production fragmentation qualification failed"
    jq -e '.backend_id == "fat'$bits'" and .fragmented_files >= 8 and
           .unknown_bytes == 0 and
           .map_accuracy == "exact-allocation" and
           (if .directory_fragmentation_available then .fragmented_directories == 0 else (.fragmented_directories == null) end) and
           .cell_count == 1 and .cells[0].fragmented > 0' "$WORK/fat$bits-map.json" >/dev/null ||
        fail "FAT$bits provider did not produce genuinely fragmented test files"
    "$MAPPER" "$image" --fstype "fat$bits" --cells 128 >"$WORK/fat$bits-detail.json" ||
        fail "FAT$bits detailed map failed"
    jq -e --slurpfile one "$WORK/fat$bits-map.json" '
        .fragmented_files == $one[0].fragmented_files and
        .free_bytes == $one[0].free_bytes and .used_bytes == $one[0].used_bytes and
        ([.cells[].fragmented]|add) == $one[0].cells[0].fragmented and
        .cell_count == 128' "$WORK/fat$bits-detail.json" >/dev/null ||
        fail "FAT$bits map totals changed with display resolution"

    # Analyse and unconfirmed writes must never modify the source image.
    for operation in defrag growth-defrag recover; do
        if "$FAT_WORKER" "$operation" "$image" \
                >"$WORK/fat$bits-$operation-readonly.log" 2>&1; then
            fail "FAT$bits accepted an unconfirmed mutation: $operation"
        fi
    done
    after=$(sha256sum "$image" | cut -d' ' -f1)
    [[ "$before" == "$after" ]] ||
        fail "FAT$bits Common V2 inspection or rejection modified media"
    "$FIXTURE" verify "fat$bits" "$image" || fail "FAT$bits provider payload changed"

    # Require genuine native relocation, not the historical MAP-only
    # rejection. Ordinary and growth operations start from independent
    # fragmented corpora; each must preserve all payloads and eliminate
    # actual displaced extents. The provider's VERIFY checks growth gaps.
    "$FIXTURE" qualify-defrag "fat$bits" "$image" ||
        fail "FAT$bits native defrag failed"
    growth="$WORK/fat$bits-growth.img"
    "$FIXTURE" qualify-create "fat$bits" "$growth" ||
        fail "FAT$bits independent growth corpus failed"
    "$FIXTURE" qualify-growth "fat$bits" "$growth" ||
        fail "FAT$bits native growth defrag failed"
    "$FAT_WORKER" recover "$growth" --write --confirm "$growth" ||
        fail "FAT$bits native recovery refused a clean checkpoint"
    "$FIXTURE" verify "fat$bits" "$growth" ||
        fail "FAT$bits recovery changed payloads"
    "$FIXTURE" qualify-recovery "fat$bits" ||
        fail "FAT$bits interrupted relocation and recovery failed"
    echo "FAT$bits Common V2 VOLUME+MAP, defrag, growth and clean recovery passed"
done

echo 'FAT12/16/32 STRICT COMMON V2 PROVIDER INTEGRATION PASS'
