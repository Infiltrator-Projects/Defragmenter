# SPDX-License-Identifier: GPL-3.0-or-later

add_library(linux-defragger-btrfs-native STATIC
    gui/filesystems/btrfs/native/btrfs_native.c)
target_include_directories(linux-defragger-btrfs-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/btrfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-btrfs-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-btrfs-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-btrfs-native PUBLIC linux-defragger-core)

add_executable(linux-defragger-btrfs-worker
    gui/filesystems/btrfs/native/btrfs_worker.c)
target_include_directories(linux-defragger-btrfs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/btrfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-btrfs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-btrfs-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-btrfs-worker PRIVATE
    linux-defragger-btrfs-native linux-defragger-core OpenSSL::Crypto)

install(TARGETS linux-defragger-btrfs-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/btrfs)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME linux-defragger-btrfs-native-python
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_btrfs_native.py")
    set_tests_properties(linux-defragger-btrfs-native-python PROPERTIES
        ENVIRONMENT
            "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/gui:${CMAKE_CURRENT_SOURCE_DIR}/tests;LINUX_DEFRAGGER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR};LINUX_DEFRAGGER_BTRFS_WORKER=${CMAKE_CURRENT_BINARY_DIR}/linux-defragger-btrfs-worker")
endif()
