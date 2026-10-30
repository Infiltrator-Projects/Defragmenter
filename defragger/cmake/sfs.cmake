# SPDX-License-Identifier: GPL-3.0-or-later
# All SFS0/SFS2 native format and recovery logic is owned by Filesystem-SFS.
# Defragmenter uses only its standard trusted module host.
add_executable(defragmenter-sfs-worker native/sfs_module_loader.c)
target_include_directories(defragmenter-sfs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-sfs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-sfs-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-sfs-worker PRIVATE
    defragmenter-fs-host-services defragmenter-filesystem-module-host)
install(TARGETS defragmenter-sfs-worker
    RUNTIME DESTINATION /System/Commands/Defragmenter/sfs)
if(BUILD_TESTING AND OFS_DEFRAG_TEST_MODULE_PARENT)
    add_executable(defragmenter-sfs-test-worker native/sfs_module_loader.c)
    target_include_directories(defragmenter-sfs-test-worker PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-sfs-test-worker PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-sfs-test-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-sfs-test-worker PRIVATE
        defragmenter-fs-host-services defragmenter-filesystem-module-test-host)
endif()
# The original regression test compiles the authoritative source only for
# independent destructive fixtures, never into a distributed Defragmenter.
set(SFS_NATIVE_TEST_SOURCE_DIR "" CACHE PATH "Qualified SFS.so source checkout for fixture tests")
if(BUILD_TESTING AND SFS_NATIVE_TEST_SOURCE_DIR)
    add_library(defragmenter-sfs-fixture-native STATIC
        "${SFS_NATIVE_TEST_SOURCE_DIR}/native/sfs_native.c")
    target_include_directories(defragmenter-sfs-fixture-native PUBLIC
        "${SFS_NATIVE_TEST_SOURCE_DIR}/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
    target_compile_options(defragmenter-sfs-fixture-native PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-sfs-fixture-native PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-sfs-fixture-native PUBLIC defragmenter-core)
    add_executable(defragmenter-sfs-native-test tests/test_sfs_native.c)
    target_include_directories(defragmenter-sfs-native-test PRIVATE
        "${SFS_NATIVE_TEST_SOURCE_DIR}/native")
    target_compile_options(defragmenter-sfs-native-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-sfs-native-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-sfs-native-test PRIVATE
        defragmenter-sfs-fixture-native defragmenter-core)
    add_test(NAME defragmenter-sfs-native COMMAND defragmenter-sfs-native-test)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME defragmenter-sfs-transaction
        COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sfs_transaction.py")
    set_tests_properties(defragmenter-sfs-transaction PROPERTIES
        ENVIRONMENT
          "PYTHONDONTWRITEBYTECODE=1;DEFRAGMENTER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}")
endif()
