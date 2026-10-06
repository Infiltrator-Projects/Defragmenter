#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""High-level architectural contracts for Defragmenter.

These tests intentionally protect dependency direction and ownership rather than
source formatting.  They must survive harmless refactors such as changing
container reservations, helper function names, CMake ordering, or splitting a
translation unit without changing the product architecture.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUI = ROOT / "gui"

# Release/audit scope.  This is a product contract, not an implementation-count
# assertion: adding a writer requires deliberately extending the audited scope.
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
    "ufs": "ufs-native",
}

READ_ONLY_BACKENDS = {"zfs", "swap"}


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _cmake_source() -> str:
    paths = [ROOT / "CMakeLists.txt", *sorted((ROOT / "cmake").glob("*.cmake"))]
    return "\n".join(_read(path) for path in paths if path.is_file())


def test_native_languages_are_declared_at_the_project_boundary() -> None:
    cmake = _read(ROOT / "CMakeLists.txt")
    declaration = re.search(r"project\s*\((?P<body>[^)]*)\)", cmake, re.S)
    assert declaration is not None
    body = declaration.group("body")
    languages = re.search(r"\bLANGUAGES\b(?P<items>.*)", body, re.S)
    assert languages is not None
    tokens = set(re.findall(r"[A-Za-z0-9_+.-]+", languages.group("items")))
    assert {"C", "CXX"} <= tokens


def test_runtime_registry_owns_backend_capabilities() -> None:
    runtime = _read(ROOT / "native" / "runtime.cpp")
    mapper = _read(ROOT / "native" / "mapper.cpp")
    operation_engine = _read(ROOT / "native" / "operation_engine.cpp")

    # The public registry API is the authority consumed by both dispatchers.
    assert "backend_registry()" in runtime
    assert "registry_manifest_json" in runtime
    assert "backend_registry()" in mapper
    assert "backend_by_fstype" in mapper
    assert "backend_by_fstype" in operation_engine
    assert "operation_for" in operation_engine

    # Protect the audited product scope without pinning vector size, statement
    # order, whitespace, helper names, or the exact spelling of construction.
    literals = set(re.findall(r'"([A-Za-z0-9+_.-]+)"', runtime))
    for filesystem, worker in NATIVE_WRITERS.items():
        assert filesystem in literals, f"runtime registry lost {filesystem}"
        assert worker in literals, f"runtime registry lost worker {worker}"
    assert READ_ONLY_BACKENDS <= literals


def test_operation_dispatch_stays_filesystem_neutral() -> None:
    source = _read(ROOT / "native" / "operation_engine.cpp")
    assert "backend_by_fstype" in source
    assert "operation_for" in source
    assert "resolve_program" in source

    # A dispatcher may validate generic target state, but filesystem selection
    # belongs to the registry/worker contract rather than a central if-chain.
    compact = re.sub(r"\s+", " ", source)
    for filesystem in {*NATIVE_WRITERS, *READ_ONLY_BACKENDS}:
        assert not re.search(
            rf"\bfilesystem\s*==\s*\"{re.escape(filesystem)}\"", compact
        ), f"central dispatcher gained {filesystem}-specific policy"


def test_filesystem_policy_remains_owned_by_native_packages() -> None:
    package_for = {
        "fat12": "fat",
        "fat16": "fat",
        "fat32": "fat",
        "ext4": "ext4",
        "affs": "affs",
        "hfsplus": "hfsplus",
        "hfs": "hfs",
    }
    for filesystem in NATIVE_WRITERS:
        package = package_for.get(filesystem, filesystem)
        native = GUI / "filesystems" / package / "native"
        assert native.is_dir(), f"{filesystem} lost its native ownership boundary"
        native_sources = [
            path for path in native.iterdir()
            if path.is_file() and path.suffix in {".c", ".h", ".cpp", ".hpp"}
        ]
        assert native_sources, f"{filesystem} has no native implementation"
        assert not list((GUI / "filesystems" / package).glob("plugin.py")), (
            f"{filesystem} reintroduced a second Python capability declaration"
        )

    for obsolete in ("fat12", "fat16", "fat32"):
        assert not (GUI / "filesystems" / obsolete).exists()


def test_desktop_tests_use_public_boundaries_not_private_implementation() -> None:
    source = _read(ROOT / "tests" / "test_desktop_analysis.cpp")
    assert '#include "../native/desktop.cpp"' not in source
    assert "LD_DESKTOP_ANALYSIS_TEST" not in source
    assert "friend struct DesktopAnalysisTest" not in source
    assert "desktop_policy.hpp" in source
    assert "backend_by_fstype" in source


def test_common_is_a_versioned_dependency_not_copied_source() -> None:
    common = ROOT / "shared" / "infiltratr-common"
    assert common.is_dir()
    assert _read(common / "VERSION").strip() == "1.19.38"
    gitmodules = _read(ROOT / ".gitmodules")
    assert "shared/infiltratr-common" in gitmodules
    assert "Infiltrator-Libraries.git" in gitmodules

    cmake = _cmake_source()
    assert "add_subdirectory" in cmake
    assert "InfiltratrCommon::Common" in cmake
    assert not (ROOT / "vendor").exists()


def test_production_control_plane_is_native() -> None:
    architecture = _read(ROOT.parent / "docs" / "ARCHITECTURE.md")
    assert "native/runtime.cpp" in architecture
    assert "single application registry" in architecture

    # Retained Python files are fixtures/tests only.  Filesystem packages must
    # not own executable Python dispatch/plugin declarations.
    for package in (GUI / "filesystems").iterdir():
        if package.is_dir():
            assert not list(package.glob("*.py")), (
                f"production filesystem package {package.name} gained Python control code"
            )


def main() -> None:
    tests = [
        value for name, value in sorted(globals().items())
        if name.startswith("test_") and callable(value)
    ]
    for test in tests:
        test()
    print(f"architecture contracts: {len(tests)} passed")


if __name__ == "__main__":
    main()
