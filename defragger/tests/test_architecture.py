#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Enforce the current C-first native architecture with selective C++ RAII."""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT.parent
GUI = ROOT / "gui"


NATIVE_WRITERS = {
    "fat12": "fat-native",
    "fat16": "fat-native",
    "fat32": "fat-native",
    "exfat": "exfat-native",
    "ntfs": "ntfs-native",
    "ext4": "ext-native",
    "xfs": "xfs-native",
    "affs": "affs-native",
    "apfs": "apfs-native",
    "btrfs": "btrfs-native",
    "sfs": "sfs-native",
    "pfs3": "pfs3-native",
    "hfs": "hfs-native",
    "hfsplus": "hfsplus-native",
    "minix": "minix-native",
}


def _cmake_source() -> str:
    paths = [ROOT / "CMakeLists.txt", *sorted((ROOT / "cmake").glob("*.cmake"))]
    return "\n".join(path.read_text() for path in paths if path.is_file())


def test_top_level_cmake_owns_native_language_declaration() -> None:
    root_cmake = (ROOT / "CMakeLists.txt").read_text()
    project_fragment = (ROOT / "cmake" / "project.cmake").read_text()
    assert root_cmake.count("cmake_minimum_required(VERSION 3.20)") == 1
    assert root_cmake.count("project(linux_defragger VERSION 1.8.0 LANGUAGES C CXX)") == 1
    assert root_cmake.index("cmake_minimum_required(VERSION 3.20)") < root_cmake.index("project(linux_defragger VERSION 1.8.0 LANGUAGES C CXX)")
    assert "cmake_minimum_required(" not in project_fragment
    assert "project(" not in project_fragment


def test_native_registry_is_the_single_capability_authority() -> None:
    runtime = (ROOT / "native" / "runtime.cpp").read_text()
    mapper = (ROOT / "native" / "mapper.cpp").read_text()
    operation_engine = (ROOT / "native" / "operation_engine.cpp").read_text()

    assert "result.reserve(18)" in runtime
    assert "registry_manifest_json" in runtime
    assert "registry_manifest_json(2U)" in mapper
    assert "backend_registry()" in mapper
    assert "backend_by_fstype(filesystem)" in operation_engine
    assert "operation_for(*backend, operation)" in operation_engine
    for filesystem, worker in NATIVE_WRITERS.items():
        assert f'"{worker}"' in runtime, f"{filesystem} lost native worker {worker}"
    for readonly in ("ufs", "zfs", "swap"):
        marker = f'"{readonly}"'
        assert marker in runtime

def test_unqualified_ufs_mutation_is_fail_closed_in_the_installed_worker() -> None:
    source = (GUI / "filesystems" / "ufs" / "native" / "ufs_worker.c").read_text()
    refusal = source.index("UFS mutation is not production-qualified")
    mutation_dispatch = source.index("const char *mode = argv[1]")
    assert refusal < mutation_dispatch

    assert not (GUI / "filesystems" / "ufs" / "plugin.py").exists()
    assert "ld_device_format_identity" in source

    runtime = (ROOT / "native" / "runtime.cpp").read_text()
    assert '"ufs", "Solaris/BSD UFS"' in runtime
    ufs_entry = runtime.split('"ufs", "Solaris/BSD UFS"', 1)[1].split("result.push_back", 1)[0]
    assert 'read, "variant-dependent", "ufs-native"' in ufs_entry
    assert "standard_write_ops" not in ufs_entry


def test_dispatch_is_filesystem_neutral() -> None:
    source = (ROOT / "native" / "operation_engine.cpp").read_text()
    for required in (
        "backend_by_fstype(filesystem)",
        "operation_for(*backend, operation)",
        "resolve_program(specification->worker)",
        "without_options(forwarded, specification->unsupported_options)",
    ):
        assert required in source
    for filesystem in NATIVE_WRITERS:
        assert f'filesystem == "{filesystem}"' not in source

def test_single_filesystem_hierarchy_and_c_first_writers() -> None:
    assert not (ROOT / "src" / "filesystems").exists()
    assert not (ROOT / "vendor").exists(), "bundled third-party source tree reintroduced"
    assert not (ROOT / "src" / "engine").exists()
    assert not (ROOT / "src" / "core" / "ld_plugin.h").exists()
    assert not (ROOT / "src" / "linux_defragger_engine.c").exists()
    assert not (GUI / "ext_engine.py").exists()
    assert not (GUI / "ntfs_engine.py").exists()
    assert not (GUI / "exfat_engine.py").exists()
    assert not (GUI / "xfs_engine.py").exists()

    required_native = {
        "ext4": {"ext_native.h", "ext_common.c", "ext_catalog.c", "ext_plan.c", "ext_worker.c"},
        "ntfs": {"ntfs_native.h", "ntfs_common.c", "ntfs_catalog.c", "ntfs_plan.c", "ntfs_plan_db.cpp", "ntfs_worker.c"},
        "exfat": {"exfat_native.h", "exfat_common.c", "exfat_plan.c", "exfat_worker.c"},
        "xfs": {"xfs_native.h", "xfs_common.c", "xfs_catalog.c", "xfs_plan.c", "xfs_metadata.c", "xfs_worker.c"},
        "affs": {"affs_native.h", "affs_native.c", "affs_worker.c"},
        "hfsplus": {"hfsplus_native.h", "hfsplus_native.c", "hfsplus_worker.c"},
        "minix": {"minix_native.h", "minix_native.c", "minix_worker.c"},
        "swap": {"swap_native.h", "swap_native.c", "swap_worker.c"},
        "ufs": {"ufs_native.h", "ufs_native.c", "ufs_worker.c"},
        "zfs": {"zfs_native.h", "zfs_native.c", "zfs_worker.c"},
        "sfs": {"sfs_native.h", "sfs_native.c", "sfs_worker.c"},
        "pfs3": {"pfs3_native.h", "pfs3_native.c", "pfs3_worker.c"},
        "hfs": {"analyser.c", "writer.c"},
    }
    for filesystem, native_files in required_native.items():
        package = GUI / "filesystems" / filesystem
        native = package / "native"
        assert native.is_dir()
        assert native_files <= {path.name for path in native.iterdir() if path.is_file()}
        assert not list(package.glob("*.py")), (
            f"{filesystem} retained a duplicate Python filesystem declaration"
        )

    for obsolete_width_package in ("fat12", "fat16", "fat32"):
        assert not (GUI / "filesystems" / obsolete_width_package).exists()
    assert not (GUI / "filesystems" / "__init__.py").exists()

    ntfs_native = GUI / "filesystems" / "ntfs" / "native"
    ntfs_plan = (ntfs_native / "ntfs_plan.c").read_text()
    ntfs_plan_db = (ntfs_native / "ntfs_plan_db.cpp").read_text()
    ntfs_planning = ntfs_plan + ntfs_plan_db
    assert "fixed_primary" in ntfs_planning
    assert "growth&&!catalogue.growth_10_satisfied" not in ntfs_planning.replace(" ", "")
    assert "catalogue.growth_10_satisfied" not in ntfs_planning
    assert "class SqliteStatement" in ntfs_plan_db
    assert "class RollbackGuard" in ntfs_plan_db
    assert 'extern "C" int ntfs_create_plan_db' in ntfs_plan_db
    assert "virtual " not in ntfs_plan_db
    cpp_sources = sorted((GUI / "filesystems").rglob("*.cpp"))
    assert cpp_sources == [ntfs_native / "ntfs_plan_db.cpp"]

    fat_native = GUI / "filesystems" / "fat" / "native"
    assert (fat_native / "writer.c").is_file()
    hfs_analyser = GUI / "filesystems" / "hfs" / "native" / "analyser.c"
    assert hfs_analyser.is_file()
    hfs_source = hfs_analyser.read_text()
    assert "libhfs" not in hfs_source
    assert "vendor/" not in _cmake_source()


