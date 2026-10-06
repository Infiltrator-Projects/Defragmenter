#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Entry point for the native architecture contract suite."""

from __future__ import annotations

import _architecture_contracts as contracts


for _name in dir(contracts):
    if _name.startswith("test_") and callable(getattr(contracts, _name)):
        globals()[_name] = getattr(contracts, _name)


def test_qualified_ufs_writer_is_registered_and_fail_closed_by_format() -> None:
    source = (
        contracts.GUI / "filesystems" / "ufs" / "native" / "ufs_worker.c"
    ).read_text()
    native = (
        contracts.GUI / "filesystems" / "ufs" / "native" / "ufs_native.c"
    ).read_text()

    assert "UFS mutation is not production-qualified" not in source
    assert not (
        contracts.GUI / "filesystems" / "ufs" / "plugin.py"
    ).exists()
    assert source.count("ld_device_capture_binding(") >= 2
    assert "--write" in source and "--confirm" in source
    assert "writer_supported" in native
    for required in (
        "clean, non-journalled, snapshot-free",
        "regular-file fragment geometry is invalid",
        "shared or overlapping regular-file data",
    ):
        assert required in native
    assert "choose_run_in_group(" in native
    assert "pass < summary->cylinder_groups" in native

    runtime = (contracts.ROOT / "native" / "runtime.cpp").read_text()
    assert '"ufs", "Solaris/BSD UFS"' in runtime
    ufs_entry = runtime.split(
        '"ufs", "Solaris/BSD UFS"', 1
    )[1].split("result.push_back", 1)[0]
    assert 'write, "exact", "ufs-native"' in ufs_entry
    assert "standard_write_ops" in ufs_entry


contracts.test_qualified_ufs_writer_is_registered_and_fail_closed_by_format = (
    test_qualified_ufs_writer_is_registered_and_fail_closed_by_format
)


def main() -> None:
    contracts.main()


if __name__ == "__main__":
    main()
