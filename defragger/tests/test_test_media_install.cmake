# SPDX-License-Identifier: GPL-3.0-or-later
if(NOT DEFINED LD_BINARY_DIR)
    message(FATAL_ERROR "LD_BINARY_DIR is required")
endif()

set(_root "${LD_BINARY_DIR}/test-media-install-root")
file(REMOVE_RECURSE "${_root}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "DESTDIR=${_root}"
            "${CMAKE_COMMAND}" --install "${LD_BINARY_DIR}" --prefix /usr
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_stdout
    ERROR_VARIABLE _install_stderr)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR
        "staged Test Media install failed (${_install_result})\n${_install_stdout}\n${_install_stderr}")
endif()

foreach(_path IN ITEMS
    "Programs/defragmenter-test-media"
    "Programs/defragmenter-desktop"
    "Programs/defragmenter"
    "System/Filesystems/Defragmenter/ext4/ext-metadata-worker"
    "System/Resources/Icons/defragmenter.png"
    "System/Resources/Documentation/Defragmenter/README.md"
    "System/Resources/Themes/Defragmenter/art/hero-landscape.jpg"
    "System/Filesystems/Defragmenter/test-media-mkfs-ofs"
    "System/Filesystems/Defragmenter/test-media-mkfs-ffs"
    "System/Filesystems/Defragmenter/test-media-mkfs-ufs"
    "usr/share/applications/io.github.defragmenter.TestMedia.desktop")
    if(NOT EXISTS "${_root}/${_path}")
        message(FATAL_ERROR "staged Test Media install is missing ${_path}")
    endif()
endforeach()

file(REMOVE_RECURSE "${_root}")
message(STATUS "Test Media staged install contains GUI plus OFS/FFS/UFS C creators")
