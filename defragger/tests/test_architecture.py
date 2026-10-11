#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""High-level architectural contracts for Defragmenter.

Prefer observable public contracts over source-shape assertions. Structural checks
are kept only where the directory/dependency boundary itself is the contract.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
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
    "ufs": "ufs-native",
}
READ_ONLY_BACKENDS = {"zfs", "swap"}


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _cmake_source() -> str:
    paths = [ROOT / "CMakeLists.txt", *sorted((ROOT / "cmake").glob("*.cmake"))]
    return "\n".join(_read(path) for path in paths if path.is_file())


def _build_dir() -> Path:
    configured = os.environ.get("LINUX_DEFRAGGER_BUILD_DIR")
    if configured:
        return Path(configured)
    return ROOT / "build"


def _registry_manifest() -> dict[str, object]:
    engine = _build_dir() / "linux-defragger-operation-engine"
    assert engine.is_file(), f"native operation engine is unavailable: {engine}"
    completed = subprocess.run(
        [str(engine), "--list-plugins"],
        check=True,
        capture_output=True,
        text=True,
    )
    payload = json.loads(completed.stdout)
    assert isinstance(payload, dict)
    assert payload.get("schema") == 3
    backends = payload.get("backends")
    assert isinstance(backends, list)
    return payload


def _backends_by_id() -> dict[str, dict[str, object]]:
    payload = _registry_manifest()
    result: dict[str, dict[str, object]] = {}
    for entry in payload["backends"]:
        assert isinstance(entry, dict)
        backend_id = entry.get("id")
        assert isinstance(backend_id, str) and backend_id
        assert backend_id not in result, f"duplicate backend id: {backend_id}"
        result[backend_id] = entry
    return result


def test_native_languages_are_declared_at_the_project_boundary() -> None:
    cmake = _read(ROOT / "CMakeLists.txt")
    declaration = re.search(r"project\s*\((?P<body>[^)]*)\)", cmake, re.S)
    assert declaration is not None
    body = declaration.group("body")
    languages = re.search(r"\bLANGUAGES\b(?P<items>.*)", body, re.S)
    assert languages is not None
    tokens = set(re.findall(r"[A-Za-z0-9_+.-]+", languages.group("items")))
    assert {"C", "CXX"} <= tokens


def test_registry_manifest_is_the_capability_authority() -> None:
    backends = _backends_by_id()
    assert len(backends) == 18
    assert set(NATIVE_WRITERS) <= set(backends)
    assert READ_ONLY_BACKENDS <= set(backends)

    for backend_id, worker in NATIVE_WRITERS.items():
        backend = backends[backend_id]
        operations = backend.get("operations")
        assert isinstance(operations, list) and operations, (
            f"write backend {backend_id} exposes no operations"
        )
        workers = {
            operation.get("worker")
            for operation in operations
            if isinstance(operation, dict)
        }
        assert workers == {worker}, (
            f"{backend_id} operation worker drift: {workers!r} != {worker!r}"
        )

    for backend_id in READ_ONLY_BACKENDS:
        assert backends[backend_id].get("operations") == []


def test_registry_exposes_mapper_policy_without_hidden_source_lists() -> None:
    backends = _backends_by_id()
    worker_preflight = {"apfs", "btrfs", "ext4", "hfs", "hfsplus", "ufs", "xfs"}
    for backend_id, backend in backends.items():
        mode = backend.get("qualification_mode")
        assert mode in {"none", "worker-preflight", "map-or-worker-preflight"}
        expected = (
            "map-or-worker-preflight"
            if backend_id == "ntfs"
            else "worker-preflight"
            if backend_id in worker_preflight
            else "none"
        )
        assert mode == expected, f"{backend_id} qualification policy drift"

    assert backends["ext4"].get("map_auxiliary_worker") == "ext-metadata"
    for backend_id, backend in backends.items():
        if backend_id != "ext4":
            assert backend.get("map_auxiliary_worker") == ""


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
