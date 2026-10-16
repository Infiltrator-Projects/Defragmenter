# SPDX-License-Identifier: GPL-3.0-or-later
# Focused integration coverage for the shared FAT12/FAT16/FAT32 relayout engine.

if(BUILD_TESTING AND TARGET defragmenter-fat-worker)
    add_test(
        NAME defragmenter-fat-relayout
        COMMAND bash
                "${CMAKE_CURRENT_LIST_DIR}/../tests/test_fat_relayout_engine.sh"
                "$<TARGET_FILE:defragmenter-fat-worker>"
    )
    set_tests_properties(
        defragmenter-fat-relayout
        PROPERTIES
            TIMEOUT 180
            ENVIRONMENT
                "PYTHONDONTWRITEBYTECODE=1"
    )
endif()
