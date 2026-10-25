#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression tests for APT/system-managed Defragmenter updates."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_defragmenter_uses_system_update_manager_only() -> None:
    desktop = (ROOT / "packaging" / "io.github.defragmenter.desktop").read_text()
    cmake_main = (ROOT / "cmake" / "test_media.cmake").read_text()
    package_builder = (ROOT / "cmake" / "project.cmake").read_text()
    local_runner = (ROOT / "packaging" / "local-run-header.sh.in").read_text()
    desktop_code = (ROOT / "native" / "desktop.cpp").read_text()

    assert "Exec=/Programs/defragmenter" in desktop
    assert "TryExec=/Programs/defragmenter" in desktop
    assert "Icon=/System/Resources/Icons/defragmenter.png" in desktop
    test_media = (ROOT / "packaging" / "io.github.defragmenter.TestMedia.desktop").read_text()
    assert "Icon=/System/Resources/Icons/defragmenter.png" in test_media
    assert "CheckUpdates" not in desktop
    assert "--check-updates" not in desktop

    assert "OUTPUT_NAME defragmenter" in cmake_main
    assert "install(TARGETS defragmenter-desktop RUNTIME DESTINATION /Programs)" in cmake_main
    assert "install(PROGRAMS packaging/defragmenter" not in package_builder
    assert not (ROOT / "packaging" / "defragmenter").exists()
    assert "/usr/bin/defragmenter" not in desktop

    # Opening the GUI must not summon pkexec. The privileged helper is
    # started only after the user explicitly requests a protected operation.
    assert "auth_timer_" not in desktop_code
    assert "else start_helper();" in desktop_code
    assert "void start_helper()" in desktop_code

    assert "gui/update_helper.py" not in package_builder
    assert not (ROOT / "gui" / "update_helper.py").exists()
    assert not (ROOT / "gui" / "ui" / "update_launcher.py").exists()
    assert not (ROOT / "gui" / "ui" / "update_service.py").exists()

    assert "normal system updater" in local_runner
    assert "detected by Defragmenter itself" not in local_runner


if __name__ == "__main__":
    test_defragmenter_uses_system_update_manager_only()
    print("Defragmenter system-update contract passed")
