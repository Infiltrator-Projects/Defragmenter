# SPDX-License-Identifier: GPL-3.0-or-later

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" LINUX_DEFRAGGER_PACKAGE_VERSION)
string(STRIP "${LINUX_DEFRAGGER_PACKAGE_VERSION}" LINUX_DEFRAGGER_PACKAGE_VERSION)
if(NOT LINUX_DEFRAGGER_PACKAGE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+-[0-9]+$")
    message(FATAL_ERROR "VERSION must contain a release such as 1.8.0-49")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)

set(INFILTRATR_COMMON_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/infiltratr-common")
set(INFILTRATR_COMMON_URL
    "https://github.com/Infiltrator-Projects/Infiltrator-Libraries.git")
set(INFILTRATR_COMMON_TAG "v1.19.10")
set(INFILTRATR_COMMON_EXPECTED_VERSION "1.19.10")
set(INFILTRATR_COMMON_EXPECTED_COMMIT
    "33e69c0a462b56d388881d89c4eb49f72fa0b0fe")

if(NOT EXISTS "${INFILTRATR_COMMON_DIR}/VERSION")
    find_program(LD_GIT_EXECUTABLE git REQUIRED)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
        execute_process(
            COMMAND "${LD_GIT_EXECUTABLE}" submodule update --init --depth 1 --
                    shared/infiltratr-common
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            RESULT_VARIABLE LD_COMMON_FETCH_RESULT)
    else()
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/shared")
        execute_process(
            COMMAND "${LD_GIT_EXECUTABLE}" clone --depth 1 --branch
                    "${INFILTRATR_COMMON_TAG}" "${INFILTRATR_COMMON_URL}"
                    "${INFILTRATR_COMMON_DIR}"
            RESULT_VARIABLE LD_COMMON_FETCH_RESULT)
    endif()
    if(NOT LD_COMMON_FETCH_RESULT EQUAL 0 OR
       NOT EXISTS "${INFILTRATR_COMMON_DIR}/VERSION")
        message(FATAL_ERROR
            "Unable to retrieve Infiltratr Common ${INFILTRATR_COMMON_EXPECTED_VERSION}.")
    endif()
endif()

file(READ "${INFILTRATR_COMMON_DIR}/VERSION" INFILTRATR_COMMON_VERSION)
string(STRIP "${INFILTRATR_COMMON_VERSION}" INFILTRATR_COMMON_VERSION)
if(NOT INFILTRATR_COMMON_VERSION STREQUAL INFILTRATR_COMMON_EXPECTED_VERSION)
    message(FATAL_ERROR
        "Infiltratr Common ${INFILTRATR_COMMON_EXPECTED_VERSION} is required; found ${INFILTRATR_COMMON_VERSION}.")
endif()
if(EXISTS "${INFILTRATR_COMMON_DIR}/.git")
    find_program(LD_GIT_EXECUTABLE git REQUIRED)
    execute_process(
        COMMAND "${LD_GIT_EXECUTABLE}" -C "${INFILTRATR_COMMON_DIR}" rev-parse HEAD
        OUTPUT_VARIABLE INFILTRATR_COMMON_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE LD_COMMON_COMMIT_RESULT)
    if(NOT LD_COMMON_COMMIT_RESULT EQUAL 0 OR
       NOT INFILTRATR_COMMON_COMMIT STREQUAL INFILTRATR_COMMON_EXPECTED_COMMIT)
        message(FATAL_ERROR
            "Infiltratr Common must be pinned to ${INFILTRATR_COMMON_EXPECTED_COMMIT}; found ${INFILTRATR_COMMON_COMMIT}.")
    endif()
endif()

option(LD_ENABLE_WERROR "Treat first-party compiler warnings as errors" OFF)
option(LD_ENABLE_SANITIZERS "Enable address and undefined-behaviour sanitizers" OFF)
option(LD_GENERIC_AMD64 "Build for the baseline x86-64 instruction set" OFF)
option(LD_NATIVE_OPTIMIZATION "Optimise native code for this build machine" OFF)

if(LD_GENERIC_AMD64)
    set(LINUX_DEFRAGGER_BUILD_PROFILE "generic")
    set(LINUX_DEFRAGGER_BUILD_LABEL "Generic / APT package")
elseif(LD_NATIVE_OPTIMIZATION)
    set(LINUX_DEFRAGGER_BUILD_PROFILE "native")
    set(LINUX_DEFRAGGER_BUILD_LABEL "Native / local machine compile")
else()
    set(LINUX_DEFRAGGER_BUILD_PROFILE "source")
    set(LINUX_DEFRAGGER_BUILD_LABEL "Source / development build")
endif()

if(LD_GENERIC_AMD64 AND LD_NATIVE_OPTIMIZATION)
    message(FATAL_ERROR
        "LD_GENERIC_AMD64 and LD_NATIVE_OPTIMIZATION are mutually exclusive")
endif()

# Release artifacts deliberately have two CPU policies.  The distributed
# amd64 package targets the original x86-64 baseline, while the local .run
# installer selects the compiler's complete native target for that machine.
include(CheckCCompilerFlag)
set(LD_CPU_FLAGS)
if(LD_GENERIC_AMD64)
    check_c_compiler_flag("-march=x86-64" LD_HAS_MARCH_X86_64)
    check_c_compiler_flag("-mtune=generic" LD_HAS_MTUNE_GENERIC)
    if(NOT LD_HAS_MARCH_X86_64 OR NOT LD_HAS_MTUNE_GENERIC)
        message(FATAL_ERROR
            "The selected compiler cannot produce the generic amd64 build")
    endif()
    list(APPEND LD_CPU_FLAGS -march=x86-64 -mtune=generic)
elseif(LD_NATIVE_OPTIMIZATION)
    check_c_compiler_flag("-march=native" LD_HAS_MARCH_NATIVE)
    check_c_compiler_flag("-mtune=native" LD_HAS_MTUNE_NATIVE)
    if(NOT LD_HAS_MARCH_NATIVE OR NOT LD_HAS_MTUNE_NATIVE)
        message(FATAL_ERROR
            "The selected compiler does not support native CPU optimisation")
    endif()
    list(APPEND LD_CPU_FLAGS -march=native -mtune=native)
endif()

if(LD_CPU_FLAGS)
    add_compile_options(${LD_CPU_FLAGS})
endif()

set(LD_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${LD_GENERATED_DIR}")
configure_file(src/core/version.h.in "${LD_GENERATED_DIR}/version.h" @ONLY)
configure_file(packaging/generated/version.py.in "${LD_GENERATED_DIR}/version.py" @ONLY)

set(LD_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2)
if(LD_ENABLE_WERROR)
    list(APPEND LD_WARNING_FLAGS -Werror)
endif()
if(LD_ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Infiltratr Common owns generic parsing, arithmetic, byte-order, exact-I/O,
# checked allocation growth, strings, paths, sysfs primitives and generic
# durable file publication/removal. Consume its authoritative package target
# so every source dependency and transitive platform library remains defined by
# Common itself. Application-specific device safety, staging, Stop and
# filesystem transaction mechanics remain local to Defragmenter.
set(INFILTRATR_COMMON_BUILD_TESTS OFF)
set(INFILTRATR_COMMON_BUILD_SHARED OFF)
set(INFILTRATR_COMMON_WARNINGS_AS_ERRORS "${LD_ENABLE_WERROR}")
add_subdirectory(
    "${INFILTRATR_COMMON_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/infiltratr-common"
    EXCLUDE_FROM_ALL)

add_library(linux-defragger-core STATIC
    src/core/ld_runtime.c
    src/core/ld_io.c
    src/core/ld_device.c
    src/core/ld_stop.c
    src/core/ld_path.c
    src/core/ld_protocol.c)
target_include_directories(linux-defragger-core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-core PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-core PUBLIC _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-core PUBLIC InfiltratrCommon::Common)

# C++17 owns application-level contracts and orchestration where value types,
# scoped state and RAII improve the implementation. Filesystem parsing and
# mutation remain in the existing native C engines.
add_library(linux-defragger-runtime-cpp STATIC
    native/helper_policy.cpp
    native/json.cpp
    native/map.cpp
    native/process.cpp
    native/runtime.cpp)
target_include_directories(linux-defragger-runtime-cpp PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-runtime-cpp PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-runtime-cpp PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-runtime-cpp PUBLIC
    linux-defragger-core InfiltratrCommon::Common Threads::Threads)

add_executable(linux-defragger-operation-engine-cpp
    native/operation_engine.cpp)
set_target_properties(linux-defragger-operation-engine-cpp PROPERTIES
    OUTPUT_NAME linux-defragger-operation-engine)
target_include_directories(linux-defragger-operation-engine-cpp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-operation-engine-cpp PRIVATE
    ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-operation-engine-cpp PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-operation-engine-cpp PRIVATE
    linux-defragger-runtime-cpp linux-defragger-core)

add_executable(linux-defragger-mapper-cpp
    native/mapper.cpp)
set_target_properties(linux-defragger-mapper-cpp PROPERTIES
    OUTPUT_NAME linux-defragger-mapper)
target_include_directories(linux-defragger-mapper-cpp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-mapper-cpp PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-mapper-cpp PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-mapper-cpp PRIVATE
    linux-defragger-runtime-cpp linux-defragger-core)

add_executable(linux-defragger-privileged-helper-cpp
    native/privileged_helper.cpp)
set_target_properties(linux-defragger-privileged-helper-cpp PROPERTIES
    OUTPUT_NAME linux-defragger-privileged-helper)
target_include_directories(linux-defragger-privileged-helper-cpp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-privileged-helper-cpp PRIVATE
    ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-privileged-helper-cpp PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-privileged-helper-cpp PRIVATE
    linux-defragger-runtime-cpp linux-defragger-core Threads::Threads)

# FAT remains native C, but it is a private implementation detail of the
# authoritative gui/filesystems/fat plugin.  There is deliberately no second
# native filesystem registry or plugin ABI.
add_executable(linux-defragger-fat-worker
    gui/filesystems/fat/native/writer.c
    gui/filesystems/fat/native/fat_analysis.c
    gui/filesystems/fat/native/fat_directory.c
    gui/filesystems/fat/native/fat_relayout.c
    gui/filesystems/fat/native/fat_io.c
    gui/filesystems/fat/native/fat_journal.c
    gui/filesystems/fat/native/fat_relocation.c
    gui/filesystems/fat/native/fat_volume.c)
target_include_directories(linux-defragger-fat-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/fat/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-fat-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-fat-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-fat-worker PRIVATE
    linux-defragger-core Threads::Threads)

# APFS remains analysis-only, but its on-disk NX superblock interpretation is
# native C. Python is retained only as the GUI/backend adapter.
add_library(linux-defragger-apfs-native STATIC
    gui/filesystems/apfs/native/apfs_native.c)
target_include_directories(linux-defragger-apfs-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/apfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-apfs-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-apfs-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-apfs-native PUBLIC linux-defragger-core)

add_executable(linux-defragger-apfs-worker
    gui/filesystems/apfs/native/apfs_worker.c)
target_include_directories(linux-defragger-apfs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/apfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-apfs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-apfs-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-apfs-worker PRIVATE
    linux-defragger-apfs-native linux-defragger-core)

find_package(OpenSSL REQUIRED)

# Minix exact analysis, offline staging, mutation and recovery are native C.
# Python is retained only as the GUI/backend adapter.
add_library(linux-defragger-minix-native STATIC
    gui/filesystems/minix/native/minix_native.c)
target_include_directories(linux-defragger-minix-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/minix/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-minix-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-minix-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-minix-native PUBLIC linux-defragger-core)

add_executable(linux-defragger-minix-worker
    gui/filesystems/minix/native/minix_worker.c)
target_include_directories(linux-defragger-minix-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/minix/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-minix-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-minix-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-minix-worker PRIVATE
    linux-defragger-minix-native linux-defragger-core OpenSSL::Crypto)

# Linux swap identification, metadata and allocation mapping are native C.
# Python remains only as a temporary GUI/backend adapter.
add_library(linux-defragger-swap-native STATIC
    gui/filesystems/swap/native/swap_native.c)
target_include_directories(linux-defragger-swap-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/swap/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-swap-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-swap-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-swap-native PUBLIC linux-defragger-core)

add_executable(linux-defragger-swap-worker
    gui/filesystems/swap/native/swap_worker.c)
target_include_directories(linux-defragger-swap-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/swap/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-swap-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-swap-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-swap-worker PRIVATE
    linux-defragger-swap-native linux-defragger-core)

find_package(SQLite3 REQUIRED)

add_library(linux-defragger-xfs-native STATIC
    gui/filesystems/xfs/native/xfs_common.c
    gui/filesystems/xfs/native/xfs_catalog.c
    gui/filesystems/xfs/native/xfs_plan.c
    gui/filesystems/xfs/native/xfs_metadata.c)
target_include_directories(linux-defragger-xfs-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/xfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-xfs-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-xfs-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-xfs-native PUBLIC
    linux-defragger-core SQLite::SQLite3 OpenSSL::Crypto)

# EXT2/3/4 is implemented directly in native C through the linked libext2fs
# library.  Filesystem mutation is performed in-process by the plugin worker.
find_package(PkgConfig REQUIRED)
pkg_check_modules(EXT2FS REQUIRED IMPORTED_TARGET ext2fs)
find_library(COM_ERR_LIBRARY com_err REQUIRED)

add_library(linux-defragger-ext-native STATIC
    gui/filesystems/ext4/native/ext_common.c
    gui/filesystems/ext4/native/ext_catalog.c
    gui/filesystems/ext4/native/ext_plan.c
    gui/filesystems/ext4/native/ext_workspace.c)
target_include_directories(linux-defragger-ext-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ext4/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-ext-native PRIVATE
    ${LD_WARNING_FLAGS} -Wno-deprecated-declarations)
target_compile_definitions(linux-defragger-ext-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-ext-native PUBLIC
    linux-defragger-core SQLite::SQLite3 OpenSSL::Crypto PkgConfig::EXT2FS ${COM_ERR_LIBRARY})

add_executable(linux-defragger-ext-worker
    gui/filesystems/ext4/native/ext_worker.c)
target_include_directories(linux-defragger-ext-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ext4/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-ext-worker PRIVATE
    ${LD_WARNING_FLAGS} -Wno-deprecated-declarations)
target_compile_definitions(linux-defragger-ext-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-ext-worker PRIVATE
    linux-defragger-ext-native linux-defragger-core SQLite::SQLite3 OpenSSL::Crypto PkgConfig::EXT2FS ${COM_ERR_LIBRARY})

add_library(linux-defragger-ntfs-native STATIC
    gui/filesystems/ntfs/native/ntfs_common.c
    gui/filesystems/ntfs/native/ntfs_catalog.c
    gui/filesystems/ntfs/native/ntfs_plan.c
    gui/filesystems/ntfs/native/ntfs_plan_db.cpp)
target_include_directories(linux-defragger-ntfs-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ntfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-ntfs-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-ntfs-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-ntfs-native PUBLIC
    linux-defragger-core SQLite::SQLite3 OpenSSL::Crypto)

add_executable(linux-defragger-ntfs-worker
    gui/filesystems/ntfs/native/ntfs_worker.c)
target_include_directories(linux-defragger-ntfs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ntfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-ntfs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-ntfs-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-ntfs-worker PRIVATE
    linux-defragger-ntfs-native linux-defragger-core SQLite::SQLite3 OpenSSL::Crypto)
set_property(TARGET linux-defragger-ntfs-worker PROPERTY LINKER_LANGUAGE CXX)

add_library(linux-defragger-exfat-native STATIC
    gui/filesystems/exfat/native/exfat_common.c
    gui/filesystems/exfat/native/exfat_plan.c
    gui/filesystems/exfat/native/exfat_relayout.c)
target_include_directories(linux-defragger-exfat-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/exfat/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-exfat-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-exfat-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-exfat-native PUBLIC
    linux-defragger-core OpenSSL::Crypto)

add_executable(linux-defragger-exfat-worker
    gui/filesystems/exfat/native/exfat_worker.c)
target_include_directories(linux-defragger-exfat-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/exfat/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-exfat-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-exfat-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-exfat-worker PRIVATE
    linux-defragger-exfat-native linux-defragger-core OpenSSL::Crypto)

add_library(linux-defragger-affs-native STATIC
    gui/filesystems/affs/native/affs_native.c)
target_include_directories(linux-defragger-affs-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/affs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-affs-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-affs-native PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-affs-native PUBLIC linux-defragger-core)

add_executable(linux-defragger-affs-worker
    gui/filesystems/affs/native/affs_worker.c)
target_include_directories(linux-defragger-affs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/affs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-affs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-affs-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-affs-worker PRIVATE linux-defragger-affs-native linux-defragger-core)
add_library(linux-defragger-hfsplus-native STATIC
    gui/filesystems/hfsplus/native/hfsplus_native.c)
target_include_directories(linux-defragger-hfsplus-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/hfsplus/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-hfsplus-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-hfsplus-native PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-hfsplus-native PUBLIC linux-defragger-core OpenSSL::Crypto)

add_executable(linux-defragger-hfsplus-worker
    gui/filesystems/hfsplus/native/hfsplus_worker.c)
target_include_directories(linux-defragger-hfsplus-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/hfsplus/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-hfsplus-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-hfsplus-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-hfsplus-worker PRIVATE linux-defragger-hfsplus-native linux-defragger-core OpenSSL::Crypto)

add_executable(linux-defragger-xfs-worker
    gui/filesystems/xfs/native/xfs_worker.c)
target_include_directories(linux-defragger-xfs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/xfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-xfs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-xfs-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-xfs-worker PRIVATE
    linux-defragger-xfs-native linux-defragger-core SQLite::SQLite3 OpenSSL::Crypto)

# Classic HFS analysis and bounded offline mutation share the same first-party
# parser.  The installed binary retains the historical hfs_analyser name for
# compatibility while adding Defragment/Growth Defrag/Recover.
add_executable(linux-defragger-hfs-worker gui/filesystems/hfs/native/writer.c)
target_include_directories(linux-defragger-hfs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/hfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-hfs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-hfs-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-hfs-worker PRIVATE
    linux-defragger-core OpenSSL::Crypto)
set_target_properties(linux-defragger-hfs-worker PROPERTIES OUTPUT_NAME hfs_analyser)

install(TARGETS linux-defragger-affs-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/affs)
install(TARGETS linux-defragger-fat-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/fat)
install(TARGETS linux-defragger-apfs-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/apfs)
install(TARGETS linux-defragger-minix-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/minix)
install(TARGETS linux-defragger-swap-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/swap)
install(TARGETS linux-defragger-xfs-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/xfs)
install(TARGETS linux-defragger-ext-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/ext4)
install(TARGETS linux-defragger-ntfs-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/ntfs)
install(TARGETS linux-defragger-exfat-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/exfat)
install(TARGETS linux-defragger-hfsplus-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/hfsplus)
install(TARGETS linux-defragger-hfs-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/hfs)

# Native C++ application services replace the Python mapper, operation
# dispatcher and privileged helper while preserving their established command
# and JSON protocol contracts during the staged GUI migration.
install(TARGETS
        linux-defragger-operation-engine-cpp
        linux-defragger-mapper-cpp
        linux-defragger-privileged-helper-cpp
        RUNTIME DESTINATION lib/linux-defragger)

install(PROGRAMS
    gui/linux_defragger_gui.py
    gui/allocation_mapper.py
    gui/privileged_helper.py
    gui/operation_engine.py
    DESTINATION lib/linux-defragger)
install(FILES "${LD_GENERATED_DIR}/version.py" DESTINATION lib/linux-defragger)

install(DIRECTORY gui/core gui/engine gui/ui gui/backends gui/filesystems
        DESTINATION lib/linux-defragger
        FILES_MATCHING PATTERN "*.py"
        PATTERN "__pycache__" EXCLUDE)

install(PROGRAMS packaging/linux-defragger DESTINATION bin)
install(FILES packaging/io.github.linuxdefragger.desktop
        DESTINATION share/applications)
install(FILES packaging/io.github.linuxdefragger.png
        DESTINATION share/icons/hicolor/96x96/apps)
install(FILES packaging/io.github.linuxdefragger.png
        DESTINATION share/app-install/icons
        RENAME infiltrator-defragmenter.png)
install(FILES packaging/io.github.linuxdefragger.png
        DESTINATION lib/linux-defragger
        RENAME defragmenter-icon.png)

# Install only documentation that is part of the self-contained application
# source tree. Canonical repository documentation lives at ../docs and is not
# duplicated into the native installer payload.
install(FILES README.md
        DESTINATION share/doc/linux-defragger)
install(FILES LICENSE
        DESTINATION share/doc/linux-defragger RENAME COPYING.GPL-3.0)
include(CTest)
if(BUILD_TESTING)
    add_executable(linux-defragger-cpp-runtime-test
        tests/test_cpp_runtime.cpp)
    target_include_directories(linux-defragger-cpp-runtime-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-cpp-runtime-test PRIVATE
        ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-cpp-runtime-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-cpp-runtime-test PRIVATE
        linux-defragger-runtime-cpp linux-defragger-core)
    add_test(NAME linux-defragger-cpp-runtime
        COMMAND linux-defragger-cpp-runtime-test)

    add_executable(linux-defragger-privileged-helper-test
        native/privileged_helper.cpp)
    target_include_directories(linux-defragger-privileged-helper-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-privileged-helper-test PRIVATE
        ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-privileged-helper-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE LD_PRIVILEGED_HELPER_TEST_MODE=1)
    target_link_libraries(linux-defragger-privileged-helper-test PRIVATE
        linux-defragger-runtime-cpp linux-defragger-core Threads::Threads)
    add_test(NAME linux-defragger-privileged-helper-protocol
        COMMAND /bin/bash
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_privileged_helper_protocol.sh"
            "$<TARGET_FILE:linux-defragger-privileged-helper-test>")
    find_program(LD_HELPER_TEST_PYTHON NAMES python3 REQUIRED)
    add_test(NAME linux-defragger-privileged-helper-lifecycle
        COMMAND "${LD_HELPER_TEST_PYTHON}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_privileged_helper_lifecycle.py"
            "$<TARGET_FILE:linux-defragger-privileged-helper-test>")
    set_tests_properties(linux-defragger-privileged-helper-lifecycle
        PROPERTIES TIMEOUT 90)

    add_test(NAME linux-defragger-operation-engine-cpp-manifest
        COMMAND linux-defragger-operation-engine-cpp --list-plugins)
    add_test(NAME linux-defragger-mapper-cpp-manifest
        COMMAND linux-defragger-mapper-cpp --list-backends)

    add_test(NAME linux-defragger-native-malformed-matrix
        COMMAND "${LD_HELPER_TEST_PYTHON}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_malformed_matrix.py")
    set_tests_properties(linux-defragger-native-malformed-matrix PROPERTIES
        ENVIRONMENT "LINUX_DEFRAGGER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        TIMEOUT 120)

    add_executable(linux-defragger-apfs-native-test
        tests/test_apfs_native.c)
    target_include_directories(linux-defragger-apfs-native-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/apfs/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-apfs-native-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-apfs-native-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-apfs-native-test PRIVATE
        linux-defragger-apfs-native linux-defragger-core)
    add_test(NAME linux-defragger-apfs-native COMMAND linux-defragger-apfs-native-test)

    add_executable(linux-defragger-minix-native-test
        tests/test_minix_native.c)
    target_include_directories(linux-defragger-minix-native-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/minix/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-minix-native-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-minix-native-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-minix-native-test PRIVATE
        linux-defragger-minix-native linux-defragger-core)
    add_test(NAME linux-defragger-minix-native COMMAND linux-defragger-minix-native-test)

    add_test(NAME linux-defragger-minix-transaction
        COMMAND "${LD_HELPER_TEST_PYTHON}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_minix_transaction.py")
    set_tests_properties(linux-defragger-minix-transaction PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;LINUX_DEFRAGGER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        TIMEOUT 120)

    add_executable(linux-defragger-swap-native-test
        tests/test_swap_native.c)
    target_include_directories(linux-defragger-swap-native-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/swap/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-swap-native-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-swap-native-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-swap-native-test PRIVATE
        linux-defragger-swap-native linux-defragger-core)
    add_test(NAME linux-defragger-swap-native COMMAND linux-defragger-swap-native-test)

    add_executable(linux-defragger-xfs-metadata-test
        tests/test_xfs_metadata_whitebox.c
        gui/filesystems/xfs/native/xfs_common.c)
    target_include_directories(linux-defragger-xfs-metadata-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/xfs/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-xfs-metadata-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-xfs-metadata-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-xfs-metadata-test PRIVATE
        linux-defragger-core SQLite::SQLite3)
    add_test(NAME linux-defragger-xfs-metadata COMMAND linux-defragger-xfs-metadata-test)

    add_executable(linux-defragger-xfs-native-test tests/test_xfs_native.c)
    target_include_directories(linux-defragger-xfs-native-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/xfs/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-xfs-native-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-xfs-native-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-xfs-native-test PRIVATE
        linux-defragger-xfs-native linux-defragger-core SQLite::SQLite3 OpenSSL::Crypto)
    add_test(NAME linux-defragger-xfs-native COMMAND linux-defragger-xfs-native-test)

    add_executable(linux-defragger-ext-fixture tests/make_ext4_image.c)
    target_compile_options(linux-defragger-ext-fixture PRIVATE
        ${LD_WARNING_FLAGS} -Wno-deprecated-declarations)
    target_link_libraries(linux-defragger-ext-fixture PRIVATE
        PkgConfig::EXT2FS ${COM_ERR_LIBRARY})

    add_executable(linux-defragger-native-core-test tests/test_native_core.c)
    target_include_directories(linux-defragger-native-core-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-native-core-test PRIVATE ${LD_WARNING_FLAGS})
    target_link_libraries(linux-defragger-native-core-test PRIVATE linux-defragger-core)
    add_test(NAME linux-defragger-native-core COMMAND linux-defragger-native-core-test)
    add_executable(linux-defragger-mounted-image-identity-test
        tests/test_mounted_image_identity.c)
    target_include_directories(linux-defragger-mounted-image-identity-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-mounted-image-identity-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-mounted-image-identity-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-mounted-image-identity-test PRIVATE linux-defragger-core)
    add_test(NAME linux-defragger-mounted-image-identity
        COMMAND linux-defragger-mounted-image-identity-test)
    add_executable(linux-defragger-fat-volume-test
        tests/test_fat_volume.c
        gui/filesystems/fat/native/fat_io.c
        gui/filesystems/fat/native/fat_volume.c)
    target_include_directories(linux-defragger-fat-volume-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/fat/native"
        "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-fat-volume-test PRIVATE
        ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-fat-volume-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-fat-volume-test PRIVATE
        linux-defragger-core Threads::Threads)
    add_test(NAME linux-defragger-fat-volume
        COMMAND linux-defragger-fat-volume-test)
    add_executable(linux-defragger-fat-directory-test
        tests/test_fat_directory.c
        gui/filesystems/fat/native/fat_directory.c
        gui/filesystems/fat/native/fat_volume.c)
    target_include_directories(linux-defragger-fat-directory-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/fat/native"
        "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-fat-directory-test PRIVATE
        ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-fat-directory-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-fat-directory-test PRIVATE
        linux-defragger-core)
    add_test(NAME linux-defragger-fat-directory
        COMMAND linux-defragger-fat-directory-test)
    add_executable(linux-defragger-fat-journal-test
        tests/test_fat_journal.c
        gui/filesystems/fat/native/fat_journal.c
        gui/filesystems/fat/native/fat_volume.c)
    target_include_directories(linux-defragger-fat-journal-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/fat/native"
        "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-fat-journal-test PRIVATE
        ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-fat-journal-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-fat-journal-test PRIVATE
        linux-defragger-core)
    add_test(NAME linux-defragger-fat-journal
        COMMAND linux-defragger-fat-journal-test)
    add_executable(linux-defragger-fat-relayout-model-test
        tests/test_fat_relayout.c
        gui/filesystems/fat/native/fat_directory.c
        gui/filesystems/fat/native/fat_relayout.c
        gui/filesystems/fat/native/fat_volume.c)
    target_include_directories(linux-defragger-fat-relayout-model-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/fat/native"
        "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-fat-relayout-model-test PRIVATE
        ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-fat-relayout-model-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-fat-relayout-model-test PRIVATE
        linux-defragger-core)
    add_test(NAME linux-defragger-fat-relayout-model
        COMMAND linux-defragger-fat-relayout-model-test)
    add_test(NAME linux-defragger-tests
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh
                     $<TARGET_FILE:linux-defragger-fat-worker>)
endif()
