# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(defragmenter-ext-metadata-worker
    native/ext_module_loader.c)
target_include_directories(defragmenter-ext-metadata-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-ext-metadata-worker PRIVATE
    ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-ext-metadata-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE IFS_EXT_METADATA_MODE=1)
target_link_libraries(defragmenter-ext-metadata-worker PRIVATE
    defragmenter-fs-host-services defragmenter-filesystem-module-host)

set_target_properties(defragmenter-ext-metadata-worker PROPERTIES
    OUTPUT_NAME ext-metadata-worker)

set(EXT_METADATA_WORKER_FOR_TESTS "$<TARGET_FILE:defragmenter-ext-metadata-worker>")
if(BUILD_TESTING AND TARGET defragmenter-filesystem-module-test-host)
    add_executable(defragmenter-ext-metadata-test-worker native/ext_module_loader.c)
    target_include_directories(defragmenter-ext-metadata-test-worker PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/native" "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-ext-metadata-test-worker PRIVATE
        ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-ext-metadata-test-worker PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE IFS_EXT_METADATA_MODE=1)
    target_link_libraries(defragmenter-ext-metadata-test-worker PRIVATE
        defragmenter-fs-host-services defragmenter-filesystem-module-test-host)
    set(EXT_METADATA_WORKER_FOR_TESTS "$<TARGET_FILE:defragmenter-ext-metadata-test-worker>")
endif()
set(EXT_WORKER_FOR_TESTS "$<TARGET_FILE:defragmenter-ext-worker>")
if(TARGET defragmenter-ext-test-worker)
    set(EXT_WORKER_FOR_TESTS "$<TARGET_FILE:defragmenter-ext-test-worker>")
endif()

install(TARGETS defragmenter-ext-metadata-worker
        RUNTIME DESTINATION /System/Commands/Defragmenter/ext4)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME defragmenter-ext-metadata-map
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ext_metadata_map.py")
    set_tests_properties(defragmenter-ext-metadata-map PROPERTIES
        ENVIRONMENT
            "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/gui;DEFRAGMENTER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR};DEFRAGMENTER_EXT_WORKER=${EXT_WORKER_FOR_TESTS};DEFRAGMENTER_EXT_METADATA_WORKER=${EXT_METADATA_WORKER_FOR_TESTS};IFS_TEST_MODULE_PARENT=${OFS_DEFRAG_TEST_MODULE_PARENT}")
endif()
