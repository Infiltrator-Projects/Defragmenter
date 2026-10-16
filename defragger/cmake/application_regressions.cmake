# SPDX-License-Identifier: GPL-3.0-or-later
if(BUILD_TESTING)
    add_executable(defragmenter-helper-policy-test
        tests/test_helper_policy.cpp)
    target_include_directories(defragmenter-helper-policy-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-helper-policy-test PRIVATE
        ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-helper-policy-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-helper-policy-test PRIVATE
        defragmenter-runtime-cpp defragmenter-core)
    add_test(NAME defragmenter-helper-policy
        COMMAND defragmenter-helper-policy-test)

    add_executable(defragmenter-device-topology-test
        tests/test_device_topology.c)
    target_include_directories(defragmenter-device-topology-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-device-topology-test PRIVATE
        ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-device-topology-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-device-topology-test PRIVATE
        defragmenter-core)
    add_test(NAME defragmenter-device-topology
        COMMAND defragmenter-device-topology-test)
    set_tests_properties(defragmenter-device-topology PROPERTIES
        SKIP_RETURN_CODE 77)

    # Python is an independent test-driver language only. No Python application
    # model, GTK compatibility layer or product control plane is qualified here.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME defragmenter-system-update-contract
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_service.py")
    set_tests_properties(defragmenter-system-update-contract PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endif()
