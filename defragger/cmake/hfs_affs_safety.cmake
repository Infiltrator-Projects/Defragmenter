# SPDX-License-Identifier: GPL-3.0-or-later

# HFS+ already links OpenSSL through its native library.  AFFS persists a
# cryptographic digest for its verified recovery stage as well, so its worker
# needs the same host-supplied crypto provider.
target_link_libraries(defragmenter-affs-worker PRIVATE OpenSSL::Crypto)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    add_test(
        NAME defragmenter-affs-native-integration
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_affs_native.py")
    set_tests_properties(defragmenter-affs-native-integration PROPERTIES
        ENVIRONMENT
            "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/gui:${CMAKE_CURRENT_SOURCE_DIR}/tests;DEFRAGMENTER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR};DEFRAGMENTER_AFFS_WORKER=${AFFS_WORKER_FOR_TESTS};IFS_TEST_MODULE_PARENT=${OFS_DEFRAG_TEST_MODULE_PARENT}")

    add_test(
        NAME defragmenter-affs-batched-io
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_affs_batched_io.py")
    set_tests_properties(defragmenter-affs-batched-io PROPERTIES
        ENVIRONMENT
            "PYTHONDONTWRITEBYTECODE=1")

    add_test(
        NAME defragmenter-hfsplus-native-integration
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_hfsplus_native.py")
    set_tests_properties(defragmenter-hfsplus-native-integration PROPERTIES
        ENVIRONMENT
            "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/gui:${CMAKE_CURRENT_SOURCE_DIR}/tests;DEFRAGMENTER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}")
endif()
