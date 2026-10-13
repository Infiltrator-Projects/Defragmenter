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

    find_program(LD_PYTHON3_EXECUTABLE NAMES python3 REQUIRED)

    add_test(
        NAME linux-defragger-gui-models
        COMMAND "${LD_PYTHON3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gui_models.py")
    add_test(
        NAME linux-defragger-gui-services
        COMMAND "${LD_PYTHON3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gui_services.py")
    add_test(
        NAME linux-defragger-system-update-contract
        COMMAND "${LD_PYTHON3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_service.py")
    add_test(
        NAME linux-defragger-gui-volume-identity
        COMMAND "${LD_PYTHON3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gui_volume_identity.py")
    add_test(
        NAME linux-defragger-gui-ext-display-identity
        COMMAND "${LD_PYTHON3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ext_display_identity.py")
    add_test(
        NAME linux-defragger-about-gold-standard
        COMMAND "${LD_PYTHON3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_about_gold_standard.py")

    set_tests_properties(
        linux-defragger-gui-models
        linux-defragger-gui-services
        linux-defragger-system-update-contract
        linux-defragger-gui-volume-identity
        linux-defragger-gui-ext-display-identity
        linux-defragger-about-gold-standard
        PROPERTIES
        ENVIRONMENT
            "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/gui")
endif()
