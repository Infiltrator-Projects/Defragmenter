#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

profile="${1:-}"
case "$profile" in
  quality)
    commands=(git cmake cc c++ make dpkg-deb pkg-config python3 udisksctl pkexec xvfb-run Xvfb xauth sha256sum jq gzip awk)
    packages=(gtk+-3.0 ext2fs sqlite3 openssl)
    ;;
  sanitizers)
    commands=(git cmake cc c++ make pkg-config python3)
    packages=(gtk+-3.0 ext2fs sqlite3 openssl)
    ;;
  release)
    commands=(git jq curl sha256sum dpkg-deb gzip awk)
    packages=()
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

if ((${#missing[@]} || ${#missing_packages[@]})); then
  echo "Self-hosted ${profile} runner is missing required dependencies." >&2
  ((${#missing[@]})) && printf 'Missing commands: %s\n' "${missing[*]}" >&2 || true
  ((${#missing_packages[@]})) && printf 'Missing pkg-config packages: %s\n' "${missing_packages[*]}" >&2 || true
  exit 1
fi
