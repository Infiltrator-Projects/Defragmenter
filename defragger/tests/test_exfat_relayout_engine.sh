#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORKER=${1:?exFAT worker path required}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/defragmenter-exfat-relayout.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
fail(){ echo "TEST FAILURE: $*" >&2; exit 1; }

python3 "$ROOT/tests/make_exfat_image.py" "$WORK/defrag.img"
"$WORKER" defrag "$WORK/defrag.img" --write --confirm "$WORK/defrag.img" \
    --journal "$WORK/defrag.journal" --ram-buffer 128M --live-updates \
    >"$WORK/defrag.log" 2>&1
python3 "$ROOT/tests/verify_exfat_relayout.py" "$WORKER" "$WORK/defrag.img" defrag
grep -q 'exFAT unified workspace layout:' "$WORK/defrag.log" || fail 'Defrag did not use the unified terminal workspace fast path'
if grep -Eq 'internally verified native-C exFAT working image|shadow-image compatibility path' "$WORK/defrag.log"; then
    fail 'Defrag fell back to the old full-filesystem working-image path'
fi
[[ ! -e "$WORK/defrag.journal" ]] || fail 'Defrag left its recovery journal behind'
"$WORKER" defrag "$WORK/defrag.img" --write --confirm "$WORK/defrag.img" \
    --journal "$WORK/defrag-second.journal" >"$WORK/defrag-second.log" 2>&1
grep -q 'Not needed; canonical exFAT layout with 0% post-file reserve policy verified' "$WORK/defrag-second.log" || fail 'Defrag is not idempotent'

python3 "$ROOT/tests/make_exfat_image.py" "$WORK/growth.img"
"$WORKER" growth-defrag "$WORK/growth.img" --write --confirm "$WORK/growth.img" \
    --journal "$WORK/growth.journal" --growth-percent 10 --ram-buffer 128M \
    >"$WORK/growth.log" 2>&1
python3 "$ROOT/tests/verify_exfat_relayout.py" "$WORKER" "$WORK/growth.img" growth
grep -q 'exFAT unified workspace layout:' "$WORK/growth.log" || fail 'Growth Defrag did not use the unified terminal workspace fast path'
if grep -Eq 'internally verified native-C exFAT working image|shadow-image compatibility path' "$WORK/growth.log"; then
    fail 'Growth Defrag fell back to the old full-filesystem working-image path'
fi
"$WORKER" growth-defrag "$WORK/growth.img" --write --confirm "$WORK/growth.img" \
    --journal "$WORK/growth-second.journal" --growth-percent 10 \
    >"$WORK/growth-second.log" 2>&1
grep -q 'Not needed; canonical exFAT layout with 10% post-file reserve policy verified' "$WORK/growth-second.log" || fail 'Growth Defrag is not idempotent'

"$WORKER" defrag "$WORK/growth.img" --write --confirm "$WORK/growth.img" \
    --journal "$WORK/growth-to-packed.journal" --ram-buffer 128M \
    >"$WORK/growth-to-packed.log" 2>&1
python3 "$ROOT/tests/verify_exfat_relayout.py" "$WORKER" "$WORK/growth.img" defrag
grep -q 'exFAT unified workspace layout:' "$WORK/growth-to-packed.log" || fail 'Growth-to-packed conversion missed unified workspace path'

printf 'Unified exFAT in-place relayout tests passed.\n'
