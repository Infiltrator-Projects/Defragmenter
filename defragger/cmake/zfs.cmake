# SPDX-License-Identifier: GPL-3.0-or-later
# ZFS's complete native parser, allocation engine and Test Media formatter
# live exclusively in installed ZFS.so. The generic host owns trusted loading.
add_executable(defragmenter-zfs-worker native/zfs_module_loader.c)
target_include_directories(defragmenter-zfs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-zfs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-zfs-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-zfs-worker PRIVATE defragmenter-filesystem-module-host)
install(TARGETS defragmenter-zfs-worker
    RUNTIME DESTINATION /System/Commands/Defragmenter/zfs)

# The isolated worker uses the existing test-only Common module host.
if(BUILD_TESTING AND OFS_DEFRAG_TEST_MODULE_PARENT)
    add_executable(defragmenter-zfs-test-worker native/zfs_module_loader.c)
    target_include_directories(defragmenter-zfs-test-worker PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-zfs-test-worker PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-zfs-test-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-zfs-test-worker PRIVATE
        defragmenter-filesystem-module-test-host)
endif()
