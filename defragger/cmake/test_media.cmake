# SPDX-License-Identifier: GPL-3.0-or-later
# Separate all-C GTK companion for creating and verifying sacrificial test media.

pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

add_executable(defragmenter-desktop
    native/desktop.cpp
    native/desktop_map_render.cpp
    native/desktop_process_session.cpp)
target_include_directories(defragmenter-desktop PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native")
target_compile_options(defragmenter-desktop PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-desktop PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-desktop PRIVATE
    defragmenter-runtime-cpp PkgConfig::GTK3)
set_target_properties(defragmenter-desktop PROPERTIES OUTPUT_NAME defragmenter)
install(TARGETS defragmenter-desktop RUNTIME DESTINATION /Programs)

add_library(defragmenter-test-media-core STATIC
    test_media/test_media_core.c
    test_media/test_media_raw.c
    test_media/test_media_fat_module.c
    test_media/test_media_ext_module.c
    test_media/test_media_minix_module.c
    test_media/test_media_swap_module.c
    test_media/test_media_hfs_module.c
    test_media/test_media_hfsplus_module.c
    test_media/test_media_ntfs_module.c
    test_media/test_media_xfs_module.c
    test_media/test_media_btrfs_module.c
    test_media/test_media_raw_zfs.c
    test_media/test_media_worker.c
    test_media/test_media_reserved.c
    test_media/test_media_ffs_module.c
    test_media/test_media_amiga_dispatch.c
    test_media/test_media_ofs_module.c
    test_media/test_media_sfs.c
    test_media/test_media_pfs3.c
    test_media/test_media_apfs.c)
target_include_directories(defragmenter-test-media-core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/test_media"
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${LD_GENERATED_DIR}")
target_include_directories(defragmenter-test-media-core PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ufs/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/zfs/native")
target_compile_options(defragmenter-test-media-core PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-test-media-core PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-test-media-core PUBLIC
    OpenSSL::Crypto InfiltratrCommon::Common defragmenter-core defragmenter-filesystem-module-host defragmenter-fs-host-services)

add_executable(defragmenter-test-media
    test_media/test_media_main.c
    test_media/test_media_gui.c
    test_media/test_media_theme.c)
target_include_directories(defragmenter-test-media PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/test_media"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-test-media PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-test-media PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-test-media PRIVATE
    defragmenter-test-media-core PkgConfig::GTK3 OpenSSL::Crypto)

add_executable(defragmenter-test-media-mkfs-ofs
    test_media/test_media_amiga_mkfs.c)
target_include_directories(defragmenter-test-media-mkfs-ofs PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/test_media")
target_compile_options(defragmenter-test-media-mkfs-ofs PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-test-media-mkfs-ofs PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE LDTM_AMIGA_DOSTYPE=0 LDTM_AMIGA_LABEL="LD_OFS")
target_link_libraries(defragmenter-test-media-mkfs-ofs PRIVATE
    defragmenter-test-media-core)
set_target_properties(defragmenter-test-media-mkfs-ofs PROPERTIES
    OUTPUT_NAME test-media-mkfs-ofs)

add_executable(defragmenter-test-media-mkfs-ffs
    test_media/test_media_amiga_mkfs.c)
target_include_directories(defragmenter-test-media-mkfs-ffs PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/test_media")
target_compile_options(defragmenter-test-media-mkfs-ffs PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-test-media-mkfs-ffs PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE LDTM_AMIGA_DOSTYPE=1 LDTM_AMIGA_LABEL="LD_FFS")
target_link_libraries(defragmenter-test-media-mkfs-ffs PRIVATE
    defragmenter-test-media-core)
set_target_properties(defragmenter-test-media-mkfs-ffs PROPERTIES
    OUTPUT_NAME test-media-mkfs-ffs)

add_executable(defragmenter-test-media-mkfs-ufs
    native/ufs_test_media_module_loader.c)
target_include_directories(defragmenter-test-media-mkfs-ufs PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-test-media-mkfs-ufs PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-test-media-mkfs-ufs PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-test-media-mkfs-ufs PRIVATE
    defragmenter-filesystem-module-host)
set_target_properties(defragmenter-test-media-mkfs-ufs PROPERTIES
    OUTPUT_NAME test-media-mkfs-ufs)

# The test-only module host permits a disposable SWAP/OFS/UFS .so root.
# It must never be installed or used in the production wrapper.
set(UFS_MKFS_WORKER_FOR_TESTS "$<TARGET_FILE:defragmenter-test-media-mkfs-ufs>")
set(UFS_WORKER_FOR_TESTS "$<TARGET_FILE:defragmenter-ufs-worker>")
if(BUILD_TESTING AND OFS_DEFRAG_TEST_MODULE_PARENT)
    add_executable(defragmenter-test-media-mkfs-ufs-test
        native/ufs_test_media_module_loader.c)
    target_include_directories(defragmenter-test-media-mkfs-ufs-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-test-media-mkfs-ufs-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-test-media-mkfs-ufs-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-test-media-mkfs-ufs-test PRIVATE
        defragmenter-filesystem-module-test-host)
    set(UFS_MKFS_WORKER_FOR_TESTS "$<TARGET_FILE:defragmenter-test-media-mkfs-ufs-test>")
    set(UFS_WORKER_FOR_TESTS "$<TARGET_FILE:defragmenter-ufs-test-worker>")
endif()


install(TARGETS defragmenter-test-media RUNTIME DESTINATION /Programs)
install(TARGETS
    defragmenter-test-media-mkfs-ofs
    defragmenter-test-media-mkfs-ffs
    defragmenter-test-media-mkfs-ufs
    RUNTIME DESTINATION /System/Commands/Defragmenter)
install(FILES packaging/io.github.defragmenter.TestMedia.desktop
        DESTINATION share/applications)

if(BUILD_TESTING)
    add_executable(defragmenter-apfs-so-fixture-test tests/test_apfs_so_fixture.c)
    target_include_directories(defragmenter-apfs-so-fixture-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/test_media"
        "${CMAKE_CURRENT_SOURCE_DIR}/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
    target_compile_options(defragmenter-apfs-so-fixture-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-apfs-so-fixture-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-apfs-so-fixture-test PRIVATE
        defragmenter-test-media-core)
    if(TARGET defragmenter-filesystem-module-test-host)
        target_link_libraries(defragmenter-apfs-so-fixture-test PRIVATE
            defragmenter-filesystem-module-test-host)
    endif()
    add_executable(defragmenter-sfs-so-fixture-test tests/test_sfs_so_fixture.c)
    target_include_directories(defragmenter-sfs-so-fixture-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/test_media"
        "${CMAKE_CURRENT_SOURCE_DIR}/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
    target_compile_options(defragmenter-sfs-so-fixture-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-sfs-so-fixture-test PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-sfs-so-fixture-test PRIVATE defragmenter-test-media-core)
    if(TARGET defragmenter-filesystem-module-test-host)
        target_link_libraries(defragmenter-sfs-so-fixture-test PRIVATE
            defragmenter-filesystem-module-test-host)
    endif()
    add_executable(defragmenter-pfs3-so-fixture-test tests/test_pfs3_so_fixture.c)
    target_include_directories(defragmenter-pfs3-so-fixture-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/test_media"
        "${CMAKE_CURRENT_SOURCE_DIR}/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
    target_compile_options(defragmenter-pfs3-so-fixture-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-pfs3-so-fixture-test PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-pfs3-so-fixture-test PRIVATE defragmenter-test-media-core)
    if(TARGET defragmenter-filesystem-module-test-host)
        target_link_libraries(defragmenter-pfs3-so-fixture-test PRIVATE
            defragmenter-filesystem-module-test-host)
    endif()
    # The standalone hosted integration runs this on its own disposable image.
    add_executable(defragmenter-test-media-raw-test tests/test_test_media_raw.c)
    target_include_directories(defragmenter-test-media-raw-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
    target_compile_options(defragmenter-test-media-raw-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-test-media-raw-test PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-test-media-raw-test PRIVATE
        defragmenter-test-media-core)
    add_test(NAME defragmenter-test-media-corpus-contract COMMAND defragmenter-test-media-raw-test)
    # Every fixture creates private media and a private journal. Register them
    # independently so CTest can bound and schedule each complete destructive
    # qualification instead of timing out one serial multi-filesystem command.
    # Start the longer EXT family early; no format, payload or assertion is skipped.
    set(LDTM_NATIVE_FIXTURE_FORMATS
        ext2 ext3 ext4 xfs btrfs ntfs
        fat12 fat16 fat32 exfat hfs hfsplus minix ufs zfs swap)
    foreach(ldtm_fs IN LISTS LDTM_NATIVE_FIXTURE_FORMATS)
        add_test(NAME "defragmenter-test-media-native-${ldtm_fs}"
            COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_test_media_raw.py"
                "${CMAKE_BINARY_DIR}" "${ldtm_fs}")
        set_tests_properties("defragmenter-test-media-native-${ldtm_fs}"
            PROPERTIES TIMEOUT 1800
                ENVIRONMENT "DEFRAGMENTER_FAT_WORKER=${FAT_WORKER_FOR_TESTS};DEFRAGMENTER_EXT_WORKER=${EXT_WORKER_FOR_TESTS};DEFRAGMENTER_EXT_METADATA_WORKER=${EXT_METADATA_WORKER_FOR_TESTS}")
    endforeach()
    add_executable(defragmenter-test-media-gui-test
        tests/test_test_media_gui.c test_media/test_media_theme.c)
    target_include_directories(defragmenter-test-media-gui-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/test_media" "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-test-media-gui-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-test-media-gui-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-test-media-gui-test PRIVATE
        defragmenter-test-media-core PkgConfig::GTK3 OpenSSL::Crypto)
    find_program(LDTM_XVFB_RUN xvfb-run)
    if(LDTM_XVFB_RUN)
        add_test(NAME defragmenter-test-media-gui
            COMMAND "${LDTM_XVFB_RUN}" -a -s "-screen 0 1024x768x24"
                "$<TARGET_FILE:defragmenter-test-media-gui-test>")
        set_tests_properties(defragmenter-test-media-gui PROPERTIES TIMEOUT 20)
    endif()
    add_executable(defragmenter-desktop-analysis-test tests/test_desktop_analysis.cpp)
    target_compile_options(defragmenter-desktop-analysis-test PRIVATE ${LD_WARNING_FLAGS})
    target_link_libraries(defragmenter-desktop-analysis-test PRIVATE
        defragmenter-runtime-cpp)
    add_test(NAME defragmenter-desktop-analysis
        COMMAND "$<TARGET_FILE:defragmenter-desktop-analysis-test>")
    set_tests_properties(defragmenter-desktop-analysis PROPERTIES TIMEOUT 20)
    # This independent test directly calls SFS native routines; its engine
    # source comes ONLY from the pinned filesystem repository during CI.
    if(SFS_NATIVE_TEST_SOURCE_DIR AND PFS3_NATIVE_TEST_SOURCE_DIR AND APFS_NATIVE_TEST_SOURCE_DIR AND FFS_NATIVE_TEST_SOURCE_DIR)
    add_executable(defragmenter-test-media-test tests/test_test_media.c)
    target_include_directories(defragmenter-test-media-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/test_media"
        "${FFS_NATIVE_TEST_SOURCE_DIR}"
        "${SFS_NATIVE_TEST_SOURCE_DIR}/native"
        "${PFS3_NATIVE_TEST_SOURCE_DIR}/native"
        "${APFS_NATIVE_TEST_SOURCE_DIR}/native")
    target_compile_options(defragmenter-test-media-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-test-media-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-test-media-test PRIVATE
        defragmenter-test-media-core defragmenter-affs-test-native
        defragmenter-sfs-fixture-native defragmenter-pfs3-fixture-native
        defragmenter-apfs-fixture-native OpenSSL::Crypto)
    add_test(NAME defragmenter-test-media-core COMMAND defragmenter-test-media-test)
    endif()
    add_test(NAME defragmenter-test-media-install
        COMMAND "${CMAKE_COMMAND}"
            -DLD_BINARY_DIR=${CMAKE_BINARY_DIR}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_test_media_install.cmake")
    add_test(NAME defragmenter-test-media-ufs-makefs
        COMMAND /bin/sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ufs_makefs.sh"
            "${UFS_MKFS_WORKER_FOR_TESTS}"
            "${UFS_WORKER_FOR_TESTS}")
endif()

# Test processes resolve modules only from the explicitly provisioned disposable
# root. Installed tools remain linked to the production host without overrides.
if(TARGET defragmenter-filesystem-module-test-host)
    foreach(test_program defragmenter-test-media-test defragmenter-test-media-raw-test)
        if(TARGET ${test_program})
            target_link_libraries(${test_program} PRIVATE defragmenter-filesystem-module-test-host)
        endif()
    endforeach()
endif()
