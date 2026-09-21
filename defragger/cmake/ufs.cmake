# SPDX-License-Identifier: GPL-3.0-or-later

# UFS1/UFS2 exact allocation/inode-tree analysis and bounded recoverable
# offline mutation are native C. Python remains only GUI/backend glue.
add_library(linux-defragger-ufs-native STATIC
    gui/filesystems/ufs/native/ufs_native.c)
target_include_directories(linux-defragger-ufs-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ufs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-ufs-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-ufs-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-ufs-native PUBLIC linux-defragger-core)

add_executable(linux-defragger-ufs-worker
    gui/filesystems/ufs/native/ufs_worker.c)
target_include_directories(linux-defragger-ufs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ufs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-ufs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-ufs-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-ufs-worker PRIVATE
    linux-defragger-ufs-native linux-defragger-core OpenSSL::Crypto)

install(TARGETS linux-defragger-ufs-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/ufs)

if(BUILD_TESTING)
    add_executable(linux-defragger-ufs-native-test tests/test_ufs_native.c)
    target_include_directories(linux-defragger-ufs-native-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ufs/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-ufs-native-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-ufs-native-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-ufs-native-test PRIVATE
        linux-defragger-ufs-native linux-defragger-core)
    add_test(NAME linux-defragger-ufs-native COMMAND linux-defragger-ufs-native-test)
endif()
