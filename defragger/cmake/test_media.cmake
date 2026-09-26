# SPDX-License-Identifier: GPL-3.0-or-later
# Separate all-C GTK companion for creating and verifying sacrificial test media.

pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

set_source_files_properties(test_media/test_media_amiga.c PROPERTIES
    COMPILE_DEFINITIONS "ldtm_format_amiga_volume=ldtm_format_amiga_volume_affs")
set_source_files_properties(test_media/test_media_amiga_payload.c PROPERTIES
    COMPILE_DEFINITIONS "ldtm_populate_amiga_volume=ldtm_populate_amiga_volume_affs;ldtm_verify_amiga_payload=ldtm_verify_amiga_payload_affs")

add_library(linux-defragger-test-media-core STATIC
    test_media/test_media_core.c
    test_media/test_media_worker.c
    test_media/test_media_reserved.c
    test_media/test_media_amiga.c
    test_media/test_media_amiga_payload.c
    test_media/test_media_amiga_dispatch.c
    test_media/test_media_sfs.c
    test_media/test_media_pfs3.c
    test_media/test_media_apfs.c)
target_include_directories(linux-defragger-test-media-core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/test_media"
    "${LD_GENERATED_DIR}")
target_include_directories(linux-defragger-test-media-core PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/affs/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/sfs/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/pfs3/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/apfs/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ufs/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/zfs/native")
target_compile_options(linux-defragger-test-media-core PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-test-media-core PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-test-media-core PUBLIC
    OpenSSL::Crypto InfiltratrCommon::Common linux-defragger-core)
target_link_libraries(linux-defragger-test-media-core PRIVATE
    linux-defragger-affs-native linux-defragger-sfs-native
    linux-defragger-pfs3-native linux-defragger-apfs-native
    linux-defragger-ufs-native linux-defragger-zfs-native)

add_executable(linux-defragger-test-media
    test_media/test_media_main.c
    test_media/test_media_gui.c
    test_media/test_media_theme.c)
target_include_directories(linux-defragger-test-media PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/test_media"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-test-media PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-test-media PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-test-media PRIVATE
    linux-defragger-test-media-core PkgConfig::GTK3 OpenSSL::Crypto)

add_executable(linux-defragger-test-media-mkfs-ofs
    test_media/test_media_amiga_mkfs.c)
target_include_directories(linux-defragger-test-media-mkfs-ofs PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/test_media")
target_compile_options(linux-defragger-test-media-mkfs-ofs PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-test-media-mkfs-ofs PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE LDTM_AMIGA_DOSTYPE=0 LDTM_AMIGA_LABEL="LD_OFS")
target_link_libraries(linux-defragger-test-media-mkfs-ofs PRIVATE
    linux-defragger-test-media-core)
set_target_properties(linux-defragger-test-media-mkfs-ofs PROPERTIES
    OUTPUT_NAME test-media-mkfs-ofs)

add_executable(linux-defragger-test-media-mkfs-ffs
    test_media/test_media_amiga_mkfs.c)
target_include_directories(linux-defragger-test-media-mkfs-ffs PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/test_media")
target_compile_options(linux-defragger-test-media-mkfs-ffs PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-test-media-mkfs-ffs PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE LDTM_AMIGA_DOSTYPE=1 LDTM_AMIGA_LABEL="LD_FFS")
target_link_libraries(linux-defragger-test-media-mkfs-ffs PRIVATE
    linux-defragger-test-media-core)
set_target_properties(linux-defragger-test-media-mkfs-ffs PROPERTIES
    OUTPUT_NAME test-media-mkfs-ffs)

install(TARGETS linux-defragger-test-media RUNTIME DESTINATION bin)
install(TARGETS
    linux-defragger-test-media-mkfs-ofs
    linux-defragger-test-media-mkfs-ffs
    RUNTIME DESTINATION lib/linux-defragger)
install(FILES packaging/io.github.linuxdefragger.TestMedia.desktop
        DESTINATION share/applications)

if(BUILD_TESTING)
    add_executable(linux-defragger-test-media-test tests/test_test_media.c)
    target_include_directories(linux-defragger-test-media-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/test_media"
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/affs/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/sfs/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/pfs3/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/apfs/native")
    target_compile_options(linux-defragger-test-media-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-test-media-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-test-media-test PRIVATE
        linux-defragger-test-media-core linux-defragger-affs-native
        linux-defragger-sfs-native linux-defragger-pfs3-native
        linux-defragger-apfs-native OpenSSL::Crypto)
    add_test(NAME linux-defragger-test-media-core COMMAND linux-defragger-test-media-test)
    add_test(NAME linux-defragger-test-media-install
        COMMAND "${CMAKE_COMMAND}"
            -DLD_BINARY_DIR=${CMAKE_BINARY_DIR}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_test_media_install.cmake")
    add_test(NAME linux-defragger-test-media-ufs-makefs
        COMMAND /bin/sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ufs_makefs.sh"
            "$<TARGET_FILE:linux-defragger-ufs-worker>")
    set_tests_properties(linux-defragger-test-media-ufs-makefs
        PROPERTIES SKIP_RETURN_CODE 77)
endif()
