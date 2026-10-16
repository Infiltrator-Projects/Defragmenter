# SPDX-License-Identifier: GPL-3.0-or-later

# UFS1/UFS2 analysis and the qualified bounded mutation path are native C.
# The writer remains fail-closed outside clean, non-journalled, snapshot-free
# layouts whose allocation and regular-file block trees are fully understood.
add_library(defragmenter-ufs-native STATIC
    gui/filesystems/ufs/native/ufs_native.c)
target_include_directories(defragmenter-ufs-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ufs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-ufs-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-ufs-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-ufs-native PUBLIC defragmenter-core)

add_executable(defragmenter-ufs-worker
    gui/filesystems/ufs/native/ufs_worker.c)
target_include_directories(defragmenter-ufs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ufs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-ufs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-ufs-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-ufs-worker PRIVATE
    defragmenter-ufs-native defragmenter-core OpenSSL::Crypto)

install(TARGETS defragmenter-ufs-worker
        RUNTIME DESTINATION lib/defragmenter/filesystems/ufs)

if(BUILD_TESTING)
    add_executable(defragmenter-ufs-native-test tests/test_ufs_native.c)
    target_include_directories(defragmenter-ufs-native-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ufs/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-ufs-native-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-ufs-native-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-ufs-native-test PRIVATE
        defragmenter-ufs-native defragmenter-core)
    add_test(NAME defragmenter-ufs-native COMMAND defragmenter-ufs-native-test)
endif()
