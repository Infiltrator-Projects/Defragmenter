#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression tests for mount topology and privileged-helper shutdown safety."""

from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "gui"))

from core import devices
from ui import support


def _link(link: Path, target: Path) -> None:
    link.parent.mkdir(parents=True, exist_ok=True)
    link.symlink_to(target, target_is_directory=True)


def test_block_topology() -> None:
    with tempfile.TemporaryDirectory(prefix="linux-defragger-topology-") as directory:
        root = Path(directory)
        nodes = root / "nodes"
        sys_block = root / "sys-dev-block"
        disk = nodes / "block" / "sda"
        partition = disk / "sda1"
        sibling = disk / "sda2"
        mapper = nodes / "virtual" / "block" / "dm-0"
        partition.mkdir(parents=True)
        sibling.mkdir(parents=True)
        mapper.mkdir(parents=True)
        sys_block.mkdir()
        _link(sys_block / "8:0", disk)
        _link(sys_block / "8:1", partition)
        _link(sys_block / "8:2", sibling)
        _link(sys_block / "253:0", mapper)
        _link(disk / "holders" / "dm-0", mapper)
        _link(mapper / "slaves" / "sda", disk)

        old_root = devices._SYS_DEV_BLOCK
        devices._SYS_DEV_BLOCK = sys_block
        try:
            # Whole-disk mutation overlaps both child partitions.
            assert devices._related_block_ids("8:0") == {
                "8:0", "8:1", "8:2", "253:0"
            }
            # Partition mutation overlaps its parent container and mappings,
            # but NOT the disjoint sibling partition.
            assert devices._related_block_ids("8:1") == {"8:0", "8:1", "253:0"}
            # The whole-disk mapper still overlaps every partition of its slave.
            assert devices._related_block_ids("253:0") == {
                "8:0", "8:1", "8:2", "253:0"
            }
        finally:
            devices._SYS_DEV_BLOCK = old_root


def test_regular_image_mount_source() -> None:
    with tempfile.TemporaryDirectory(prefix="linux-defragger-mountinfo-") as directory:
        root = Path(directory)
        image = root / "volume.img"
        image.write_bytes(b"\0" * 4096)
        mountinfo = root / "mountinfo"
        mountinfo.write_text(
            f"36 25 7:0 / /mnt rw,relatime - ext4 {image} rw\n",
            encoding="utf-8",
        )
        old_mountinfo = devices._MOUNTINFO
        old_sysfs = devices._SYS_DEV_BLOCK
        devices._MOUNTINFO = mountinfo
        devices._SYS_DEV_BLOCK = root / "empty-sysfs"
        try:
            assert devices.is_mounted(str(image))
            try:
                devices.require_unmounted(str(image))
            except RuntimeError as exc:
                assert "mounted" in str(exc)
            else:
                raise AssertionError("mounted regular image was accepted for mutation")
        finally:
            devices._MOUNTINFO = old_mountinfo
            devices._SYS_DEV_BLOCK = old_sysfs


def test_root_owned_journal_namespace() -> None:
    old_xdg = os.environ.get("XDG_STATE_HOME")
    os.environ["XDG_STATE_HOME"] = "/tmp/attacker-controlled-state"
    try:
        assert support.state_dir() == Path("/var/lib/linux-defragger/state") / str(os.getuid())
    finally:
        if old_xdg is None:
            os.environ.pop("XDG_STATE_HOME", None)
        else:
            os.environ["XDG_STATE_HOME"] = old_xdg

def test_native_privileged_helper_contract() -> None:
    source = (ROOT / "native" / "privileged_helper.cpp").read_text(encoding="utf-8")
    engine = (ROOT / "native" / "operation_engine.cpp").read_text(encoding="utf-8")
    policy = (ROOT / "native" / "helper_policy.cpp").read_text(encoding="utf-8")

    assert "posix_spawn(" in source
    assert "fork(" not in source
    assert "POSIX_SPAWN_SETPGROUP" in source
    assert "POSIX_SPAWN_SETSIGDEF" in source
    assert "SIGPIPE" in source and "SIG_IGN" in source
    assert "transport_failed_" in source
    assert "worker_running_" in source
    assert "kill(-child, SIGINT)" in source
    assert "stop_active_and_wait()" in source

    assert "ld_path_is_mounted(device.c_str())" in engine
    assert "execv(raw[0], raw.data())" in engine
    assert '"/var/lib/linux-defragger/state"' in policy
    assert "journal.lexically_normal().parent_path() != expected_parent" in policy


if __name__ == "__main__":
    test_block_topology()
    test_regular_image_mount_source()
    test_root_owned_journal_namespace()
    test_native_privileged_helper_contract()
    print("mount-topology and native privileged-helper safety tests passed")
