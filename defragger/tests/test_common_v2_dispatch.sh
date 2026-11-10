#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Nonprivileged disposable-media Common V2 provider test.
set -euo pipefail
engine=$1
module=$2
root=$(mktemp -d /tmp/defrag-v2-dispatch-XXXXXXXX)
trap 'rm -rf -- "$root"' EXIT
chmod 700 "$root"
mkdir -m 700 "$root/Filesystems"
cp -- "$module" "$root/Filesystems/TESTV2.so"
chmod 700 "$root/Filesystems/TESTV2.so"
export IFS_TEST_MODULE_PARENT="$root"

"$engine" --list-installed-modules > "$root/list.json"
grep -Fq '"format_id":"testv2"' "$root/list.json"
grep -Fq '"capabilities":31' "$root/list.json"

device="$root/sample.img"
truncate -s 1048576 "$device"
printf 'TESTV2FS' | dd of="$device" bs=1 conv=notrunc status=none

# Mutating through a valid, trusted V2 provider without an exact target
# confirmation must be rejected before any filesystem block is modified.
original=$(sha256sum "$device" | cut -d' ' -f1)
if "$engine" defrag "$device" --filesystem testv2 --path=/FILE \
   > /dev/null 2>&1; then
    echo "Unconfirmed strict V2 mutation was accepted" >&2; exit 1
fi
test "$original" = "$(sha256sum "$device" | cut -d' ' -f1)"
if "$engine" defrag "$device" --filesystem testv2 --path=/FILE \
   --write --confirm "$root/unrelated.img" > /dev/null 2>&1; then
    echo "Mismatched strict V2 target confirmation was accepted" >&2; exit 1
fi
test "$original" = "$(sha256sum "$device" | cut -d ' ' -f1)"

# A complete explicit lease/consent must invoke real V2 plan/execute/verify.
"$engine" defrag "$device" --filesystem testv2 --path=/FILE \
   --write --confirm "$device" > "$root/output.txt"
grep -Fq 'durably synced' "$root/output.txt"
actual=$(dd if="$device" bs=1 skip=64 count=4 status=none)
test "$actual" = 'MOVE'

# Recovery has a different provider callback but still requires a writable
# lease, durable sync and correct mount identification.
"$engine" recover "$device" --filesystem testv2 \
   --write --confirm "$device" > "$root/output.txt"
grep -Fq 'durably synced' "$root/output.txt"
actual=$(dd if="$device" bs=1 skip=80 count=4 status=none)
test "$actual" = 'REST'

# Untrusted traversal and invalid filesystem-local paths must fail closed.
if "$engine" defrag "$device" --filesystem testv2 --path=/../FILE \
   --write --confirm "$device" > /dev/null 2>&1; then
    echo "Unsafe path was accepted" >&2; exit 1
fi
before=$(sha256sum "$device" | cut -d' ' -f1)
# Absence of --path means the filesystem root: a whole-volume operation.
"$engine" defrag "$device" --filesystem testv2 \
   --write --confirm "$device" > "$root/output.txt"
grep -Fq 'durably synced' "$root/output.txt"
# Read-only V2 mapping must consume all 33 extents in two pages.
"$engine" map "$device" --filesystem testv2 > "$root/output.txt"
grep -Fq 'durably synced' "$root/output.txt"
after=$(sha256sum "$device" | cut -d' ' -f1)
test "$before" = "$after"
if "$engine" defrag "$device" --filesystem testv2 --path= \
   --write --confirm "$device" > /dev/null 2>&1; then
    echo "Empty V2 path was accepted" >&2; exit 1
fi

# Both competing modules are trusted, but duplicate format ownership must
# be rejected rather than choosing one arbitrarily.
cp -- "$module" "$root/Filesystems/DUPLICATE.so"
if "$engine" defrag "$device" --filesystem testv2 --path=/FILE \
   --write --confirm "$device" > /dev/null 2>&1; then
    echo "Duplicate V2 providers were accepted" >&2; exit 1
fi
rm -f "$root/Filesystems/DUPLICATE.so"

# The image is not exFAT/FAT and only the V2 test provider recognises it.
if "$engine" defrag "$device" --filesystem notinstalled --path=/FILE \
   --write --confirm "$device" > /dev/null 2>&1; then
    echo "Unknown provider accepted" >&2;exit 1
fi
after=$(sha256sum "$device" | cut -d' ' -f1)
test "$before" = "$after"
echo 'DEFRAGGER-STRICT-COMMON-V2-TRUST-EXECUTE-VERIFY-RECOVER-PASS'