def test_build_and_path_registry_install_native_workers() -> None:
    cmake = _cmake_source()
    paths = (GUI / "core" / "paths.py").read_text()
    for worker, filesystem in (
        ("linux-defragger-ext-worker", "ext4"),
        ("linux-defragger-ntfs-worker", "ntfs"),
        ("linux-defragger-exfat-worker", "exfat"),
        ("linux-defragger-xfs-worker", "xfs"),
        ("linux-defragger-fat-worker", "fat"),
        ("linux-defragger-affs-worker", "affs"),
        ("linux-defragger-hfsplus-worker", "hfsplus"),
        ("linux-defragger-minix-worker", "minix"),
        ("linux-defragger-swap-worker", "swap"),
        ("linux-defragger-ufs-worker", "ufs"),
        ("linux-defragger-zfs-worker", "zfs"),
        ("linux-defragger-sfs-worker", "sfs"),
        ("linux-defragger-pfs3-worker", "pfs3"),
    ):
        assert worker in cmake
        install_pattern = re.compile(
            rf"install\(TARGETS\s+{re.escape(worker)}\s+"
            rf"RUNTIME DESTINATION lib/linux-defragger/filesystems/{filesystem}\)"
        )
        assert install_pattern.search(cmake), f"{worker} has no package install rule"
        assert worker in paths
    assert "linux-defragger-hfs-worker" in cmake
    assert re.search(
        r"install\(TARGETS\s+linux-defragger-hfs-worker\s+"
        r"RUNTIME DESTINATION lib/linux-defragger/filesystems/hfs\)",
        cmake,
    )
    assert "hfs_analyser" in paths
    for obsolete in ("ext-raw", "ntfs-raw", "exfat-raw"):
        assert obsolete not in paths


