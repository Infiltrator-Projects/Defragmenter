# SPDX-License-Identifier: GPL-3.0-or-later

# Classic HFS keeps its compatibility binary name while the target now owns
# exact analysis plus bounded Defragment/Growth Defrag/Recover.
if(BUILD_TESTING)
    add_executable(defragmenter-hfs-native-test tests/test_hfs_worker.c)
    target_compile_options(defragmenter-hfs-native-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-hfs-native-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    add_test(NAME defragmenter-hfs-native
        COMMAND defragmenter-hfs-native-test
                $<TARGET_FILE:defragmenter-hfs-worker>)
endif()
