#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Verify that an older driver's typed service table is exactly the shared
# V1 prefix. Later filesystem workers extend this append-only host ABI with target-safety hooks.
set -euo pipefail
[[ $# -eq 1 ]] || { echo "usage: verify_legacy_fs_services_abi.sh older-header" >&2; exit 2; }
file=$1
[[ -f "$file" ]] || { echo "Missing host services header: $file" >&2; exit 1; }
work=$(mktemp)
trap 'rm -f "$work"' EXIT
awk '/    \/\* XFS workers require these additional filesystem-neutral safety hooks\./ {exit} {print}'     native/ifs_defrag_services_v1.h > "$work"
printf '} IfsDefragServicesV1;\n#endif\n' >> "$work"
# The addition of ld_device.h is a later extension's type dependency; remove
# that single include while comparing original legacy prefix declarations.
sed -i '/^#include "ld_device.h"$/d' "$work"
cmp "$work" "$file"
