# SPDX-License-Identifier: GPL-3.0-or-later

# Amiga Smart File System (SFS0) raw allocation analysis and offline relocation.
# Filesystem parsing, staging, commit and recovery are native C; the installed
# GTK application consumes the native worker protocol directly.
add_library(defragmenter-sfs-native STATIC
    gui/filesystems/sfs/native/sfs_native.c)
target_include_directories(defragmenter-sfs-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/sfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-sfs-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-sfs-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-sfs-native PUBLIC defragmenter-core)

add_executable(defragmenter-sfs-worker
    gui/filesystems/sfs/native/sfs_worker.c)
target_include_directories(defragmenter-sfs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/sfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-sfs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-sfs-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-sfs-worker PRIVATE
    defragmenter-sfs-native defragmenter-core OpenSSL::Crypto)

install(TARGETS defragmenter-sfs-worker
        RUNTIME DESTINATION /System/Filesystems/Defragmenter/sfs)

if(BUILD_TESTING)
    add_executable(defragmenter-sfs-native-test
        tests/test_sfs_native.c)
    target_include_directories(defragmenter-sfs-native-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/sfs/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-sfs-native-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-sfs-native-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-sfs-native-test PRIVATE
        defragmenter-sfs-native defragmenter-core)
    add_test(NAME defragmenter-sfs-native
             COMMAND defragmenter-sfs-native-test)

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME defragmenter-sfs-transaction
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sfs_transaction.py")
    set_tests_properties(defragmenter-sfs-transaction PROPERTIES
        ENVIRONMENT
            "PYTHONDONTWRITEBYTECODE=1;DEFRAGMENTER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}")
endif()