def test_infiltratr_common_integration() -> None:
    common = ROOT / "shared" / "infiltratr-common"
    assert (common / "VERSION").read_text().strip() == "1.19.10"
    gitmodules = (ROOT / ".gitmodules").read_text()
    assert "shared/infiltratr-common" in gitmodules
    assert "Infiltrator-Libraries.git" in gitmodules
    cmake = _cmake_source()
    assert "33e69c0a462b56d388881d89c4eb49f72fa0b0fe" in cmake
    assert "add_subdirectory(" in cmake
    assert "InfiltratrCommon::Common" in cmake
    assert "set(INFILTRATR_COMMON_BUILD_TESTS OFF)" in cmake
    assert "set(INFILTRATR_COMMON_BUILD_SHARED OFF)" in cmake
    assert '${INFILTRATR_COMMON_DIR}/src/core.c' not in cmake
    assert '${INFILTRATR_COMMON_DIR}/src/posix.c' not in cmake
    local_installer = (ROOT / "packaging" / "build-local-run.sh").read_text()
    typography_vendor = (ROOT / "packaging" / "vendor-mb-fonts.cmake").read_text()
    assert "InfiltratrTypographyAssets.cmake" in typography_vendor
    assert "Verified MB Corpo archive was not materialised" in typography_vendor
    assert 'COMMON_VERSION="1.19.10"' in local_installer
    assert 'COMMON_COMMIT="33e69c0a462b56d388881d89c4eb49f72fa0b0fe"' in local_installer
    device = (ROOT / "src" / "core" / "ld_device.c").read_text()
    assert "infiltratr_realpath_copy" in device
    assert "infiltratr_read_u64_file" in device
    assert "infiltratr_string_starts_with" in device
    assert "infiltratr_path_basename" in device
    fat = (GUI / "filesystems" / "fat" / "native" / "writer.c").read_text()
    assert "infiltratr_parse_u64_range" in fat
    assert "infiltratr_parse_binary_quantity_u64" in fat
    assert "strtoull(" not in fat
    assert "infiltratr_path_basename" in fat
    assert "infiltratr_size_add_checked" in fat
    assert "while (new_cap" not in fat
    fat_journal = (GUI / "filesystems" / "fat" / "native" / "fat_journal.c").read_text()
    assert "infiltratr_parse_u64" in fat_journal
    assert "infiltratr_parse_u64_range" in fat_journal
    assert "infiltratr_config_parse_line" in fat_journal
    production_c = "\n".join(
        path.read_text(encoding="utf-8", errors="replace")
        for path in [*Path(ROOT / "src").rglob("*.c"),
                     *Path(GUI / "filesystems").rglob("*.c")]
    )
    assert "infiltratr_u64_add_checked" in production_c
    assert "infiltratr_u64_add_saturating" in production_c
    assert "infiltratr_array_reserve" in production_c
    assert "infiltratr_atomic_file_write" in production_c
    assert "infiltratr_unlink_durable" in production_c
    assert "ld_u64_add" not in production_c
    io = (ROOT / "src" / "core" / "ld_io.c").read_text()
    assert "infiltratr_pread_full" in io
    assert "infiltratr_pwrite_full" in io
    assert re.search(r"(?<![A-Za-z0-9_])pread\s*\(", production_c) is None
    assert re.search(r"(?<![A-Za-z0-9_])pwrite\s*\(", production_c) is None
    endian_consumers = (
        GUI / "filesystems" / "affs" / "native" / "affs_native.c",
        GUI / "filesystems" / "btrfs" / "native" / "btrfs_native.c",
        GUI / "filesystems" / "exfat" / "native" / "exfat_common.c",
        GUI / "filesystems" / "hfs" / "native" / "analyser.c",
        GUI / "filesystems" / "hfsplus" / "native" / "hfsplus_native.c",
        GUI / "filesystems" / "sfs" / "native" / "sfs_native.c",
        GUI / "filesystems" / "swap" / "native" / "swap_native.c",
        GUI / "filesystems" / "xfs" / "native" / "xfs_common.c",
    )
    for path in endian_consumers:
        assert "infiltratr_load_" in path.read_text(), (
            f"{path.relative_to(ROOT)} bypasses Common endian access"
        )
        source = path.read_text()
        for wrapper in ("static uint16_t be16", "static uint32_t be32",
                        "static uint64_t be64", "static uint16_t le16",
                        "static uint32_t le32", "static uint64_t le64"):
            assert wrapper not in source, (
                f"{path.relative_to(ROOT)} retains a pass-through endian wrapper"
            )
    for filesystem, worker in (("ext4", "ext_worker.c"), ("ntfs", "ntfs_worker.c"),
                               ("exfat", "exfat_worker.c"), ("xfs", "xfs_worker.c")):
        source = (GUI / "filesystems" / filesystem / "native" / worker).read_text()
        assert "infiltratr_parse_u64" in source
        assert "infiltratr_trim_line_end" in source
    exfat = (GUI / "filesystems" / "exfat" / "native" / "exfat_worker.c").read_text()
    assert "infiltratr_parse_binary_quantity_u64" in exfat
    assert "strtoull(" not in exfat
    exfat_common = (GUI / "filesystems" / "exfat" / "native" / "exfat_common.c").read_text()
    assert "infiltratr_load_le16" in exfat_common
    assert "infiltratr_load_le32" in exfat_common
    assert "infiltratr_load_le64" in exfat_common
    for wrapper in ("static uint16_t read_le16", "static uint32_t read_le32",
                    "static uint64_t read_le64"):
        assert wrapper not in exfat_common
    ext_common = (GUI / "filesystems" / "ext4" / "native" / "ext_common.c").read_text()
    ext_catalog = (GUI / "filesystems" / "ext4" / "native" / "ext_catalog.c").read_text()
    assert "infiltratr_array_reserve" in ext_common
    assert "infiltratr_array_reserve" in ext_catalog
    for path in (
        GUI / "filesystems" / "fat" / "native" / "fat_io.c",
        GUI / "filesystems" / "minix" / "native" / "minix_native.c",
        GUI / "filesystems" / "minix" / "native" / "minix_worker.c",
        GUI / "filesystems" / "sfs" / "native" / "sfs_worker.c",
        GUI / "filesystems" / "ufs" / "native" / "ufs_worker.c",
    ):
        assert "infiltratr_size_multiply_checked" in path.read_text()
    for filesystem, worker in (("ext4", "ext_worker.c"), ("ntfs", "ntfs_worker.c"),
                               ("exfat", "exfat_worker.c"), ("xfs", "xfs_worker.c")):
        source = (GUI / "filesystems" / filesystem / "native" / worker).read_text()
        assert "atoi(" not in source
    sfs_native = (GUI / "filesystems" / "sfs" / "native" / "sfs_native.c").read_text()
    assert "infiltratr_array_reserve" in sfs_native
    assert "realloc(" not in sfs_native
    ext_plan = (GUI / "filesystems" / "ext4" / "native" / "ext_plan.c").read_text()
    assert "infiltratr_array_reserve" in ext_plan
    assert "ld_xrealloc(" not in ext_plan
    runtime_header = (ROOT / "src" / "core" / "ld_runtime.h").read_text()
    runtime_source = (ROOT / "src" / "core" / "ld_runtime.c").read_text()
    for retired in ("ld_read_le16", "ld_read_le32", "ld_write_le16", "ld_write_le32"):
        assert retired not in runtime_header
        assert retired not in production_c
    assert "ld_xrealloc" not in runtime_header
    assert "ld_xrealloc" not in runtime_source
    assert "infiltratr_size_add_checked" in runtime_source

    native_json = (ROOT / "native" / "json.cpp").read_text()
    assert "infiltratr_parse_double" in native_json
    assert "std::strtod" not in native_json

    test_media_theme = (ROOT / "test_media" / "test_media_theme.c").read_text()
    assert "infiltratr_typography()" in test_media_theme
    assert "infiltratr_design_metrics()" in test_media_theme
    assert "infiltratr_theme_resolve" in test_media_theme
    for role in (
        "titlebar_rgb",
        "connection_rgb",
        "connection_border_rgb",
        "heading_rgb",
        "summary_rgb",
        "detail_label_rgb",
        "note_rgb",
        "status_border_rgb",
        "accent_hover_rgb",
    ):
        assert f"palette->{role}" in test_media_theme, (
            f"Test Media does not consume Common 1.19.10 role {role}"
        )

    protocol = (ROOT / "src" / "core" / "ld_protocol.c").read_text()
    journal_config_consumers = (
        GUI / "filesystems" / "fat" / "native" / "fat_journal.c",
        GUI / "filesystems" / "affs" / "native" / "affs_worker.c",
        GUI / "filesystems" / "sfs" / "native" / "sfs_worker.c",
        GUI / "filesystems" / "xfs" / "native" / "xfs_worker.c",
        GUI / "filesystems" / "ext4" / "native" / "ext_worker.c",
        GUI / "filesystems" / "exfat" / "native" / "exfat_worker.c",
        GUI / "filesystems" / "ntfs" / "native" / "ntfs_worker.c",
        GUI / "filesystems" / "hfsplus" / "native" / "hfsplus_worker.c",
        GUI / "filesystems" / "exfat" / "native" / "exfat_relayout.c",
    )
    for path in journal_config_consumers:
        source = path.read_text()
        assert "infiltratr_config_parse_line" in source, (
            f"{path.relative_to(ROOT)} duplicates Common key=value parsing"
        )
        assert "strchr(line, '=')" not in source, (
            f"{path.relative_to(ROOT)} retained a private key=value splitter"
        )

    exfat_common = (GUI / "filesystems" / "exfat" / "native" / "exfat_common.c").read_text()
    assert "infiltratr_path_join" in exfat_common
    assert "join_path_alloc" in exfat_common

    assert "infiltratr_escape_json" in protocol
    result_workers = (
        GUI / "filesystems" / "fat" / "native" / "writer.c",
        GUI / "filesystems" / "ext4" / "native" / "ext_worker.c",
        GUI / "filesystems" / "ntfs" / "native" / "ntfs_worker.c",
        GUI / "filesystems" / "exfat" / "native" / "exfat_worker.c",
        GUI / "filesystems" / "xfs" / "native" / "xfs_worker.c",
        GUI / "filesystems" / "affs" / "native" / "affs_worker.c",
        GUI / "filesystems" / "sfs" / "native" / "sfs_worker.c",
        GUI / "filesystems" / "hfsplus" / "native" / "hfsplus_worker.c",
    )
    for path in result_workers:
        source = path.read_text()
        assert "ld_emit_result_event" in source
        assert r'@@RESULT {\"operation' not in source

    for path in (
        GUI / "filesystems" / "btrfs" / "native" / "btrfs_worker.c",
        GUI / "filesystems" / "ntfs" / "native" / "ntfs_worker.c",
        GUI / "filesystems" / "exfat" / "native" / "exfat_worker.c",
        GUI / "filesystems" / "xfs" / "native" / "xfs_catalog.c",
    ):
        assert "infiltratr_percent_u64" in path.read_text()

    for path in (
        GUI / "filesystems" / "ntfs" / "native" / "ntfs_common.c",
        GUI / "filesystems" / "ntfs" / "native" / "ntfs_catalog.c",
        GUI / "filesystems" / "swap" / "native" / "swap_native.c",
        GUI / "filesystems" / "ufs" / "native" / "ufs_native.c",
    ):
        assert "infiltratr_u64_multiply_checked" in path.read_text()

    test_media_worker = (ROOT / "test_media" / "test_media_worker.c").read_text()
    test_media_gui = (ROOT / "test_media" / "test_media_gui.c").read_text()
    test_media_amiga = (ROOT / "test_media" / "test_media_amiga_payload.c").read_text()
    assert "strtoull(" not in test_media_worker
    assert "strtoul(" not in test_media_worker
    assert "atoi(" not in test_media_worker
    assert "strtoull(" not in test_media_gui
    assert "strtoul(" not in test_media_gui
    assert "atoi(" not in test_media_gui
    assert "ldtm_decode_hex_byte" in test_media_worker
    assert "ldtm_decode_hex_byte" in test_media_gui
    assert "infiltratr_array_reserve" in test_media_worker
    assert "infiltratr_path_basename" in test_media_worker
    assert "infiltratr_string_starts_with" in test_media_worker
    assert "infiltratr_path_join" in test_media_worker
    assert "static int join_path(" not in test_media_worker
    test_media_cmake = (ROOT / "cmake" / "test_media.cmake").read_text()
    assert "InfiltratrCommon::Common" in test_media_cmake
    assert "infiltratr_array_reserve" in test_media_amiga
    assert "infiltratr_load_be32" in test_media_amiga
    assert "infiltratr_store_be32" in test_media_amiga
    assert "infiltratr_pread_full" in test_media_amiga
    assert "infiltratr_pwrite_full" in test_media_amiga
    test_media_amiga_source = (ROOT / "test_media" / "test_media_amiga.c").read_text()
    test_media_sfs_source = (ROOT / "test_media" / "test_media_sfs.c").read_text()
    test_media_dispatch_source = (ROOT / "test_media" / "test_media_amiga_dispatch.c").read_text()
    for source in (test_media_amiga_source, test_media_amiga,
                   test_media_sfs_source, test_media_dispatch_source):
        assert re.search(r"(?<![A-Za-z0-9_])pread\s*\(", source) is None
        assert re.search(r"(?<![A-Za-z0-9_])pwrite\s*\(", source) is None
    ext_worker = (GUI / "filesystems" / "ext4" / "native" / "ext_worker.c").read_text()
    ntfs_worker = (GUI / "filesystems" / "ntfs" / "native" / "ntfs_worker.c").read_text()
    assert "infiltratr_string_starts_with" in ext_worker
    assert "infiltratr_string_starts_with" in ntfs_worker
    path_source = (ROOT / "src" / "core" / "ld_path.c").read_text()
    path_header = (ROOT / "src" / "core" / "ld_path.h").read_text()
    assert "SYS_openat2" in path_source
    assert "RESOLVE_BENEATH" in path_source
    assert "RESOLVE_NO_SYMLINKS" in path_source
    assert "RESOLVE_NO_MAGICLINKS" in path_source
    assert "O_NOFOLLOW" in path_source
    assert "infiltratr_size_add_checked" in path_source
    assert "infiltratr_path_concat" in path_source
    assert "ld_path_open_atomic_temp" not in path_source + path_header
    assert "ld_path_fsync_parent" not in path_source + path_header

    # The old Python transaction/journal layer had no production consumer and
    # duplicated Common's native durable-file contract. Keep it removed rather
    # than maintaining a second persistence implementation beside the writers.
    assert not (GUI / "core" / "journal.py").exists()
    assert not (GUI / "core" / "transaction.py").exists()
    assert not (ROOT / "tests" / "test_transactions.py").exists()

    # The filesystem-side Python compatibility graph has been removed entirely.
    # Python is now presentation/glue only; raw filesystem I/O remains native.
    assert not (GUI / "engine").exists()
    assert not (GUI / "backends").exists()
    assert not list((GUI / "filesystems").rglob("*.py"))

    for filesystem, worker in (("affs", "affs_worker.c"), ("apfs", "apfs_worker.c"), ("btrfs", "btrfs_worker.c"), ("sfs", "sfs_worker.c"),
                               ("pfs3", "pfs3_worker.c"), ("hfs", "writer.c"),
                               ("hfsplus", "hfsplus_worker.c"), ("minix", "minix_worker.c")):
        source = (GUI / "filesystems" / filesystem / "native" / worker).read_text()
        assert "infiltratr_parse_u64_range" in source
        assert "infiltratr_trim_line_end" in source


