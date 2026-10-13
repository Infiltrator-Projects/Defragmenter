# SPDX-License-Identifier: GPL-3.0-or-later
if(BUILD_TESTING)
    add_executable(linux-defragger-helper-policy-test
        tests/test_helper_policy.cpp)
    target_include_directories(linux-defragger-helper-policy-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-helper-policy-test PRIVATE
        ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-helper-policy-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-helper-policy-test PRIVATE
        linux-defragger-runtime-cpp linux-defragger-core)
    add_test(NAME linux-defragger-helper-policy
        COMMAND linux-defragger-helper-policy-test)

    # Python is an independent test-driver language only. No Python application
    # model, GTK compatibility layer or product control plane is qualified here.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME linux-defragger-system-update-contract
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_service.py")
    set_tests_properties(linux-defragger-system-update-contract PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endif()
