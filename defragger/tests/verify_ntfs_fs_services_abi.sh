#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Verify the NTFS host callback prefix is unchanged when XFS adds append-only
# host services. Disallow accidental reordering of NTFS safety operations.
set -euo pipefail
[[ $# -eq 1 ]] || exit 2
f=$1
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
awk '/    \/\* Appended filesystem-neutral XFS safety and allocation callbacks\./ {exit} {print}'   native/ifs_defrag_services_v1.h > "$tmp"
printf '} IfsDefragServicesV1;\n#endif\n' >> "$tmp"
cmp "$tmp" "$f"
