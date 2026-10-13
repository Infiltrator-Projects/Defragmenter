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
CAP_DEFRAG = 1 << 2


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


def _operation_workers(backend: dict[str, object]) -> set[str]:
    operations = backend.get("operations")
    assert isinstance(operations, list)
    workers = {
        operation.get("worker")
        for operation in operations
        if isinstance(operation, dict)
    }
    assert all(isinstance(worker, str) and worker for worker in workers)
    return workers


def _native_package(backend_id: str, backend: dict[str, object]) -> str:
    candidates = [backend_id]
    workers = _operation_workers(backend)
    if workers:
        assert len(workers) == 1, (
            f"{backend_id} operations span multiple native workers: {workers!r}"
        )
        worker = next(iter(workers))
        assert worker.endswith("-native"), (
            f"{backend_id} operation worker is not a native package: {worker!r}"
        )
        candidates.append(worker.removesuffix("-native"))

    for candidate in dict.fromkeys(candidates):
        if (GUI / "filesystems" / candidate / "native").is_dir():
            return candidate
    raise AssertionError(
        f"{backend_id} has no native package matching its registry contract: "
        f"{candidates!r}"
    )


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

    writable = 0
    read_only = 0
    for backend_id, backend in backends.items():
        capabilities = backend.get("capabilities")
        operations = backend.get("operations")
        assert isinstance(capabilities, int)
        assert isinstance(operations, list)

        if operations:
            writable += 1
            assert capabilities & CAP_DEFRAG, (
                f"{backend_id} exposes mutation operations without CAP_DEFRAG"
            )
            workers = _operation_workers(backend)
            assert len(workers) == 1, (
                f"{backend_id} operation worker drift: {workers!r}"
            )
        else:
            read_only += 1
            assert not capabilities & CAP_DEFRAG, (
                f"{backend_id} advertises CAP_DEFRAG without operations"
            )

    assert writable > 0
    assert read_only > 0


def test_registry_exposes_mapper_policy_without_hidden_source_lists() -> None:
    backends = _backends_by_id()
    for backend_id, backend in backends.items():
        operations = backend.get("operations")
        mode = backend.get("qualification_mode")
        accepted = backend.get("accepted_map_accuracies")
        accuracy = backend.get("map_accuracy")
        auxiliary = backend.get("map_auxiliary_worker")

        assert mode in {"none", "worker-preflight", "map-or-worker-preflight"}
        assert isinstance(accepted, list) and accepted
        assert all(isinstance(item, str) and item for item in accepted)
        assert len(accepted) == len(set(accepted))
        assert isinstance(accuracy, str) and accuracy
        assert isinstance(auxiliary, str)

        if not operations:
            assert mode == "none", (
                f"read-only backend {backend_id} declares mutation qualification"
            )


def test_filesystem_policy_remains_owned_by_native_packages() -> None:
    for backend_id, backend in _backends_by_id().items():
        package = _native_package(backend_id, backend)
        native = GUI / "filesystems" / package / "native"
        native_sources = [
            path for path in native.iterdir()
            if path.is_file() and path.suffix in {".c", ".h", ".cpp", ".hpp"}
        ]
        assert native_sources, f"{backend_id} has no native implementation"
        assert not list((GUI / "filesystems" / package).glob("plugin.py")), (
            f"{backend_id} reintroduced a second Python capability declaration"
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

    # One repository-level declaration owns the gitlink. Do not mirror
    # .gitmodules inside the application directory.
    assert not (ROOT / ".gitmodules").exists()
    gitmodules = _read(ROOT.parent / ".gitmodules")
    assert "defragger/shared/infiltratr-common" in gitmodules
    assert "Infiltrator-Libraries.git" in gitmodules

    cmake = _cmake_source()
    assert "add_subdirectory" in cmake
    assert "InfiltratrCommon::Common" in cmake
    assert not (ROOT / "vendor").exists()


def test_production_control_plane_is_native() -> None:
    architecture = _read(ROOT.parent / "docs" / "ARCHITECTURE.md")
    assert "native/runtime.cpp" in architecture
    assert "single application registry" in architecture

    # The retired Python application/model compatibility graph must stay gone.
    # Python remains valid under tests/ as an independent fixture/driver language,
    # but no Python source belongs inside the production gui/ tree.
    python_product_files = sorted(GUI.rglob("*.py"))
    assert not python_product_files, (
        f"Python application/control code returned: {python_product_files!r}"
    )
    for retired in (
        ROOT / "packaging" / "generated" / "version.py.in",
        ROOT / "tests" / "run_typecheck.sh",
        ROOT / "tests" / "pyrightconfig.json",
        ROOT / "pytest.ini",
        ROOT / "tools" / "update-theme-tokens.py",
    ):
        assert not retired.exists(), f"retired Python compatibility artefact returned: {retired}"


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
