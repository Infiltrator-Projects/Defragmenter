#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression tests for APT/system-managed Defragmenter updates."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_defragmenter_uses_system_update_manager_only() -> None:
    desktop = (ROOT / "packaging" / "io.github.defragmenter.desktop").read_text()
    wrapper = (ROOT / "packaging" / "defragmenter").read_text()
    package_builder = (ROOT / "cmake" / "project.cmake").read_text()
    local_runner = (ROOT / "packaging" / "local-run-header.sh.in").read_text()

    assert "Exec=/usr/bin/defragmenter" in desktop
    assert "TryExec=/usr/bin/defragmenter" in desktop
    assert "CheckUpdates" not in desktop
    assert "--check-updates" not in desktop

    assert "/usr/bin/defragmenter-desktop" in wrapper
    assert "python" not in wrapper.lower()
    assert "update_launcher.py" not in wrapper
    assert "pkexec" not in wrapper

    assert "gui/update_helper.py" not in package_builder
    assert not (ROOT / "gui" / "update_helper.py").exists()
    assert not (ROOT / "gui" / "ui" / "update_launcher.py").exists()
    assert not (ROOT / "gui" / "ui" / "update_service.py").exists()

    assert "normal system updater" in local_runner
    assert "detected by Defragmenter itself" not in local_runner


if __name__ == "__main__":
    test_defragmenter_uses_system_update_manager_only()
    print("Defragmenter system-update contract passed")
