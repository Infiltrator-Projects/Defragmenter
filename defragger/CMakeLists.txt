# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(linux_defragger VERSION 1.8.0 LANGUAGES C CXX)

# Keep the established project/build graph isolated from filesystem-specific
# native modules so new native engines can be added without duplicating the registry.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/project.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/hfs_affs_safety.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/btrfs.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/ext_metadata_map.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/fat_relayout.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/exfat_relayout.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/ufs.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/zfs.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/hfs.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/sfs.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/pfs3.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/test_media.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/gui_regressions.cmake")
