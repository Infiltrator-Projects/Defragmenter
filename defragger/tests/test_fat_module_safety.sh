#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# This operates only on temporary files and the separately built test host.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
worker=${1:?pass the test-only host}
modules=${2:?pass the directory containing the built FAT modules}
boundary_fixture=${3:-}
work=$(mktemp -d "${TMPDIR:-/tmp}/fat-module-safety.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
mkdir -m 0755 "$work/Filesystems"
export IFS_TEST_MODULE_PARENT="$work" PYTHONDONTWRITEBYTECODE=1
install -m 0644 "$modules"/FAT{12,16,32}.so "$work/Filesystems/"
python3 "$root/tests/make_fat12_16_image.py" fat12 "$work/12.img" fragmented >/dev/null
python3 "$root/tests/make_fat12_16_image.py" fat16 "$work/16.img" fragmented >/dev/null
"$worker" identify "$work/12.img" | grep -q '"bits":12'
"$worker" identify "$work/16.img" | grep -q '"bits":16'

before=$(sha256sum "$work/12.img")
install -m 0644 "$modules/FAT16.so" "$work/Filesystems/FAT12.so"
if "$worker" defrag "$work/12.img" --write --confirm "$work/12.img" \
        --journal "$work/wrong.journal" >"$work/failure.log" 2>&1; then
    echo 'accepted FAT16.so renamed as FAT12.so' >&2; exit 1
fi
[[ $(sha256sum "$work/12.img") == "$before" && ! -e "$work/wrong.journal" ]] || {
    echo 'wrong module changed the target or created a journal' >&2; exit 1;
}
install -m 0644 "$modules/FAT12.so" "$work/Filesystems/FAT12.so"
chmod 0666 "$work/Filesystems/FAT12.so"
if "$worker" identify "$work/12.img" >"$work/failure.log" 2>&1; then
    echo 'accepted a world-writable module' >&2; exit 1
fi
chmod 0644 "$work/Filesystems/FAT12.so"
mv "$work/Filesystems/FAT12.so" "$work/saved.so"
ln -s "$work/saved.so" "$work/Filesystems/FAT12.so"
if "$worker" identify "$work/12.img" >"$work/failure.log" 2>&1; then
    echo 'accepted a symlinked module' >&2; exit 1
fi
rm "$work/Filesystems/FAT12.so"
mv "$work/saved.so" "$work/Filesystems/FAT12.so"
if "$worker" defrag "$work/12.img" --write --confirm wrong \
        --journal "$work/wrong.journal" >"$work/failure.log" 2>&1; then
    echo 'accepted an incorrect write confirmation' >&2; exit 1
fi
[[ $(sha256sum "$work/12.img") == "$before" && ! -e "$work/wrong.journal" ]] || {
    echo 'incorrect confirmation changed the target or created a journal' >&2; exit 1;
}
if [[ -n $boundary_fixture ]]; then
    python3 "$boundary_fixture" "$work/boundary"
    "$worker" identify "$work/boundary/boundary-4084.img" | grep -q '"bits":12'
    "$worker" identify "$work/boundary/boundary-4086.img" | grep -q '"bits":16'
    ambiguous="$work/boundary/boundary-4085.img"
    before=$(sha256sum "$ambiguous")
    if "$worker" defrag "$ambiguous" --write --confirm "$ambiguous" \
            --journal "$work/boundary.journal" >"$work/failure.log" 2>&1; then
        echo 'accepted the ambiguous 4085-cluster FAT boundary' >&2; exit 1
    fi
    [[ $(sha256sum "$ambiguous") == "$before" && ! -e "$work/boundary.journal" ]] || {
        echo 'ambiguous FAT boundary changed the target or created a journal' >&2; exit 1;
    }
fi
echo 'FAT module identity, trust and write-confirmation checks passed'