def test_core_remains_filesystem_neutral() -> None:
    core = ROOT / "src" / "core"
    expected = {"ld_device.c", "ld_device.h", "ld_io.c", "ld_io.h", "ld_runtime.c",
                "ld_runtime.h", "ld_path.c", "ld_path.h", "ld_protocol.c",
                "ld_protocol.h", "ld_stop.c", "ld_stop.h"}
    assert expected <= {path.name for path in core.iterdir() if path.is_file()}
    combined = "\n".join(
        path.read_text(encoding="utf-8", errors="replace").lower()
        for path in core.glob("*.[ch]")
    )
    for filesystem in ("xfs", "ntfs", "exfat", "ext4", "btrfs", "hfs", "hfsplus", "pfs3"):
        assert f"{filesystem}_" not in combined


def test_production_write_safety_is_enforced_at_every_boundary() -> None:
    runtime_header = (ROOT / "src" / "core" / "ld_runtime.h").read_text()
    runtime_source = (ROOT / "src" / "core" / "ld_runtime.c").read_text()
    engine = (ROOT / "native" / "operation_engine.cpp").read_text()
    helper_policy = (ROOT / "native" / "helper_policy.cpp").read_text()
    planner = (GUI / "ui" / "operation_planner.py").read_text()
    combined_policy = "\n".join(
        (runtime_header, runtime_source, engine, helper_policy, planner)
    )
    assert "UNAUDITED_RAW_WRITES" not in combined_policy
    assert "ld_path_is_mounted(device.c_str())" in engine
    assert "validate_operation_args" in helper_policy
    assert "operation journal must be directly below" in helper_policy
    assert "if volume.mounted:" in planner

    native = GUI / "filesystems"
    workers = {
        "fat": native / "fat" / "native" / "writer.c",
        "ext": native / "ext4" / "native" / "ext_worker.c",
        "ntfs": native / "ntfs" / "native" / "ntfs_worker.c",
        "exfat": native / "exfat" / "native" / "exfat_worker.c",
        "xfs": native / "xfs" / "native" / "xfs_worker.c",
        "affs": native / "affs" / "native" / "affs_worker.c",
        "apfs": native / "apfs" / "native" / "apfs_worker.c",
        "btrfs": native / "btrfs" / "native" / "btrfs_worker.c",
        "sfs": native / "sfs" / "native" / "sfs_worker.c",
        "pfs3": native / "pfs3" / "native" / "pfs3_worker.c",
        "hfs": native / "hfs" / "native" / "writer.c",
        "hfsplus": native / "hfsplus" / "native" / "hfsplus_worker.c",
        "minix": native / "minix" / "native" / "minix_worker.c",
    }
    sources = {name: path.read_text() for name, path in workers.items()}
    for name, source in sources.items():
        assert "--write" in source and "--confirm" in source, (
            f"{name} lost explicit mutation confirmation"
        )
        assert "ld_runtime_require_write_audit_override" not in source

    device_source = (ROOT / "src" / "core" / "ld_device.c").read_text()
    for required in (
        "realpath(path, NULL)",
        "O_NOFOLLOW",
        "fstat(fd, &opened)",
        "errno = ESTALE",
        "errno = EBUSY",
    ):
        assert required in device_source, f"raw target open lost hardening: {required}"

    for path in (
        native / "ntfs" / "native" / "ntfs_common.c",
        native / "exfat" / "native" / "exfat_common.c",
    ):
        opener = path.read_text()
        assert "O_NOFOLLOW" in opener
        assert "fstat(fd" in opener
        assert "identity changed between validation and open" in opener

    assert "ld_device_open(device_path, mutating)" in sources["fat"]
    assert sources["ext"].count("ld_path_is_mounted(device)") >= 2
    assert sources["ntfs"].count("ld_path_is_mounted(device)") >= 2
    assert "ld_path_is_mounted(device)" in sources["exfat"]
    assert "ld_device_number_is_mounted(status.st_rdev)" in sources["xfs"]
    assert "ld_path_is_mounted(device)" in sources["affs"]
    assert "ld_path_is_mounted(device)" in sources["apfs"]
    assert "ld_path_is_mounted(device)" in sources["btrfs"]
    assert "ld_path_is_mounted(device)" in sources["sfs"]
    assert "ld_path_is_mounted(device)" in sources["pfs3"]
    assert "ld_path_is_mounted(device)" in sources["hfs"]
    assert "ld_path_is_mounted(device)" in sources["hfsplus"]
    assert "ld_path_is_mounted(device)" in sources["minix"]

    recovery_bindings = {
        "ext": (".ext-stage.img", ".ext-plan.sqlite"),
        "ntfs": (".ntfs-stage.img", ".ntfs-plan.sqlite"),
        "exfat": (".exfat-stage.img",),
        "xfs": (".xfs-stage.img", ".xfs-plan.sqlite"),
        "affs": (".affs-stage",),
        "apfs": (".apfs-stage",),
        "btrfs": (".btrfs-stage",),
        "sfs": (".sfs-stage",),
        "pfs3": (".pfs3-stage",),
        "hfs": (".hfs-stage",),
        "hfsplus": (".hfsplus-stage",),
        "minix": (".minix-stage",),
    }
    for name, suffixes in recovery_bindings.items():
        assert "ld_path_is_derived_from" in sources[name]
        for suffix in suffixes:
            assert suffix in sources[name], f"{name} lost {suffix} recovery binding"

    journal_sources = {
        "fat": native / "fat" / "native" / "fat_journal.c",
        "ext": workers["ext"],
        "ntfs": workers["ntfs"],
        "exfat": workers["exfat"],
        "xfs": workers["xfs"],
        "affs": workers["affs"],
        "apfs": workers["apfs"],
        "btrfs": workers["btrfs"],
        "sfs": workers["sfs"],
        "pfs3": workers["pfs3"],
        "hfs": workers["hfs"],
        "hfsplus": workers["hfsplus"],
        "minix": workers["minix"],
    }
    for name, path in journal_sources.items():
        assert "infiltratr_atomic_file_write" in path.read_text(), (
            f"{name} journal lost Common durable atomic-file handling"
        )
    assert "infiltratr_atomic_file_write" in (
        native / "exfat" / "native" / "exfat_relayout.c"
    ).read_text()

    for path in (
        native / "ext4" / "native" / "ext_catalog.c",
        native / "ntfs" / "native" / "ntfs_plan_db.cpp",
        native / "xfs" / "native" / "xfs_plan.c",
    ):
        assert "SQLITE_OPEN_NOFOLLOW" in path.read_text(), (
            f"{path.relative_to(ROOT)} lost SQLite symlink refusal"
        )

    # Persistent stages can be created by a root worker below a user-owned state
    # directory. They must never follow or truncate a pre-created symlink.
    for path in (
        native / "exfat" / "native" / "exfat_plan.c",
        native / "affs" / "native" / "affs_native.c",
        native / "apfs" / "native" / "apfs_native.c",
        native / "btrfs" / "native" / "btrfs_native.c",
        native / "sfs" / "native" / "sfs_native.c",
        native / "pfs3" / "native" / "pfs3_native.c",
        native / "hfs" / "native" / "writer.c",
        native / "hfsplus" / "native" / "hfsplus_native.c",
        native / "minix" / "native" / "minix_native.c",
    ):
        stage_source = path.read_text()
        assert "O_EXCL" in stage_source, (
            f"{path.relative_to(ROOT)} lost exclusive stage creation"
        )
        assert "O_NOFOLLOW" in stage_source, (
            f"{path.relative_to(ROOT)} lost stage symlink refusal"
        )
        assert "O_TRUNC" not in stage_source, (
            f"{path.relative_to(ROOT)} can truncate a pre-existing stage target"
        )

    assert "an unfinished NTFS journal exists; run Recover first" in sources["ntfs"]

    path_source = (ROOT / "src" / "core" / "ld_path.c").read_text()
    for required in ("openat(", "mkdirat(", "O_NOFOLLOW", "fstat("):
        assert required in path_source, (
            f"trusted journal path walker lost {required}"
        )

    support_source = (GUI / "ui" / "support.py").read_text()
    helper_policy = (ROOT / "native" / "helper_policy.cpp").read_text()
    assert "/var/lib/linux-defragger/state" in support_source
    assert "XDG_STATE_HOME" not in support_source
    assert "validate_operation_args" in helper_policy
    assert "operation journal must be directly below" in helper_policy

    fat_journal = native / "fat" / "native" / "fat_journal.c"
    assert "ld_path_ensure_trusted_directory_tree" in fat_journal.read_text()

    journal_workers = (
        workers["ext"], workers["ntfs"], workers["exfat"], workers["xfs"],
        workers["affs"], workers["apfs"], workers["btrfs"], workers["sfs"], workers["pfs3"], workers["hfs"], workers["hfsplus"], workers["minix"],
        native / "exfat" / "native" / "exfat_relayout.c",
    )
    for path in journal_workers:
        source = path.read_text()
        assert "ld_path_ensure_trusted_directory_tree" in source, (
            f"{path.relative_to(ROOT)} bypasses trusted journal-parent traversal"
        )
        assert "mkdir(copy, 0700)" not in source, (
            f"{path.relative_to(ROOT)} reintroduced pathname-based parent traversal"
        )

    device_source = (ROOT / "src" / "core" / "ld_device.c").read_text()
    for required in (
        "ld_device_try_open",
        "ld_device_format_identity",
        "ld_device_matches_identity",
        "ld_fd_matches_identity",
        "ld_device_open_verified_fd",
        "ld_device_capture_binding",
    ):
        assert required in device_source, (
            f"raw target identity core lost {required}"
        )

    for name in ("ext", "ntfs", "xfs"):
        assert "ld_device_format_identity" in sources[name], (
            f"{name} bypasses shared target identity formatting"
        )
    for name in ("exfat", "affs", "apfs", "btrfs", "ufs", "sfs", "pfs3", "hfs", "hfsplus", "minix"):
        assert "ld_device_capture_binding" in sources[name], (
            f"{name} bypasses one-open shared transaction binding"
        )

    for path in (
        workers["ext"], workers["ntfs"], workers["affs"], workers["apfs"], workers["btrfs"],
        workers["sfs"], workers["pfs3"], workers["hfs"], workers["hfsplus"], workers["minix"],
        native / "ntfs" / "native" / "ntfs_plan.c",
        native / "exfat" / "native" / "exfat_relayout.c",
    ):
        source = path.read_text()
        assert (
            "ld_device_open_verified_fd" in source
            or "ld_device_matches_identity" in source
        ), f"{path.relative_to(ROOT)} bypasses journal-bound target opening"

    xfs_source = workers["xfs"].read_text()
    assert "ld_device_try_open" in xfs_source
    assert "ld_device_matches_identity" in xfs_source
    assert xfs_source.index("xfs_rebuild_allocation_metadata") < xfs_source.index(
        "xfs_permute_payloads"
    ), "XFS metadata capacity must be proven before payload staging"

    ext_plan = (native / "ext4" / "native" / "ext_plan.c").read_text()
    assert "ext_apply_mappings_under_lock" in ext_plan
    assert "ext_apply_mappings_under_lock(device" in sources["ext"]
    assert sources["ext"].count(
        "(void)flock(fd, LOCK_UN); close(fd); fd = -1;\n"
        "        if (validate_restored_ext(device"
    ) >= 2, "EXT rollback validation must run after releasing its raw lock"

    hfsplus_native = (
        native / "hfsplus" / "native" / "hfsplus_native.c"
    ).read_text()
    assert "ld_fd_size_bytes(volume->fd, &volume->bytes)" in hfsplus_native

    ntfs_plan = (native / "ntfs" / "native" / "ntfs_plan.c").read_text()
    ntfs_worker = workers["ntfs"].read_text()
    assert "ld_fd_matches_identity" in ntfs_plan
    assert "ld_fd_matches_identity" in ntfs_worker
    assert "open(device, O_RDWR | O_CLOEXEC)" not in ntfs_plan
    assert "open(device, O_RDWR | O_CLOEXEC)" not in ntfs_worker

    exfat_worker = workers["exfat"].read_text()
    exfat_relayout = (native / "exfat" / "native" / "exfat_relayout.c").read_text()
    assert "ld_fd_matches_identity" in exfat_worker
    assert "target_identity" in exfat_relayout
    assert "device_size" in exfat_relayout
    assert "ld_fd_format_identity" in exfat_relayout
    assert "ld_fd_matches_identity" in exfat_relayout

    for path in (
        workers["ext"], workers["ntfs"], workers["affs"], workers["apfs"], workers["btrfs"],
        workers["sfs"], workers["pfs3"], workers["hfs"], workers["hfsplus"], workers["minix"],
    ):
        source = path.read_text()
        assert "open(device, O_RDWR | O_CLOEXEC)" not in source
        assert "open(target_path, O_RDWR | O_CLOEXEC)" not in source


