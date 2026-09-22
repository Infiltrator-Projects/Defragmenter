# SPDX-License-Identifier: GPL-3.0-or-later

# ZFS/OpenZFS member identification, MOS traversal and exact bounded mapping are native C.
add_library(linux-defragger-zfs-native STATIC
    gui/filesystems/zfs/native/zfs_native.c
    gui/filesystems/zfs/native/zfs_analysis.c)
target_include_directories(linux-defragger-zfs-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/zfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-zfs-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-zfs-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-zfs-native PUBLIC linux-defragger-core)

add_executable(linux-defragger-zfs-worker
    gui/filesystems/zfs/native/zfs_worker.c)
target_include_directories(linux-defragger-zfs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/zfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-zfs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-zfs-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-zfs-worker PRIVATE
    linux-defragger-zfs-native linux-defragger-core)

install(TARGETS linux-defragger-zfs-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/zfs)

if(BUILD_TESTING)
    add_executable(linux-defragger-zfs-native-test tests/test_zfs_native.c)
    target_include_directories(linux-defragger-zfs-native-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/zfs/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-zfs-native-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-zfs-native-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-zfs-native-test PRIVATE
        linux-defragger-zfs-native linux-defragger-core)
    add_test(NAME linux-defragger-zfs-native COMMAND linux-defragger-zfs-native-test)
endif()
