# SPDX-License-Identifier: GPL-3.0-or-later

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" DEFRAGMENTER_PACKAGE_VERSION)
string(STRIP "${DEFRAGMENTER_PACKAGE_VERSION}" DEFRAGMENTER_PACKAGE_VERSION)
if(NOT DEFRAGMENTER_PACKAGE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+-[0-9]+$")
    message(FATAL_ERROR "VERSION must contain a release such as 1.8.0-49")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

set(INFILTRATR_COMMON_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/infiltratr-common")
set(INFILTRATR_COMMON_URL
    "https://github.com/Infiltrator-Projects/Libraries.git")
set(INFILTRATR_COMMON_TAG "v1.19.46")
set(INFILTRATR_COMMON_EXPECTED_VERSION "1.19.46")
set(INFILTRATR_COMMON_EXPECTED_COMMIT
    "fc2c72cb038a8e4c1e0bc62145adf533fbfd7923")

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
            "Unable to retrieve Libraries ${INFILTRATR_COMMON_EXPECTED_VERSION}.")
    endif()
endif()

file(READ "${INFILTRATR_COMMON_DIR}/VERSION" INFILTRATR_COMMON_VERSION)
string(STRIP "${INFILTRATR_COMMON_VERSION}" INFILTRATR_COMMON_VERSION)
if(NOT INFILTRATR_COMMON_VERSION STREQUAL INFILTRATR_COMMON_EXPECTED_VERSION)
    message(FATAL_ERROR
        "Libraries ${INFILTRATR_COMMON_EXPECTED_VERSION} is required; found ${INFILTRATR_COMMON_VERSION}.")
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
            "Libraries must be pinned to ${INFILTRATR_COMMON_EXPECTED_COMMIT}; found ${INFILTRATR_COMMON_COMMIT}.")
    endif()
endif()

option(LD_ENABLE_WERROR "Treat first-party compiler warnings as errors" OFF)
option(LD_ENABLE_SANITIZERS "Enable address and undefined-behaviour sanitizers" OFF)
option(LD_ENABLE_HARDENING "Enable release compiler/linker hardening" ON)
option(LD_GENERIC_AMD64 "Build for the baseline x86-64 instruction set" OFF)
option(LD_NATIVE_OPTIMIZATION "Optimise native code for this build machine" OFF)

if(LD_GENERIC_AMD64)
    set(DEFRAGMENTER_BUILD_PROFILE "generic")
    set(DEFRAGMENTER_BUILD_LABEL "Generic / APT package")
elseif(LD_NATIVE_OPTIMIZATION)
    set(DEFRAGMENTER_BUILD_PROFILE "native")
    set(DEFRAGMENTER_BUILD_LABEL "Native / local machine compile")
else()
    set(DEFRAGMENTER_BUILD_PROFILE "source")
    set(DEFRAGMENTER_BUILD_LABEL "Source / development build")
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

set(LD_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2)
if(LD_ENABLE_WERROR)
    list(APPEND LD_WARNING_FLAGS -Werror)
