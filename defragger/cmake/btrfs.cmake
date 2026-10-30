# SPDX-License-Identifier: GPL-3.0-or-later
# Btrfs on-media parsing, relocation, journals, and Test Media creation
# are solely owned by BTRFS.so.
add_executable(defragmenter-btrfs-worker native/btrfs_module_loader.c)
target_include_directories(defragmenter-btrfs-worker PRIVATE
 "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-btrfs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-btrfs-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-btrfs-worker PRIVATE
 defragmenter-fs-host-services defragmenter-filesystem-module-host)
if(BUILD_TESTING AND TARGET defragmenter-filesystem-module-test-host)
 add_executable(defragmenter-btrfs-test-worker native/btrfs_module_loader.c)
 target_include_directories(defragmenter-btrfs-test-worker PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
 target_compile_options(defragmenter-btrfs-test-worker PRIVATE ${LD_WARNING_FLAGS})
 target_compile_definitions(defragmenter-btrfs-test-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
 target_link_libraries(defragmenter-btrfs-test-worker PRIVATE
  defragmenter-fs-host-services defragmenter-filesystem-module-test-host)
endif()
install(TARGETS defragmenter-btrfs-worker RUNTIME DESTINATION /System/Commands/Defragmenter/btrfs)
if(BUILD_TESTING)
 find_package(Python3 REQUIRED COMPONENTS Interpreter)
 add_test(NAME defragmenter-btrfs-native-integration
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_btrfs_native.py")
 if(TARGET defragmenter-btrfs-test-worker)
  set(BTRFS_TEST_WORKER "$<TARGET_FILE:defragmenter-btrfs-test-worker>")
 else()
  set(BTRFS_TEST_WORKER "$<TARGET_FILE:defragmenter-btrfs-worker>")
 endif()
 set_tests_properties(defragmenter-btrfs-native-integration PROPERTIES
  ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/gui:${CMAKE_CURRENT_SOURCE_DIR}/tests;DEFRAGMENTER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR};DEFRAGMENTER_BTRFS_WORKER=${BTRFS_TEST_WORKER}")
endif()
