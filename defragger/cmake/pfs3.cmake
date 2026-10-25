# SPDX-License-Identifier: GPL-3.0-or-later
add_library(defragmenter-pfs3-native STATIC
    gui/filesystems/pfs3/native/pfs3_native.c)
target_include_directories(defragmenter-pfs3-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/pfs3/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-pfs3-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-pfs3-native PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-pfs3-native PUBLIC defragmenter-core)

add_executable(defragmenter-pfs3-worker gui/filesystems/pfs3/native/pfs3_worker.c)
target_include_directories(defragmenter-pfs3-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/pfs3/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-pfs3-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-pfs3-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-pfs3-worker PRIVATE
    defragmenter-pfs3-native defragmenter-core OpenSSL::Crypto)
install(TARGETS defragmenter-pfs3-worker
        RUNTIME DESTINATION /System/Filesystems/Defragmenter/pfs3)

if(BUILD_TESTING)
    add_executable(defragmenter-pfs3-native-test tests/test_pfs3_native.c)
    target_include_directories(defragmenter-pfs3-native-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/pfs3/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-pfs3-native-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-pfs3-native-test PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-pfs3-native-test PRIVATE
        defragmenter-pfs3-native defragmenter-core)
    add_test(NAME defragmenter-pfs3-native COMMAND defragmenter-pfs3-native-test)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME defragmenter-pfs3-transaction
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pfs3_transaction.py")
    set_tests_properties(defragmenter-pfs3-transaction PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;DEFRAGMENTER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}")
endif()