endif()
if(LD_ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(LD_ENABLE_HARDENING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(CheckCXXCompilerFlag)
    include(CheckLinkerFlag)

    check_c_compiler_flag("-fstack-protector-strong" LD_C_STACK_PROTECTOR_STRONG)
    check_cxx_compiler_flag("-fstack-protector-strong" LD_CXX_STACK_PROTECTOR_STRONG)
    if(LD_C_STACK_PROTECTOR_STRONG AND LD_CXX_STACK_PROTECTOR_STRONG)
        add_compile_options(-fstack-protector-strong)
    endif()

    check_c_compiler_flag("-fPIE" LD_C_FPIE)
    check_cxx_compiler_flag("-fPIE" LD_CXX_FPIE)
    if(LD_C_FPIE AND LD_CXX_FPIE)
        add_compile_options(-fPIE)
        check_linker_flag(C "-pie" LD_C_PIE_LINK)
        check_linker_flag(CXX "-pie" LD_CXX_PIE_LINK)
        if(LD_C_PIE_LINK AND LD_CXX_PIE_LINK)
            add_link_options(-pie)
        endif()
    endif()

    check_linker_flag(C "-Wl,-z,relro" LD_C_RELRO)
    check_linker_flag(CXX "-Wl,-z,relro" LD_CXX_RELRO)
    if(LD_C_RELRO AND LD_CXX_RELRO)
        add_link_options("-Wl,-z,relro")
    endif()

    check_linker_flag(C "-Wl,-z,now" LD_C_BIND_NOW)
    check_linker_flag(CXX "-Wl,-z,now" LD_CXX_BIND_NOW)
    if(LD_C_BIND_NOW AND LD_CXX_BIND_NOW)
        add_link_options("-Wl,-z,now")
    endif()
endif()

# Libraries owns generic parsing, arithmetic, byte-order, exact-I/O,
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

add_library(defragmenter-core STATIC
    src/core/ld_runtime.c
    src/core/ld_io.c
    src/core/ld_device.c
    src/core/ld_stop.c
    src/core/ld_path.c
    src/core/ld_protocol.c)
target_include_directories(defragmenter-core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-core PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-core PUBLIC _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-core PUBLIC InfiltratrCommon::Common)

# Application-owned, filesystem-neutral callbacks supplied to trusted SOs.
add_library(defragmenter-fs-host-services STATIC native/ifs_defrag_services_v1.c)
target_include_directories(defragmenter-fs-host-services PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
target_compile_options(defragmenter-fs-host-services PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-fs-host-services PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-fs-host-services PUBLIC defragmenter-core)


# C++17 owns application-level contracts and orchestration where value types,
# scoped state and RAII improve the implementation. Filesystem parsing and
# mutation remain in the existing native C engines.
add_library(defragmenter-runtime-cpp STATIC
    native/desktop_live_map.cpp
    native/desktop_map_geometry.cpp
    native/desktop_policy.cpp
    native/helper_policy.cpp
    native/json.cpp
    native/map.cpp
    native/process.cpp
    native/runtime.cpp)
target_include_directories(defragmenter-runtime-cpp PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-runtime-cpp PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-runtime-cpp PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-runtime-cpp PUBLIC
    defragmenter-core InfiltratrCommon::Common Threads::Threads OpenSSL::Crypto)

add_executable(defragmenter-operation-engine-cpp
    native/operation_engine.cpp)
set_target_properties(defragmenter-operation-engine-cpp PROPERTIES
    OUTPUT_NAME defragmenter-operation-engine)
target_include_directories(defragmenter-operation-engine-cpp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-operation-engine-cpp PRIVATE
    ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-operation-engine-cpp PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-operation-engine-cpp PRIVATE
    defragmenter-runtime-cpp defragmenter-core)

add_executable(defragmenter-mapper-cpp
    native/mapper.cpp)
set_target_properties(defragmenter-mapper-cpp PROPERTIES
    OUTPUT_NAME defragmenter-mapper)
target_include_directories(defragmenter-mapper-cpp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-mapper-cpp PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-mapper-cpp PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-mapper-cpp PRIVATE
    defragmenter-runtime-cpp defragmenter-core)

add_executable(defragmenter-privileged-helper-cpp
    native/privileged_helper.cpp)
set_target_properties(defragmenter-privileged-helper-cpp PROPERTIES
    OUTPUT_NAME defragmenter-privileged-helper)
target_include_directories(defragmenter-privileged-helper-cpp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-privileged-helper-cpp PRIVATE
    ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-privileged-helper-cpp PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-privileged-helper-cpp PRIVATE
    defragmenter-runtime-cpp defragmenter-core Threads::Threads)

# A child-process host locates the matching root-owned Filesystems module.
# The filesystem algorithms and durable journal implementation live in FAT*.so.
add_executable(defragmenter-fat-worker native/fat_module_loader.c)
target_include_directories(defragmenter-fat-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-fat-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-fat-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-fat-worker PRIVATE
    defragmenter-core Threads::Threads ${CMAKE_DL_LIBS})

# Test-only overrides are compiled into a separate binary, never the installed host.
set(FAT_DEFRAG_SOURCE_DIR "" CACHE PATH "Filesystems FAT defrag source for unit tests")
set(FAT_DEFRAG_TEST_MODULE_PARENT "" CACHE PATH "Disposable FAT module test parent")
set(FAT_WORKER_FOR_TESTS "$<TARGET_FILE:defragmenter-fat-worker>")
if(BUILD_TESTING AND FAT_DEFRAG_TEST_MODULE_PARENT)
    add_executable(defragmenter-fat-test-worker native/fat_module_loader.c)
    target_include_directories(defragmenter-fat-test-worker PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-fat-test-worker PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-fat-test-worker PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE IFS_FAT_TESTING
        "IFS_FAT_MODULE_PARENT=\"${FAT_DEFRAG_TEST_MODULE_PARENT}\"")
    target_link_libraries(defragmenter-fat-test-worker PRIVATE
        defragmenter-core Threads::Threads ${CMAKE_DL_LIBS})
    set(FAT_WORKER_FOR_TESTS "$<TARGET_FILE:defragmenter-fat-test-worker>")
endif()

# Only APFS.so owns APFS filesystem analysis, staged writes and recovery.
add_executable(defragmenter-apfs-worker native/apfs_module_loader.c)
target_include_directories(defragmenter-apfs-worker PRIVATE
 "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-apfs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-apfs-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-apfs-worker PRIVATE defragmenter-fs-host-services defragmenter-filesystem-module-host)
if(BUILD_TESTING AND OFS_DEFRAG_TEST_MODULE_PARENT)
 add_executable(defragmenter-apfs-test-worker native/apfs_module_loader.c)
 target_include_directories(defragmenter-apfs-test-worker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
 target_compile_options(defragmenter-apfs-test-worker PRIVATE ${LD_WARNING_FLAGS})
 target_compile_definitions(defragmenter-apfs-test-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
 target_link_libraries(defragmenter-apfs-test-worker PRIVATE defragmenter-fs-host-services defragmenter-filesystem-module-test-host)
endif()
set(APFS_NATIVE_TEST_SOURCE_DIR "" CACHE PATH "Independent APFS source checkout for test oracles")
if(BUILD_TESTING AND APFS_NATIVE_TEST_SOURCE_DIR)
 add_library(defragmenter-apfs-fixture-native STATIC "${APFS_NATIVE_TEST_SOURCE_DIR}/native/apfs_native.c")
 target_include_directories(defragmenter-apfs-fixture-native PUBLIC "${APFS_NATIVE_TEST_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
 target_compile_options(defragmenter-apfs-fixture-native PRIVATE ${LD_WARNING_FLAGS})
 target_compile_definitions(defragmenter-apfs-fixture-native PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
 target_link_libraries(defragmenter-apfs-fixture-native PUBLIC defragmenter-core)
endif()

# Minix exact analysis, offline staging, mutation and recovery are native C.
# Minix layout and writer own canonical MINIX.so; host is format-neutral.
add_executable(defragmenter-minix-worker native/minix_module_loader.c)
target_include_directories(defragmenter-minix-worker PRIVATE
 "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-minix-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-minix-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-minix-worker PRIVATE
 defragmenter-fs-host-services defragmenter-filesystem-module-host)

find_package(SQLite3 REQUIRED)

add_library(defragmenter-xfs-native STATIC
    gui/filesystems/xfs/native/xfs_common.c
    gui/filesystems/xfs/native/xfs_catalog.c
    gui/filesystems/xfs/native/xfs_plan.c
    gui/filesystems/xfs/native/xfs_metadata.c)
target_include_directories(defragmenter-xfs-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/xfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-xfs-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-xfs-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-xfs-native PUBLIC
    defragmenter-core SQLite::SQLite3 OpenSSL::Crypto)

# EXT2/3/4 production parsing, allocation accounting and mapping mutation are
# first-party native C. libext2fs is retained only below as an independent
# BUILD_TESTING fixture/oracle and is not a production/runtime dependency.
add_library(defragmenter-ext-native STATIC
    gui/filesystems/ext4/native/ext_disk.c
    gui/filesystems/ext4/native/ext_common.c
    gui/filesystems/ext4/native/ext_catalog.c
    gui/filesystems/ext4/native/ext_plan.c
    gui/filesystems/ext4/native/ext_workspace.c)
target_include_directories(defragmenter-ext-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ext4/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-ext-native PRIVATE
    ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-ext-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-ext-native PUBLIC
    defragmenter-core SQLite::SQLite3 OpenSSL::Crypto)

add_executable(defragmenter-ext-worker
    gui/filesystems/ext4/native/ext_worker.c)
target_include_directories(defragmenter-ext-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ext4/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-ext-worker PRIVATE
    ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-ext-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-ext-worker PRIVATE
    defragmenter-ext-native defragmenter-core SQLite::SQLite3 OpenSSL::Crypto)

add_library(defragmenter-ntfs-native STATIC
    gui/filesystems/ntfs/native/ntfs_common.c
    gui/filesystems/ntfs/native/ntfs_catalog.c
    gui/filesystems/ntfs/native/ntfs_plan.c
    gui/filesystems/ntfs/native/ntfs_plan_db.cpp)
target_include_directories(defragmenter-ntfs-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ntfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-ntfs-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-ntfs-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-ntfs-native PUBLIC
    defragmenter-core SQLite::SQLite3 OpenSSL::Crypto Threads::Threads)

# The installed worker has no internal NTFS parsing or relocation engine.
# The retained native static library is still used by Test Media until its
# format/populate functionality moves to NTFS.so.
add_executable(defragmenter-ntfs-worker native/ntfs_module_loader.c)
target_include_directories(defragmenter-ntfs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-ntfs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-ntfs-worker PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_link_libraries(defragmenter-ntfs-worker PRIVATE
    defragmenter-filesystem-module-host defragmenter-defrag-services)

add_library(defragmenter-exfat-native STATIC
    gui/filesystems/exfat/native/exfat_common.c
    gui/filesystems/exfat/native/exfat_plan.c
    gui/filesystems/exfat/native/exfat_relayout.c)
target_include_directories(defragmenter-exfat-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/exfat/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-exfat-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-exfat-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-exfat-native PUBLIC
    defragmenter-core OpenSSL::Crypto)

add_executable(defragmenter-exfat-worker
    gui/filesystems/exfat/native/exfat_worker.c)
target_include_directories(defragmenter-exfat-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/exfat/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-exfat-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-exfat-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-exfat-worker PRIVATE
    defragmenter-exfat-native defragmenter-core OpenSSL::Crypto)

add_library(defragmenter-affs-native STATIC
    gui/filesystems/affs/native/affs_native.c)
target_include_directories(defragmenter-affs-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/affs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-affs-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-affs-native PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-affs-native PUBLIC defragmenter-core)

# Filesystem-neutral process host: the loaded SO owns the on-disk semantics,
# placement algorithm and journal schema. The existing AFFS registry entry
# dispatches OFS here and retains the FFS worker until its separate migration.
add_library(defragmenter-filesystem-module-host STATIC native/filesystem_module_host.c)
target_include_directories(defragmenter-filesystem-module-host PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-filesystem-module-host PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-filesystem-module-host PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-filesystem-module-host PUBLIC
    defragmenter-core OpenSSL::Crypto ${CMAKE_DL_LIBS})

# SWAP owns its native analyser, map and Test Media formatter.
# The installed application has only a trusted process-hosted SO loader.
add_executable(defragmenter-swap-worker native/swap_module_loader.c)
target_include_directories(defragmenter-swap-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-swap-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-swap-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-swap-worker PRIVATE defragmenter-filesystem-module-host)

add_executable(defragmenter-affs-worker
    gui/filesystems/affs/native/affs_worker.c)
target_include_directories(defragmenter-affs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/affs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-affs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-affs-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-affs-worker PRIVATE defragmenter-affs-native
    defragmenter-core defragmenter-filesystem-module-host)

set(OFS_DEFRAG_TEST_MODULE_PARENT "" CACHE PATH "Disposable OFS module test parent")
set(AFFS_WORKER_FOR_TESTS "$<TARGET_FILE:defragmenter-affs-worker>")
if(BUILD_TESTING AND OFS_DEFRAG_TEST_MODULE_PARENT)
    add_library(defragmenter-filesystem-module-test-host STATIC native/filesystem_module_host.c)
    target_include_directories(defragmenter-filesystem-module-test-host PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/native"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-filesystem-module-test-host PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-filesystem-module-test-host PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE IFS_MODULE_TESTING)
    target_link_libraries(defragmenter-filesystem-module-test-host PUBLIC
        defragmenter-core OpenSSL::Crypto ${CMAKE_DL_LIBS})
    add_executable(defragmenter-affs-test-worker
        gui/filesystems/affs/native/affs_worker.c)
    target_include_directories(defragmenter-affs-test-worker PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/affs/native" "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-affs-test-worker PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-affs-test-worker PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE IFS_MODULE_TESTING)
    target_link_libraries(defragmenter-affs-test-worker PRIVATE
        defragmenter-affs-native defragmenter-core defragmenter-filesystem-module-test-host)
    set(AFFS_WORKER_FOR_TESTS "$<TARGET_FILE:defragmenter-affs-test-worker>")
    add_executable(defragmenter-minix-test-worker native/minix_module_loader.c)
    target_include_directories(defragmenter-minix-test-worker PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-minix-test-worker PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-minix-test-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-minix-test-worker PRIVATE
        defragmenter-fs-host-services defragmenter-filesystem-module-test-host)
    # Test worker shares the exact production loader but trusts only a
    # disposable, user-owned SO root. Never install the test worker.
    add_executable(defragmenter-swap-test-worker native/swap_module_loader.c)
    target_include_directories(defragmenter-swap-test-worker PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-swap-test-worker PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-swap-test-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-swap-test-worker PRIVATE defragmenter-filesystem-module-test-host)

endif()
# HFS+/HFSX parse, Defrag, Growth Defrag and recovery are in HFSPLUS.so.
# Only this format-neutral, fail-closed loader remains in Defragmenter.
add_executable(defragmenter-hfsplus-worker native/hfsplus_module_loader.c)
target_include_directories(defragmenter-hfsplus-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-hfsplus-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-hfsplus-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-hfsplus-worker PRIVATE
    defragmenter-fs-host-services defragmenter-filesystem-module-host)
if(BUILD_TESTING AND OFS_DEFRAG_TEST_MODULE_PARENT)
    add_executable(defragmenter-hfsplus-test-worker native/hfsplus_module_loader.c)
    target_include_directories(defragmenter-hfsplus-test-worker PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-hfsplus-test-worker PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-hfsplus-test-worker PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-hfsplus-test-worker PRIVATE
        defragmenter-fs-host-services defragmenter-filesystem-module-test-host)
endif()

add_executable(defragmenter-xfs-worker
    gui/filesystems/xfs/native/xfs_worker.c)
target_include_directories(defragmenter-xfs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/xfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-xfs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-xfs-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-xfs-worker PRIVATE
    defragmenter-xfs-native defragmenter-core SQLite::SQLite3 OpenSSL::Crypto)

# Classic HFS belongs to the independent trusted HFS.so.
# The executable retains its original hfs_analyser name for compatibility.
add_executable(defragmenter-hfs-worker native/hfs_module_loader.c)
target_include_directories(defragmenter-hfs-worker PRIVATE
 "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
target_compile_options(defragmenter-hfs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(defragmenter-hfs-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(defragmenter-hfs-worker PRIVATE
 defragmenter-fs-host-services defragmenter-filesystem-module-host)
set_target_properties(defragmenter-hfs-worker PROPERTIES OUTPUT_NAME hfs_analyser)
if(BUILD_TESTING AND OFS_DEFRAG_TEST_MODULE_PARENT)
 add_executable(defragmenter-hfs-test-worker native/hfs_module_loader.c)
 target_include_directories(defragmenter-hfs-test-worker PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
 target_compile_options(defragmenter-hfs-test-worker PRIVATE ${LD_WARNING_FLAGS})
 target_compile_definitions(defragmenter-hfs-test-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
 target_link_libraries(defragmenter-hfs-test-worker PRIVATE
  defragmenter-fs-host-services defragmenter-filesystem-module-test-host)
endif()

install(TARGETS defragmenter-affs-worker
        RUNTIME DESTINATION /System/Commands/Defragmenter/affs)
install(TARGETS defragmenter-fat-worker
        RUNTIME DESTINATION /System/Commands/Defragmenter/fat)
install(TARGETS defragmenter-apfs-worker
        RUNTIME DESTINATION /System/Commands/Defragmenter/apfs)
install(TARGETS defragmenter-minix-worker
        RUNTIME DESTINATION /System/Commands/Defragmenter/minix)
install(TARGETS defragmenter-swap-worker
        RUNTIME DESTINATION /System/Commands/Defragmenter/swap)
install(TARGETS defragmenter-xfs-worker
        RUNTIME DESTINATION /System/Commands/Defragmenter/xfs)
install(TARGETS defragmenter-ext-worker
        RUNTIME DESTINATION /System/Commands/Defragmenter/ext4)
install(TARGETS defragmenter-ntfs-worker
        RUNTIME DESTINATION /System/Commands/Defragmenter/ntfs)
install(TARGETS defragmenter-exfat-worker
        RUNTIME DESTINATION /System/Commands/Defragmenter/exfat)
install(TARGETS defragmenter-hfsplus-worker
        RUNTIME DESTINATION /System/Commands/Defragmenter/hfsplus)
install(TARGETS defragmenter-hfs-worker
        RUNTIME DESTINATION /System/Commands/Defragmenter/hfs)

# Native C++ application services are the only mapper, operation dispatcher
# and privileged helper in both source and installed application.
install(TARGETS
        defragmenter-operation-engine-cpp
        defragmenter-mapper-cpp
        defragmenter-privileged-helper-cpp
        RUNTIME DESTINATION /System/Commands/Defragmenter)

# The installed product is native C/C++. The retired Python presentation
# tree remains source/test material only and is never installed at runtime.
install(FILES packaging/io.github.defragmenter.desktop
        DESTINATION share/applications)
install(FILES packaging/io.github.defragmenter.png
        DESTINATION share/icons/hicolor/96x96/apps)
install(FILES packaging/io.github.defragmenter.png
        DESTINATION share/app-install/icons
        RENAME defragmenter.png)
install(FILES packaging/io.github.defragmenter.png
        DESTINATION /System/Resources/Icons
        RENAME defragmenter.png)
install(DIRECTORY gui/ui/art
        DESTINATION /System/Resources/Themes/Defragmenter
        FILES_MATCHING PATTERN "*.jpg")

# Install only documentation that is part of the self-contained application
# source tree. Canonical repository documentation lives at ../docs and is not
# duplicated into the native installer payload.
install(FILES README.md
        DESTINATION /System/Resources/Documentation/Defragmenter)
install(FILES LICENSE
        DESTINATION /System/Resources/Documentation/Defragmenter RENAME COPYING.GPL-3.0)
include(CTest)
if(BUILD_TESTING)
    add_executable(defragmenter-desktop-live-map-test
        tests/test_desktop_live_map.cpp)
    target_link_libraries(defragmenter-desktop-live-map-test PRIVATE
        defragmenter-runtime-cpp)
    target_compile_options(defragmenter-desktop-live-map-test PRIVATE
        ${LD_WARNING_FLAGS})
    add_test(NAME defragmenter-desktop-live-map
        COMMAND defragmenter-desktop-live-map-test)
    add_executable(defragmenter-desktop-policy-test
        tests/test_desktop_policy.cpp)
    target_link_libraries(defragmenter-desktop-policy-test PRIVATE
        defragmenter-runtime-cpp)
    target_compile_options(defragmenter-desktop-policy-test PRIVATE
        ${LD_WARNING_FLAGS})
    add_test(NAME defragmenter-desktop-policy
        COMMAND defragmenter-desktop-policy-test)
    add_executable(defragmenter-cpp-runtime-test
        tests/test_cpp_runtime.cpp)
    target_include_directories(defragmenter-cpp-runtime-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-cpp-runtime-test PRIVATE
        ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-cpp-runtime-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-cpp-runtime-test PRIVATE
        defragmenter-runtime-cpp defragmenter-core)
    add_test(NAME defragmenter-cpp-runtime
        COMMAND defragmenter-cpp-runtime-test)

    add_executable(defragmenter-ext-bitmap-cache-test tests/test_ext_bitmap_cache.c)
    target_link_libraries(defragmenter-ext-bitmap-cache-test PRIVATE defragmenter-core)
    target_compile_options(defragmenter-ext-bitmap-cache-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-ext-bitmap-cache-test PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    add_test(NAME defragmenter-ext-bitmap-cache COMMAND defragmenter-ext-bitmap-cache-test)

    add_executable(defragmenter-ntfs-stream-index-test tests/test_ntfs_stream_index.c)
    target_link_libraries(defragmenter-ntfs-stream-index-test PRIVATE defragmenter-ntfs-native)
    target_compile_options(defragmenter-ntfs-stream-index-test PRIVATE ${LD_WARNING_FLAGS})
    set_property(TARGET defragmenter-ntfs-stream-index-test PROPERTY LINKER_LANGUAGE CXX)
    add_test(NAME defragmenter-ntfs-stream-index COMMAND defragmenter-ntfs-stream-index-test)
    set_tests_properties(defragmenter-ntfs-stream-index PROPERTIES TIMEOUT 20)

    add_executable(defragmenter-privileged-helper-test
        native/privileged_helper.cpp)
    target_include_directories(defragmenter-privileged-helper-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-privileged-helper-test PRIVATE
        ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-privileged-helper-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE LD_PRIVILEGED_HELPER_TEST_MODE=1)
    target_link_libraries(defragmenter-privileged-helper-test PRIVATE
        defragmenter-runtime-cpp defragmenter-core Threads::Threads)
    add_test(NAME defragmenter-privileged-helper-protocol
        COMMAND /bin/bash
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_privileged_helper_protocol.sh"
            "$<TARGET_FILE:defragmenter-privileged-helper-test>")
    find_program(LD_HELPER_TEST_PYTHON NAMES python3 REQUIRED)
    add_test(NAME defragmenter-privileged-helper-lifecycle
        COMMAND "${LD_HELPER_TEST_PYTHON}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_privileged_helper_lifecycle.py"
            "$<TARGET_FILE:defragmenter-privileged-helper-test>")
    set_tests_properties(defragmenter-privileged-helper-lifecycle
        PROPERTIES TIMEOUT 90)

    add_test(NAME defragmenter-operation-engine-cpp-manifest
        COMMAND defragmenter-operation-engine-cpp --list-plugins)
    add_test(NAME defragmenter-mapper-cpp-manifest
        COMMAND defragmenter-mapper-cpp --list-backends)

    add_test(NAME defragmenter-native-malformed-matrix
        COMMAND "${LD_HELPER_TEST_PYTHON}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_malformed_matrix.py")
    set_tests_properties(defragmenter-native-malformed-matrix PROPERTIES
        ENVIRONMENT "DEFRAGMENTER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        TIMEOUT 180)

    if(APFS_NATIVE_TEST_SOURCE_DIR)
    add_executable(defragmenter-apfs-native-test
        tests/test_apfs_native.c)
    target_include_directories(defragmenter-apfs-native-test PRIVATE
        "${APFS_NATIVE_TEST_SOURCE_DIR}/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-apfs-native-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-apfs-native-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-apfs-native-test PRIVATE
        defragmenter-apfs-fixture-native defragmenter-core)
    add_test(NAME defragmenter-apfs-native COMMAND defragmenter-apfs-native-test)
    add_test(NAME defragmenter-apfs-native-integration
        COMMAND "${LD_HELPER_TEST_PYTHON}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_apfs_native.py")
    set_tests_properties(defragmenter-apfs-native-integration PROPERTIES
        ENVIRONMENT "DEFRAGMENTER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        TIMEOUT 120)

    endif()
    # Original Minix native fixture and regression now live in Filesystem-Minix.
    # Only the independently built fixture is used to exercise Defragmenter's
    # SO loader in transaction tests.
    set(MINIX_NATIVE_FIXTURE_PATH "" CACHE FILEPATH
        "Path to the qualified independent Minix test-media fixture")
    if(MINIX_NATIVE_FIXTURE_PATH AND BUILD_TESTING)
        add_test(NAME defragmenter-minix-transaction
            COMMAND "${LD_HELPER_TEST_PYTHON}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_minix_transaction.py")
        if(TARGET defragmenter-minix-test-worker)
            set(MINIX_TEST_WORKER "$<TARGET_FILE:defragmenter-minix-test-worker>")
        else()
            set(MINIX_TEST_WORKER "$<TARGET_FILE:defragmenter-minix-worker>")
        endif()
        set_tests_properties(defragmenter-minix-transaction PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;DEFRAGMENTER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR};DEFRAGMENTER_MINIX_FIXTURE=${MINIX_NATIVE_FIXTURE_PATH};DEFRAGMENTER_MINIX_WORKER=${MINIX_TEST_WORKER}"
            TIMEOUT 120)
    endif()

    # Native SWAP engine regression moved to Filesystem-Swap's independent CI.

    add_executable(defragmenter-xfs-metadata-test
        tests/test_xfs_metadata_whitebox.c
        gui/filesystems/xfs/native/xfs_common.c)
    target_include_directories(defragmenter-xfs-metadata-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/xfs/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-xfs-metadata-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-xfs-metadata-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-xfs-metadata-test PRIVATE
        defragmenter-core SQLite::SQLite3)
    add_test(NAME defragmenter-xfs-metadata COMMAND defragmenter-xfs-metadata-test)

    add_executable(defragmenter-xfs-native-test tests/test_xfs_native.c)
    target_include_directories(defragmenter-xfs-native-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/xfs/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-xfs-native-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-xfs-native-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-xfs-native-test PRIVATE
        defragmenter-xfs-native defragmenter-core SQLite::SQLite3 OpenSSL::Crypto)
    add_test(NAME defragmenter-xfs-native COMMAND defragmenter-xfs-native-test)

    # libext2fs is a test-only independent fixture/oracle. A production build
    # with BUILD_TESTING=OFF must configure and link without e2fsprogs headers
    # or libraries.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EXT2FS REQUIRED IMPORTED_TARGET ext2fs)
    find_library(COM_ERR_LIBRARY com_err REQUIRED)

    add_executable(defragmenter-ext-fixture tests/make_ext4_image.c)
    target_compile_options(defragmenter-ext-fixture PRIVATE
        ${LD_WARNING_FLAGS} -Wno-deprecated-declarations)
    target_link_libraries(defragmenter-ext-fixture PRIVATE
        PkgConfig::EXT2FS ${COM_ERR_LIBRARY})

    add_executable(defragmenter-native-core-test tests/test_native_core.c)
    target_include_directories(defragmenter-native-core-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-native-core-test PRIVATE ${LD_WARNING_FLAGS})
    target_link_libraries(defragmenter-native-core-test PRIVATE defragmenter-core)
    add_test(NAME defragmenter-native-core COMMAND defragmenter-native-core-test)
    add_executable(defragmenter-mounted-image-identity-test
        tests/test_mounted_image_identity.c)
    target_include_directories(defragmenter-mounted-image-identity-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(defragmenter-mounted-image-identity-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-mounted-image-identity-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(defragmenter-mounted-image-identity-test PRIVATE defragmenter-core)
    add_test(NAME defragmenter-mounted-image-identity
        COMMAND defragmenter-mounted-image-identity-test)
    # Keep the existing filesystem unit tests, compiling their source from
    # Filesystems instead of retaining a second engine inside Defragmenter.
    if(FAT_DEFRAG_SOURCE_DIR)
        if(NOT EXISTS "${FAT_DEFRAG_SOURCE_DIR}/fat_volume.c")
            message(FATAL_ERROR "FAT_DEFRAG_SOURCE_DIR must name the Filesystems defrag source")
        endif()
        function(ld_fat_module_unit_test target test_source)
            add_executable(${target} "${test_source}"
                tests/fat_module_unit_host.c ${ARGN})
            target_include_directories(${target} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/native"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
                "${FAT_DEFRAG_SOURCE_DIR}" "${FAT_DEFRAG_SOURCE_DIR}/host"
                "${LD_GENERATED_DIR}")
            target_compile_options(${target} PRIVATE ${LD_WARNING_FLAGS})
            target_compile_definitions(${target} PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
            target_link_libraries(${target} PRIVATE defragmenter-core Threads::Threads)
        endfunction()
        ld_fat_module_unit_test(defragmenter-fat-volume-test tests/test_fat_volume.c
            "${FAT_DEFRAG_SOURCE_DIR}/fat_io.c" "${FAT_DEFRAG_SOURCE_DIR}/fat_volume.c")
        ld_fat_module_unit_test(defragmenter-fat-directory-test tests/test_fat_directory.c
            "${FAT_DEFRAG_SOURCE_DIR}/fat_directory.c" "${FAT_DEFRAG_SOURCE_DIR}/fat_volume.c")
        ld_fat_module_unit_test(defragmenter-fat-journal-test tests/test_fat_journal.c
            "${FAT_DEFRAG_SOURCE_DIR}/fat_journal.c" "${FAT_DEFRAG_SOURCE_DIR}/fat_volume.c")
        ld_fat_module_unit_test(defragmenter-fat-relayout-model-test tests/test_fat_relayout.c
            "${FAT_DEFRAG_SOURCE_DIR}/fat_directory.c" "${FAT_DEFRAG_SOURCE_DIR}/fat_relayout.c"
            "${FAT_DEFRAG_SOURCE_DIR}/fat_volume.c")
        add_test(NAME defragmenter-fat-volume COMMAND defragmenter-fat-volume-test)
        add_test(NAME defragmenter-fat-directory COMMAND defragmenter-fat-directory-test)
        add_test(NAME defragmenter-fat-journal COMMAND defragmenter-fat-journal-test)
        add_test(NAME defragmenter-fat-relayout-model COMMAND defragmenter-fat-relayout-model-test)
    else()
        message(STATUS "FAT source unit tests require FAT_DEFRAG_SOURCE_DIR")
    endif()
    add_executable(defragmenter-fat12-small-workspace-fixture
        tests/fat12_small_workspace_fixture.c)
    target_compile_options(defragmenter-fat12-small-workspace-fixture PRIVATE
        ${LD_WARNING_FLAGS})
    target_compile_definitions(defragmenter-fat12-small-workspace-fixture PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)

    add_test(NAME defragmenter-tests
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh
                     ${FAT_WORKER_FOR_TESTS})
    set_tests_properties(defragmenter-tests PROPERTIES
        ENVIRONMENT "DEFRAGMENTER_FAT_WORKER=${FAT_WORKER_FOR_TESTS}")
    if(TARGET defragmenter-fat-test-worker)
        add_test(NAME defragmenter-fat-module-recovery
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fat_module_recovery.sh"
                ${FAT_WORKER_FOR_TESTS})
        set(FAT_BOUNDARY_FIXTURE "")
        if(FAT_DEFRAG_SOURCE_DIR)
            get_filename_component(FAT_BOUNDARY_FIXTURE
                "${FAT_DEFRAG_SOURCE_DIR}/../../../../tests/fat_boundary_fixture.py" ABSOLUTE)
        endif()
        add_test(NAME defragmenter-fat-module-safety
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fat_module_safety.sh"
                ${FAT_WORKER_FOR_TESTS} "${FAT_DEFRAG_TEST_MODULE_PARENT}/Filesystems"
                "${FAT_BOUNDARY_FIXTURE}")
    endif()
endif()
