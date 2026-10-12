#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

profile="${1:-}"
case "$profile" in
  quality)
    commands=(git cmake cc c++ make dpkg-deb pkg-config zip unzip python3 makefs udisksctl pkexec xvfb-run Xvfb xauth npx sha256sum)
    packages=(gtk+-3.0 ext2fs sqlite3 openssl)
    require_python_gtk=true
    ;;
  sanitizers)
    commands=(git cmake cc c++ make pkg-config python3)
    packages=(gtk+-3.0 ext2fs sqlite3 openssl)
    require_python_gtk=false
    ;;
  release)
    commands=(git jq curl sha256sum dpkg-deb gzip awk)
    packages=()
    require_python_gtk=false
    ;;
  *)
    echo "usage: $0 {quality|sanitizers|release}" >&2
    exit 2
    ;;
esac

missing=()
for command_name in "${commands[@]}"; do
  command -v "$command_name" >/dev/null 2>&1 || missing+=("$command_name")
done

missing_packages=()
for package_name in "${packages[@]}"; do
  pkg-config --exists "$package_name" 2>/dev/null || missing_packages+=("$package_name")
done

python_gtk_missing=false
if [[ "$require_python_gtk" == true ]] && ! python3 -c 'import gi, cairo' >/dev/null 2>&1; then
  python_gtk_missing=true
fi

if ((${#missing[@]} || ${#missing_packages[@]})) || [[ "$python_gtk_missing" == true ]]; then
  echo "Self-hosted ${profile} runner is missing required dependencies." >&2
  ((${#missing[@]})) && printf 'Missing commands: %s\n' "${missing[*]}" >&2 || true
  ((${#missing_packages[@]})) && printf 'Missing pkg-config packages: %s\n' "${missing_packages[*]}" >&2 || true
  [[ "$python_gtk_missing" == true ]] && echo 'Missing Python GTK bindings (gi, cairo).' >&2 || true
  exit 1
fi
