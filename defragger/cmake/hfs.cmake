# SPDX-License-Identifier: GPL-3.0-or-later

# Classic HFS keeps its compatibility binary name while the target now owns
# exact analysis plus bounded Defragment/Growth Defrag/Recover.
if(BUILD_TESTING)
    add_executable(defragmenter-hfs-native-test tests/test_hfs_worker.c)
    target_compile_options(defragmenter-hfs-native-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-hfs-native-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    set(HFS_WORKER_FOR_TESTS "$<TARGET_FILE:defragmenter-hfs-worker>")
    if(TARGET defragmenter-hfs-test-worker)
        set(HFS_WORKER_FOR_TESTS "$<TARGET_FILE:defragmenter-hfs-test-worker>")
    endif()
    add_test(NAME defragmenter-hfs-native
        COMMAND defragmenter-hfs-native-test
                "${HFS_WORKER_FOR_TESTS}")
    if(OFS_DEFRAG_TEST_MODULE_PARENT)
        set_tests_properties(defragmenter-hfs-native PROPERTIES
            ENVIRONMENT "IFS_TEST_MODULE_PARENT=${OFS_DEFRAG_TEST_MODULE_PARENT}")
    endif()
endif()