def test_version_and_native_registry_ownership() -> None:
    assert re.fullmatch(r"\d+\.\d+\.\d+-\d+", (ROOT / "VERSION").read_text().strip())
    runtime = (ROOT / "native" / "runtime.cpp").read_text()
    assert "const std::vector<BackendInfo>& backend_registry()" in runtime
    assert "registry_manifest_json" in runtime
    assert not (GUI / "backends").exists()
    assert not (GUI / "engine").exists()
    for obsolete in ("allocation_mapper.py", "operation_engine.py", "privileged_helper.py"):
        assert not (GUI / obsolete).exists()
    launcher_lines = (GUI / "linux_defragger_gui.py").read_text().splitlines()
    assert len(launcher_lines) < 20
    for runtime_module in (
        GUI / "ui" / "window.py",
        GUI / "ui" / "operation_planner.py",
        GUI / "ui" / "map_presenter.py",
        GUI / "ui" / "devices.py",
    ):
        source = runtime_module.read_text()
        assert "from backends." not in source
        assert "import backends." not in source

def test_user_facing_branding_is_defragmenter() -> None:
    repository_identifier = "Infiltrator-Projects/Defragmenter"
    user_facing = (
        ROOT.parent / "README.md",
        ROOT / "README.md",
        REPO_ROOT / "docs" / "DESIGN.md",
        REPO_ROOT / "docs" / "AUDIT_STATUS.md",
        ROOT / "native" / "mapper.cpp",
        ROOT / "native" / "operation_engine.cpp",
        ROOT / "native" / "privileged_helper.cpp",
        GUI / "ui" / "window.py",
        GUI / "ui" / "window_view.py",
        GUI / "ui" / "operation_planner.py",
    )
    for candidate in user_facing:
        source = candidate.read_text().replace(repository_identifier, "")
        assert "Linux Defragger" not in source, (
            f"{candidate.relative_to(ROOT.parent)} retained the retired product name"
        )
        assert re.search(r"\bDefragger\b", source) is None, (
            f"{candidate.relative_to(ROOT.parent)} retained standalone Defragger branding"
        )

    desktop = (ROOT / "packaging" / "io.github.linuxdefragger.desktop").read_text()
    assert "Name=Defragmenter" in desktop
    assert "Icon=io.github.linuxdefragger" in desktop
    assert "StartupWMClass=io.github.linuxdefragger" in desktop

    project_cmake = (ROOT / "cmake" / "project.cmake").read_text()
    install_programs = project_cmake.split("install(PROGRAMS", 1)[1].split(
        "DESTINATION lib/linux-defragger", 1
    )[0]
    assert "install(DIRECTORY gui/core gui/ui" in project_cmake
    assert "gui/engine" not in project_cmake.split(
        "install(DIRECTORY gui/core gui/ui", 1
    )[1].split("DESTINATION lib/linux-defragger", 1)[0]
    assert "gui/filesystems" not in project_cmake.split(
        "install(DIRECTORY gui/core gui/ui", 1
    )[1].split("DESTINATION lib/linux-defragger", 1)[0]
    assert "gui/backends/" not in project_cmake.split(
        "install(DIRECTORY gui/core gui/ui", 1
    )[1].split("install(PROGRAMS packaging/linux-defragger", 1)[0]
    for legacy_dispatcher in (
        "gui/allocation_mapper.py",
        "gui/privileged_helper.py",
        "gui/operation_engine.py",
    ):
        assert legacy_dispatcher not in install_programs, (
            f"legacy Python control-plane dispatcher is still installed: {legacy_dispatcher}"
        )
    assert "gui/linux_defragger_gui.py" in install_programs
    assert "packaging/io.github.linuxdefragger.png" in project_cmake
    assert "share/icons/hicolor/96x96/apps" in project_cmake
    assert "share/icons/hicolor/128x128/apps" not in project_cmake
    assert "share/icons/hicolor/256x256/apps" not in project_cmake
    assert "DESTINATION share/app-install/icons" in project_cmake
    assert "RENAME infiltrator-defragmenter.png" in project_cmake
    assert "DESTINATION lib/linux-defragger" in project_cmake
    assert "RENAME defragmenter-icon.png" in project_cmake
    assert "packaging/io.github.linuxdefragger.svg" not in project_cmake
    icon_path = ROOT / "packaging" / "io.github.linuxdefragger.png"
    assert icon_path.is_file()
    assert subprocess.check_output(
        ["git", "hash-object", str(icon_path)],
        cwd=ROOT.parent,
        text=True,
    ).strip() == "c4d352ff04d5438085fcbe71b4bfb795c26b7755"
    png = icon_path.read_bytes()
    assert png[:8] == bytes((0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A))
    assert int.from_bytes(png[16:20], "big") == 96
    assert int.from_bytes(png[20:24], "big") == 96

    # Validate every PNG chunk boundary and CRC. The previous 256px asset
    # carried a plausible signature/IHDR but declared an IDAT larger than EOF,
    # so GdkPixbuf correctly rejected it at runtime.
    import zlib
    cursor = 8
    saw_iend = False
    while cursor < len(png):
        assert cursor + 12 <= len(png), "truncated PNG chunk header"
        length = int.from_bytes(png[cursor:cursor + 4], "big")
        chunk_type = png[cursor + 4:cursor + 8]
        data_start = cursor + 8
        data_end = data_start + length
        crc_end = data_end + 4
        assert crc_end <= len(png), (
            f"truncated PNG chunk {chunk_type!r}: declared {length} bytes"
        )
        expected_crc = int.from_bytes(png[data_end:crc_end], "big")
        actual_crc = zlib.crc32(chunk_type)
        actual_crc = zlib.crc32(png[data_start:data_end], actual_crc) & 0xFFFFFFFF
        assert actual_crc == expected_crc, f"bad PNG CRC for {chunk_type!r}"
        cursor = crc_end
        if chunk_type == b"IEND":
            saw_iend = True
            break
    assert saw_iend, "PNG has no IEND chunk"
    assert cursor == len(png), "bytes follow PNG IEND chunk"
    assert not (ROOT / "packaging" / "io.github.linuxdefragger.svg").exists()

    release_workflow = (ROOT.parent / ".github" / "workflows" / "release.yml").read_text()
    assert not (ROOT / "packaging" / "build-source-zip.sh").exists()
    assert "Defragmenter-${VERSION}.zip" not in release_workflow
    assert "Defragmenter-${VERSION}-amd64.deb" in release_workflow
    assert "Defragmenter-${VERSION}-local-folder.run" in release_workflow
    assert "linux-defragger_${VERSION}_amd64.deb" not in release_workflow
    assert "linux-defragger-${VERSION}-local-folder.run" not in release_workflow
    assert "Defragger-${VERSION}.zip" not in release_workflow

    test_media_user_facing = (
        ROOT / "test_media" / "test_media_main.c",
        ROOT / "test_media" / "test_media_gui.c",
        ROOT / "test_media" / "test_media_core.c",
        ROOT / "test_media" / "test_media_worker.c",
        ROOT / "test_media" / "test_media_amiga_payload.c",
    )
    for candidate in test_media_user_facing:
        source = candidate.read_text()
        assert "Linux Defragger" not in source, (
            f"{candidate.relative_to(ROOT.parent)} retained the retired product name"
        )
        assert "LinuxDefragger-TestData" not in source, (
            f"{candidate.relative_to(ROOT.parent)} retained the retired test-data label"
        )


