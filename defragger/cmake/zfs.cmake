# SPDX-License-Identifier: GPL-3.0-or-later

# ZFS/OpenZFS identification and bounded exact read-only MOS/allocation/fragmentation analysis are native C.
add_library(defragmenter-zfs-native STATIC
    gui/filesystems/zfs/native/zfs_native.c
    gui/filesystems/zfs/native/zfs_analysis.c)
target_include_directories(defragmenter-zfs-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/zfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-zfs-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-zfs-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-zfs-native PUBLIC
    defragmenter-core OpenSSL::Crypto)

add_executable(defragmenter-zfs-worker
    gui/filesystems/zfs/native/zfs_worker.c)
target_include_directories(defragmenter-zfs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/zfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-zfs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-zfs-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-zfs-worker PRIVATE
    defragmenter-zfs-native defragmenter-core)

install(TARGETS defragmenter-zfs-worker
        RUNTIME DESTINATION /System/Filesystems/Defragmenter/zfs)

if(BUILD_TESTING)
    add_executable(defragmenter-zfs-native-test tests/test_zfs_native.c)
    target_include_directories(defragmenter-zfs-native-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/zfs/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-zfs-native-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-zfs-native-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-zfs-native-test PRIVATE
        defragmenter-zfs-native defragmenter-core)
    add_test(NAME defragmenter-zfs-native COMMAND defragmenter-zfs-native-test)
endif()
