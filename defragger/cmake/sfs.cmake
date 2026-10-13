# SPDX-License-Identifier: GPL-3.0-or-later

# Amiga Smart File System (SFS0) raw allocation analysis and offline relocation.
# Filesystem parsing, staging, commit and recovery are native C; the installed
# GTK application consumes the native worker protocol directly.
add_library(linux-defragger-sfs-native STATIC
    gui/filesystems/sfs/native/sfs_native.c)
target_include_directories(linux-defragger-sfs-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/sfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-sfs-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-sfs-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-sfs-native PUBLIC linux-defragger-core)

add_executable(linux-defragger-sfs-worker
    gui/filesystems/sfs/native/sfs_worker.c)
target_include_directories(linux-defragger-sfs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/sfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-sfs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-sfs-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-sfs-worker PRIVATE
    linux-defragger-sfs-native linux-defragger-core OpenSSL::Crypto)

install(TARGETS linux-defragger-sfs-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/sfs)

if(BUILD_TESTING)
    add_executable(linux-defragger-sfs-native-test
        tests/test_sfs_native.c)
    target_include_directories(linux-defragger-sfs-native-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/sfs/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-sfs-native-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-sfs-native-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-sfs-native-test PRIVATE
        linux-defragger-sfs-native linux-defragger-core)
    add_test(NAME linux-defragger-sfs-native
             COMMAND linux-defragger-sfs-native-test)

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME linux-defragger-sfs-transaction
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sfs_transaction.py")
    set_tests_properties(linux-defragger-sfs-transaction PROPERTIES
        ENVIRONMENT
            "PYTHONDONTWRITEBYTECODE=1;LINUX_DEFRAGGER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}")
endif()