def test_test_media_companion_is_all_c() -> None:
    source_dir = ROOT / "test_media"
    assert source_dir.is_dir()
    required = {
        "test_media.h",
        "test_media_core.c",
        "test_media_worker.c",
        "test_media_main.c",
        "test_media_gui.c",
    }
    assert required.issubset({path.name for path in source_dir.iterdir() if path.is_file()})
    assert not list(source_dir.glob("*.py"))
    assert not (ROOT / "tools" / "linux-defragger-media-harness.py").exists()
    assert not (ROOT / "tools" / "linux-defragger-testdata.py").exists()
    assert not (ROOT / "tests" / "test_media_harness.py").exists()

    cmake = _cmake_source()
    assert "add_executable(linux-defragger-test-media" in cmake
    assert "install(TARGETS linux-defragger-test-media RUNTIME DESTINATION bin)" in cmake
    assert "linux-defragger-media-harness" not in cmake
    assert "linux-defragger-testdata" not in cmake

    desktop = (ROOT / "packaging" / "io.github.linuxdefragger.TestMedia.desktop").read_text()
    assert "Exec=linux-defragger-test-media" in desktop

    window = (GUI / "ui" / "window.py").read_text()
    window_view = (GUI / "ui" / "window_view.py").read_text()
    install_script = (ROOT / "install.sh").read_text()
    assert "linux-defragger-testdata" not in window
    assert "Create fragmented test data" not in window_view
    assert "linux-defragger-testdata" not in install_script
    assert 'cmake --install "$BUILD" --prefix /usr' in install_script

    media_worker = (source_dir / "test_media_worker.c").read_text()
    assert '"crc=1,rmapbt=0,reflink=0"' in media_worker

    architecture_doc = (REPO_ROOT / "docs" / "ARCHITECTURE.md").read_text()
    deb_builder = (ROOT / "packaging" / "build-deb.sh").read_text()
    assert "per-filesystem native C analysers / planners / writers" in architecture_doc
    assert "Amiga OFS/FFS/SFS/PFS3" in deb_builder
    assert "UFS and ZFS" in deb_builder
    assert "install(FILES README.md" in cmake
    assert "docs/AUDIT_STATUS.md" not in cmake


def main() -> None:
    test_top_level_cmake_owns_native_language_declaration()
    test_native_registry_is_the_single_capability_authority()
    test_unqualified_ufs_mutation_is_fail_closed_in_the_installed_worker()
    test_dispatch_is_filesystem_neutral()
    test_single_filesystem_hierarchy_and_c_first_writers()
    test_build_and_path_registry_install_native_workers()
    test_infiltratr_common_integration()
    test_core_remains_filesystem_neutral()
    test_production_write_safety_is_enforced_at_every_boundary()
    test_test_media_companion_is_all_c()
    test_user_facing_branding_is_defragmenter()
    test_version_and_native_registry_ownership()
    print("current C-first native-registry architecture tests passed")


if __name__ == "__main__":
    main()
