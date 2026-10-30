# SPDX-License-Identifier: GPL-3.0-or-later
# All PFS30/PFS32 native format and recovery logic is owned by Filesystem-PFS3.
# Defragmenter uses only its standard trusted module host.
add_executable(defragmenter-pfs3-worker native/pfs3_module_loader.c)
target_include_directories(defragmenter-pfs3-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-pfs3-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-pfs3-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-pfs3-worker PRIVATE
    defragmenter-fs-host-services defragmenter-filesystem-module-host)
install(TARGETS defragmenter-pfs3-worker
    RUNTIME DESTINATION /System/Commands/Defragmenter/pfs3)
if(BUILD_TESTING AND OFS_DEFRAG_TEST_MODULE_PARENT)
    add_executable(defragmenter-pfs3-test-worker native/pfs3_module_loader.c)
    target_include_directories(defragmenter-pfs3-test-worker PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-pfs3-test-worker PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-pfs3-test-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-pfs3-test-worker PRIVATE
        defragmenter-fs-host-services defragmenter-filesystem-module-test-host)
endif()
# The original regression test compiles the authoritative source only for
# independent destructive fixtures, never into a distributed Defragmenter.
set(PFS3_NATIVE_TEST_SOURCE_DIR "" CACHE PATH "Qualified PFS3.so source checkout for fixture tests")
if(BUILD_TESTING AND PFS3_NATIVE_TEST_SOURCE_DIR)
    add_library(defragmenter-pfs3-fixture-native STATIC
        "${PFS3_NATIVE_TEST_SOURCE_DIR}/native/pfs3_native.c")
    target_include_directories(defragmenter-pfs3-fixture-native PUBLIC
        "${PFS3_NATIVE_TEST_SOURCE_DIR}/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
    target_compile_options(defragmenter-pfs3-fixture-native PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-pfs3-fixture-native PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-pfs3-fixture-native PUBLIC defragmenter-core)
    add_executable(defragmenter-pfs3-native-test tests/test_pfs3_native.c)
    target_include_directories(defragmenter-pfs3-native-test PRIVATE
        "${PFS3_NATIVE_TEST_SOURCE_DIR}/native")
    target_compile_options(defragmenter-pfs3-native-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-pfs3-native-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-pfs3-native-test PRIVATE
        defragmenter-pfs3-fixture-native defragmenter-core)
    add_test(NAME defragmenter-pfs3-native COMMAND defragmenter-pfs3-native-test)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME defragmenter-pfs3-transaction
        COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pfs3_transaction.py")
    set_tests_properties(defragmenter-pfs3-transaction PROPERTIES
        ENVIRONMENT
          "PYTHONDONTWRITEBYTECODE=1;DEFRAGMENTER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}")
endif()
