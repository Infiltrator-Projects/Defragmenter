# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(linux-defragger-ext-metadata-worker
    gui/filesystems/ext4/native/ext_metadata_worker.c)
target_include_directories(linux-defragger-ext-metadata-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ext4/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-ext-metadata-worker PRIVATE
    ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-ext-metadata-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-ext-metadata-worker PRIVATE
    linux-defragger-ext-native linux-defragger-core)

install(TARGETS linux-defragger-ext-metadata-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/ext4)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME linux-defragger-ext-metadata-map
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ext_metadata_map.py")
    set_tests_properties(linux-defragger-ext-metadata-map PROPERTIES
        ENVIRONMENT
            "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/gui;LINUX_DEFRAGGER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR};LINUX_DEFRAGGER_EXT_WORKER=${CMAKE_CURRENT_BINARY_DIR}/linux-defragger-ext-worker")
endif()
