# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(defragmenter-ext-metadata-worker
    gui/filesystems/ext4/native/ext_metadata_worker.c)
target_include_directories(defragmenter-ext-metadata-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ext4/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-ext-metadata-worker PRIVATE
    ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-ext-metadata-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-ext-metadata-worker PRIVATE
    defragmenter-ext-native defragmenter-core)

set_target_properties(defragmenter-ext-metadata-worker PROPERTIES
    OUTPUT_NAME ext-metadata-worker)

install(TARGETS defragmenter-ext-metadata-worker
        RUNTIME DESTINATION /System/Filesystems/Defragmenter/ext4)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME defragmenter-ext-metadata-map
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ext_metadata_map.py")
    set_tests_properties(defragmenter-ext-metadata-map PROPERTIES
        ENVIRONMENT
            "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/gui;DEFRAGMENTER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR};DEFRAGMENTER_EXT_WORKER=${CMAKE_CURRENT_BINARY_DIR}/defragmenter-ext-worker")
endif()
