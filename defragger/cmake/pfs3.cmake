# SPDX-License-Identifier: GPL-3.0-or-later
add_library(linux-defragger-pfs3-native STATIC
    gui/filesystems/pfs3/native/pfs3_native.c)
target_include_directories(linux-defragger-pfs3-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/pfs3/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-pfs3-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-pfs3-native PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-pfs3-native PUBLIC linux-defragger-core)

add_executable(linux-defragger-pfs3-worker gui/filesystems/pfs3/native/pfs3_worker.c)
target_include_directories(linux-defragger-pfs3-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/pfs3/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-pfs3-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-pfs3-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-pfs3-worker PRIVATE
    linux-defragger-pfs3-native linux-defragger-core OpenSSL::Crypto)
install(TARGETS linux-defragger-pfs3-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/pfs3)

if(BUILD_TESTING)
    add_executable(linux-defragger-pfs3-native-test tests/test_pfs3_native.c)
    target_include_directories(linux-defragger-pfs3-native-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/pfs3/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-pfs3-native-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-pfs3-native-test PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-pfs3-native-test PRIVATE
        linux-defragger-pfs3-native linux-defragger-core)
    add_test(NAME linux-defragger-pfs3-native COMMAND linux-defragger-pfs3-native-test)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME linux-defragger-pfs3-transaction
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pfs3_transaction.py")
    set_tests_properties(linux-defragger-pfs3-transaction PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;LINUX_DEFRAGGER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}")
endif()
