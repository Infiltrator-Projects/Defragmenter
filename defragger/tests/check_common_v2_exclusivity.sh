#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Production accepts ONLY the canonical Common V2 filesystem ABI.
# Negative fixture deliberately exports non-V2 symbols to prove refusal.
set -euo pipefail

engine=$(realpath "${1:?test operation engine required}")
source_root=$(realpath "${2:?Defragmenter source root required}")
test -x "$engine"
test -d "$source_root/native"
test -d "$source_root/test_media"

# No production compilation unit may include, resolve or invoke a retired
# per-format V1 provider or a V5 filesystem bridge.
old_pattern='#[[:space:]]*include[[:space:]]*["<][^">]*(v1|v5)|ifs_defrag_services_v1|infiltrator_filesystem_[[:alnum:]_]*_v1|infiltrator_filesystem_module_v5|IFS_[[:upper:]_0-9]+_V1|infiltrator_filesystem_defrag_v2'
for dir in native test_media src; do
    if grep -RInE --include='*.c' --include='*.h' --include='*.cpp' \
         --include='*.hpp' "$old_pattern" "$source_root/$dir"; then
        echo "Rejected: production source retains a non-Common V2 interface" >&2
        exit 1
    fi
done
if find "$source_root/native" "$source_root/test_media" -type f \
    \( -iname '*_v1.*' -o -iname '*_v5.*' \) | grep .; then
    echo "Rejected: a retired filesystem ABI header remains" >&2
    exit 1
fi

temp=$(mktemp -d)
trap 'rm -rf -- "$temp"' EXIT
chmod 700 "$temp"
mkdir -m 700 "$temp/Filesystems"
# Exactly the symbol shapes formerly used by EXT and exFAT, with no Common V2.
# This object must be invisible to discovery and impossible to invoke.
cat > "$temp/non_v2.c" <<'C'
#include <stdint.h>
typedef struct { uint32_t abi_version; uint32_t structure_bytes; } OldApi;
static const OldApi old_api = {1u, sizeof(OldApi)};
__attribute__((visibility("default")))
const OldApi *infiltrator_filesystem_ext_v1(void) { return &old_api; }
__attribute__((visibility("default")))
const OldApi *infiltrator_filesystem_exfat_defrag_v1(void) { return &old_api; }
__attribute__((visibility("default")))
const OldApi *infiltrator_filesystem_module_v5(void) { return &old_api; }
C
cc -std=c11 -O2 -Wall -Wextra -Werror -fPIC -shared \
  "$temp/non_v2.c" -o "$temp/Filesystems/EXT4.so"
chmod 600 "$temp/Filesystems/EXT4.so"
export IFS_TEST_MODULE_PARENT="$temp"
"$engine" --list-installed-modules > "$temp/inventory.json"
if grep -Eq '"format_id"[[:space:]]*:[[:space:]]*"ext4"|"filename"[[:space:]]*:[[:space:]]*"EXT4.so"' \
    "$temp/inventory.json"; then
    echo "A private V1/V5-only provider was recognized" >&2
    exit 1
fi
image="$temp/sample.img"
truncate -s 1048576 "$image"
printf 'TESTV2FS' | dd of="$image" bs=1 conv=notrunc status=none
before=$(sha256sum "$image" | cut -d' ' -f1)
if "$engine" map "$image" --filesystem ext4 --path=/FILE >/dev/null 2>&1; then
    echo "V1-only provider accepted for a read-only operation" >&2
    exit 1
fi
if "$engine" defrag "$image" --filesystem ext4 --path=/FILE \
  --write --confirm "$image" >/dev/null 2>&1; then
    echo "V1-only provider accepted for mutation" >&2
    exit 1
fi
test "$before" = "$(sha256sum "$image" | cut -d' ' -f1)"
echo 'DEFRAGGER-COMMON-V2-ONLY-RETIRED-PROVIDER-REJECTION-PASS'
