#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression tests for APT/system-managed Linux Defragger updates."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_linux_defragger_uses_system_update_manager_only() -> None:
    desktop = (ROOT / "packaging" / "io.github.linuxdefragger.desktop").read_text()
    wrapper = (ROOT / "packaging" / "linux-defragger").read_text()
    package_builder = (ROOT / "cmake" / "project.cmake").read_text()
    local_runner = (ROOT / "packaging" / "local-run-header.sh.in").read_text()

    assert "Exec=/usr/bin/linux-defragger" in desktop
    assert "TryExec=/usr/bin/linux-defragger" in desktop
    assert "CheckUpdates" not in desktop
    assert "--check-updates" not in desktop

    assert "exec /usr/bin/linux-defragger-desktop" in wrapper
    assert "python3" not in wrapper
    assert "update_launcher.py" not in wrapper
    assert "pkexec" not in wrapper

    assert "gui/update_helper.py" not in package_builder
    assert not (ROOT / "gui" / "update_helper.py").exists()
    assert not (ROOT / "gui" / "ui" / "update_launcher.py").exists()
    assert not (ROOT / "gui" / "ui" / "update_service.py").exists()

    assert "normal system updater" in local_runner
    assert "detected by Linux Defragger itself" not in local_runner


if __name__ == "__main__":
    test_linux_defragger_uses_system_update_manager_only()
    print("Linux Defragger system-update contract passed")
