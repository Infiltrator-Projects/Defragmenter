#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Strict Common V2 FAT module integration using disposable, source-independent
# FAT12/16/32 images. No V1 worker, hidden native parser or legacy journal.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FAT_WORKER=${1:-"$ROOT/build/defragmenter-fat-worker"}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/fat-common-v2.XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
    "$ROOT/tests/native_fat_fixture.cpp" -o "$WORK/native-fat-fixture"
export PYTHONDONTWRITEBYTECODE=1

fail() { echo "FAT COMMON V2 TEST FAILURE: $*" >&2; exit 1; }
[[ -x "$FAT_WORKER" ]] || fail "missing FAT Common V2 dispatcher"

"$WORK/native-fat-fixture" fat32 "$WORK/fat32.img" >/dev/null
for bits in 12 16; do
    "$WORK/native-fat-fixture" "fat$bits" \
        "$WORK/fat$bits.img" fragmented >/dev/null
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

    # The old V1 engine is prohibited. A V2 reader with MAP only must reject
    # write-side PLAN/EXECUTE/VERIFY/RECOVER instead of claiming it worked.
    for operation in defrag growth-defrag recover; do
        if "$FAT_WORKER" "$operation" "$image" --write --confirm "$image" \
                >"$WORK/fat$bits-$operation.log" 2>&1; then
            fail "FAT$bits advertised an unqualified Common V2 $operation"
        fi
    done
    after=$(sha256sum "$image" | cut -d' ' -f1)
    [[ "$before" == "$after" ]] ||
        fail "FAT$bits Common V2 inspection or rejection modified media"
    echo "FAT$bits Common V2 VOLUME+MAP and mutation rejection passed"
done

echo 'FAT12/16/32 STRICT COMMON V2 PROVIDER INTEGRATION PASS'
