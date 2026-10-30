# SPDX-License-Identifier: GPL-3.0-or-later
if(BUILD_TESTING)
    add_test(
        NAME defragmenter-exfat-relayout
        COMMAND bash "${CMAKE_CURRENT_LIST_DIR}/../tests/test_exfat_relayout_engine.sh"
                "${EXFAT_WORKER_FOR_TESTS}")
    set_tests_properties(defragmenter-exfat-relayout PROPERTIES
        ENVIRONMENT
            "PYTHONDONTWRITEBYTECODE=1")
endif()
