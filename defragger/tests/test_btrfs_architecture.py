#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prevent Btrfs analysis from drifting back into a Python implementation."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUI = ROOT / "gui"
PACKAGE = GUI / "filesystems" / "btrfs"
CMAKE = ROOT / "cmake" / "btrfs.cmake"
PATHS = GUI / "core" / "paths.py"


def main() -> None:
    native = PACKAGE / "native"
    assert native.is_dir()
    required = {"btrfs_native.h", "btrfs_native.c", "btrfs_worker.c"}
    assert required <= {path.name for path in native.iterdir() if path.is_file()}

    assert not list(PACKAGE.glob("*.py"))
    cmake = CMAKE.read_text(encoding="utf-8")
    assert "add_library(linux-defragger-btrfs-native" in cmake
    assert "add_executable(linux-defragger-btrfs-worker" in cmake
    assert (
        "install(TARGETS linux-defragger-btrfs-worker\n"
        "        RUNTIME DESTINATION lib/linux-defragger/filesystems/btrfs)"
    ) in cmake
    assert "tests/test_btrfs_native.py" in cmake

    paths = PATHS.read_text(encoding="utf-8")
    assert '"btrfs-native": ProgramPath(' in paths
    assert "linux-defragger-btrfs-worker" in paths

    top = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert 'include("${CMAKE_CURRENT_LIST_DIR}/cmake/btrfs.cmake")' in top

    print("Btrfs native C architecture guard passed")


if __name__ == "__main__":
    main()
