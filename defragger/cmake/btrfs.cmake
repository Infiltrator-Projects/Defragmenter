# SPDX-License-Identifier: GPL-3.0-or-later

add_library(defragmenter-btrfs-native STATIC
    gui/filesystems/btrfs/native/btrfs_native.c)
target_include_directories(defragmenter-btrfs-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/btrfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-btrfs-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-btrfs-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-btrfs-native PUBLIC defragmenter-core)

add_executable(defragmenter-btrfs-worker
    gui/filesystems/btrfs/native/btrfs_worker.c)
target_include_directories(defragmenter-btrfs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/btrfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-btrfs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-btrfs-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-btrfs-worker PRIVATE
    defragmenter-btrfs-native defragmenter-core OpenSSL::Crypto)

install(TARGETS defragmenter-btrfs-worker
        RUNTIME DESTINATION /System/Commands/Defragmenter/btrfs)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME defragmenter-btrfs-native-integration
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_btrfs_native.py")
    set_tests_properties(defragmenter-btrfs-native-integration PROPERTIES
        ENVIRONMENT
            "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/gui:${CMAKE_CURRENT_SOURCE_DIR}/tests;DEFRAGMENTER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR};DEFRAGMENTER_BTRFS_WORKER=${CMAKE_CURRENT_BINARY_DIR}/defragmenter-btrfs-worker")
endif()
