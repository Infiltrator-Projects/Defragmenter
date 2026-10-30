# SPDX-License-Identifier: GPL-3.0-or-later
# UFS1/UFS2 relocation, analysis and recovery run exclusively through UFS1.so and UFS2.so.
# All production UFS operations, including Test Media format validation,
# are dispatched through the installed UFS1.so and UFS2.so. No local UFS codec is linked.
# CI/release must fail if a filesystem implementation is later reintroduced.
file(GLOB_RECURSE LD_EMBEDDED_UFS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ufs/native/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ufs/native/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_media/test_media_raw_ufs.c")
if(LD_EMBEDDED_UFS_SOURCES)
    message(FATAL_ERROR
        "UFS filesystem implementation belongs in Filesystem-UFS1/UFS1.so or Filesystem-UFS2/UFS2.so, not Defragmenter")
endif()
add_executable(defragmenter-ufs-worker native/ufs_module_loader.c)
target_include_directories(defragmenter-ufs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-ufs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-ufs-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-ufs-worker PRIVATE
    defragmenter-fs-host-services defragmenter-filesystem-module-host)
if(BUILD_TESTING AND OFS_DEFRAG_TEST_MODULE_PARENT)
    add_executable(defragmenter-ufs-test-worker native/ufs_module_loader.c)
    target_include_directories(defragmenter-ufs-test-worker PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-ufs-test-worker PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-ufs-test-worker PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-ufs-test-worker PRIVATE
        defragmenter-fs-host-services defragmenter-filesystem-module-test-host)
endif()
install(TARGETS defragmenter-ufs-worker
    RUNTIME DESTINATION /System/Commands/Defragmenter/ufs)
