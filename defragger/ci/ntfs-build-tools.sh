#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Use the runner's authenticated APT cache to unpack only the NTFS build and
# verification files. This avoids installing packages or mounting via FUSE.
ntfs_dependencies=$(mktemp -d "$RUNNER_TEMP/ntfs-dependencies.XXXXXXXX")
(
    cd "$ntfs_dependencies"
    library_package=$(apt-cache depends ntfs-3g-dev |
        awk '$1 == "Depends:" && $2 ~ /^libntfs-3g[0-9]/ { print $2; exit }')
    [[ -n $library_package ]]
    apt-get download ntfs-3g-dev ntfs-3g "$library_package"
    for package in ./*.deb; do dpkg-deb -x "$package" root; done
)
ntfs_root="$ntfs_dependencies/root"
ntfs_library=$(find "$ntfs_root/usr/lib" -name libntfs-3g.so -print -quit)
[[ -f $ntfs_library && -r "$ntfs_root/usr/include/ntfs-3g/device.h" ]]
ntfs_cmake_args=(
    "-DNTFS3G_INCLUDE_DIR=$ntfs_root/usr/include"
    "-DNTFS3G_LIBRARY=$ntfs_library"
)
export PATH="$ntfs_root/usr/bin:$ntfs_root/usr/sbin:$ntfs_root/bin:$ntfs_root/sbin:$PATH"
export LD_LIBRARY_PATH="$(dirname "$ntfs_library")${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# CTest's standalone tools and later consumers of NTFS.so need the same library.
printf 'LD_LIBRARY_PATH=%s\n' "$LD_LIBRARY_PATH" >> "$GITHUB_ENV"
