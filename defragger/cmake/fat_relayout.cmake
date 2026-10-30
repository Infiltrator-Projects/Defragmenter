# SPDX-License-Identifier: GPL-3.0-or-later
# The application cannot ship another FAT12, FAT16 or FAT32 filesystem
# engine: formatting, allocation, placement and relayout belong in FAT*.so.
file(GLOB_RECURSE LD_PRIVATE_FAT_ENGINES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/fat*/native/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/fat*/native/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_media/test_media_raw_fat.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_media/test_media_fat16.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_media/test_media_fat32.c")
if(LD_PRIVATE_FAT_ENGINES)
    message(FATAL_ERROR
        "FAT12/16/32 implementation must live in its authoritative FAT*.so")
endif()

# Focused integration coverage for the shared FAT12/FAT16/FAT32 relayout engine.

if(BUILD_TESTING AND TARGET defragmenter-fat-worker)
    add_test(NAME defragmenter-fat-module-architecture
        COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/../tests/test_fat_relayout_architecture.py")
    add_test(
        NAME defragmenter-fat-relayout
        COMMAND bash
                "${CMAKE_CURRENT_LIST_DIR}/../tests/test_fat_relayout_engine.sh"
                "${FAT_WORKER_FOR_TESTS}"
    )
    set_tests_properties(
        defragmenter-fat-relayout
        PROPERTIES
            TIMEOUT 180
            ENVIRONMENT
                "PYTHONDONTWRITEBYTECODE=1"
    )
endif()
