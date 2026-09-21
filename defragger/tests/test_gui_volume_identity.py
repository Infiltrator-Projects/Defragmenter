#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression tests for GUI filesystem identity and block-device discovery."""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path
from subprocess import CompletedProcess

ROOT = Path(__file__).resolve().parents[1]
GUI = ROOT / "gui"
if str(GUI) not in sys.path:
    sys.path.insert(0, str(GUI))

from ui.backend_catalog import BackendCatalog
from ui.devices import Volume, discover_volumes, natural_device_sort_key
from ui.volume_coordinator import VolumeCoordinator
from ui.volume_store import VolumeStore


def _catalog() -> BackendCatalog:
    return BackendCatalog.from_manifest(
        {
            "backends": [
                {"id": "fat12", "aliases": [], "capabilities": 1, "operations": []},
                {"id": "fat16", "aliases": [], "capabilities": 1, "operations": []},
                {"id": "fat32", "aliases": [], "capabilities": 1, "operations": []},
                {"id": "ext4", "aliases": ["ext2", "ext3"], "capabilities": 1, "operations": []},
                {
                    "id": "affs",
                    "aliases": ["amiga", "ofs", "ffs", "dostype"],
                    "capabilities": 1,
                    "operations": [],
                },
                {"id": "hfsplus", "aliases": ["hfs+"], "capabilities": 1, "operations": []},
                {"id": "sfs", "aliases": ["sfs0", "sfs2"], "capabilities": 1, "operations": []},
                {"id": "pfs3", "aliases": ["pfs"], "capabilities": 1, "operations": []},
                {"id": "apfs", "aliases": ["apfs"], "capabilities": 1, "operations": []},
            ]
        }
    )


def _volume(
    *,
    path: str = "/dev/mmcblk0p1",
    fstype: str = "vfat",
    fs_version: str = "FAT12",
    filesystem_uuid: str = "1111-2222",
    partition_uuid: str = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
    image: bool = False,
) -> Volume:
    return Volume(
        catalog=_catalog(),
        path=path,
        name=Path(path).name,
        fstype=fstype,
        label="LD_FAT12",
        size=64 * 1024 * 1024,
        mountpoints=[],
        removable=False,
        readonly=False,
        model="",
        transport="file" if image else "mmc",
        image=image,
        fs_version=fs_version,
        filesystem_uuid=filesystem_uuid,
        partition_uuid=partition_uuid,
    )


def test_lsblk_metadata_refines_generic_vfat() -> None:
    payload = {
        "blockdevices": [
            {
                "name": "mmcblk0p1",
                "path": "/dev/mmcblk0p1",
                "type": "part",
                "fstype": "vfat",
                "fsver": "FAT12",
                "label": "LD_FAT12",
                "partlabel": "LD_FAT12",
                "uuid": "ABCD-1234",
                "partuuid": "11111111-2222-3333-4444-555555555555",
                "size": 64 * 1024 * 1024,
                "mountpoints": [None],
                "rm": 0,
                "ro": 0,
                "model": "",
                "tran": "mmc",
            }
        ]
    }

    def fake_run(args, **_kwargs):
        assert "FSVER" in args[-1]
        assert "PARTLABEL" in args[-1]
        assert "UUID" in args[-1]
        assert "PARTUUID" in args[-1]
        return CompletedProcess(args, 0, stdout=json.dumps(payload), stderr="")

    volumes = discover_volumes(_catalog(), run=fake_run)
    assert len(volumes) == 1
    volume = volumes[0]
    assert volume.normalized_fstype == "fat12"
    assert volume.display_fstype == "fat12"
    assert "— FAT12 —" in volume.display_name
    assert volume.filesystem_uuid == "ABCD-1234"
    assert volume.partition_uuid.startswith("11111111-")


def test_amiga_discovery_does_not_depend_on_lsblk_fstype() -> None:
    def node(path: str, *, fstype: str = "", partlabel: str = "", label: str = "") -> dict:
        return {
            "name": Path(path).name,
            "path": path,
            "type": "part",
            "fstype": fstype,
            "fsver": "",
            "label": label,
            "partlabel": partlabel,
            "uuid": "",
            "partuuid": f"part-{Path(path).name}",
            "size": 1024 * 1024 * 1024,
            "mountpoints": [None],
            "rm": 0,
            "ro": 0,
            "model": "",
            "tran": "mmc",
        }

    payload = {
        "blockdevices": [
            # libblkid commonly leaves these two blank.  Their Test Media GPT
            # labels are a deterministic fallback when an unprivileged probe
            # cannot open the physical block device.
            node("/dev/mmcblk0p11", partlabel="LD_OFS"),
            node("/dev/mmcblk0p12", partlabel="LD_FFS"),
            # A normal Amiga partition with no Test Media label is recovered
            # from the authoritative first-party native probe.
            node("/dev/mmcblk0p20", partlabel="DH0"),
            # Raw SFS/PFS3 creators are identified from their Test Media labels
            # when an unprivileged native probe cannot open the physical device.
            node("/dev/mmcblk0p13", partlabel="LD_SFS"),
            node("/dev/mmcblk0p14", partlabel="LD_PFS3"),
            # APFS now has a first-party creator.  Its Test Media GPT label
            # must override a stale pre-rebuild signature.
            node(
                "/dev/mmcblk0p15",
                fstype="hfsplus",
                partlabel="LD_APFS",
                label="LD_APFS",
            ),
        ]
    }

    def fake_run(args, **_kwargs):
        return CompletedProcess(args, 0, stdout=json.dumps(payload), stderr="")

    def fake_probe(path: str) -> str:
        return "ffs" if path.endswith("p20") else ""

    volumes = discover_volumes(_catalog(), run=fake_run, probe_unknown=fake_probe)
    by_path = {volume.path: volume for volume in volumes}
    assert set(by_path) == {
        "/dev/mmcblk0p11",
        "/dev/mmcblk0p12",
        "/dev/mmcblk0p13",
        "/dev/mmcblk0p14",
        "/dev/mmcblk0p15",
        "/dev/mmcblk0p20",
    }
    assert by_path["/dev/mmcblk0p11"].normalized_fstype == "affs"
    assert by_path["/dev/mmcblk0p11"].display_fstype == "ofs"
    assert "— LD_OFS — OFS —" in by_path["/dev/mmcblk0p11"].display_name
    assert by_path["/dev/mmcblk0p12"].normalized_fstype == "affs"
    assert by_path["/dev/mmcblk0p12"].display_fstype == "ffs"
    assert by_path["/dev/mmcblk0p13"].display_fstype == "sfs"
    assert by_path["/dev/mmcblk0p14"].display_fstype == "pfs3"
    assert by_path["/dev/mmcblk0p15"].display_fstype == "apfs"
    assert by_path["/dev/mmcblk0p20"].display_fstype == "ffs"


def test_block_devices_sort_by_numeric_partition_number() -> None:
    paths = [
        "/dev/mmcblk0p1",
        "/dev/mmcblk0p10",
        "/dev/mmcblk0p13",
        "/dev/mmcblk0p2",
        "/dev/mmcblk0p19",
        "/dev/mmcblk0p3",
        "/dev/nvme0n1p12",
        "/dev/nvme0n1p3",
        "/dev/nvme1n1p1",
        "/dev/nvme1n1p10",
        "/dev/nvme1n1p2",
    ]
    assert sorted(paths, key=natural_device_sort_key) == [
        "/dev/mmcblk0p1",
        "/dev/mmcblk0p2",
        "/dev/mmcblk0p3",
        "/dev/mmcblk0p10",
        "/dev/mmcblk0p13",
        "/dev/mmcblk0p19",
        "/dev/nvme0n1p3",
        "/dev/nvme0n1p12",
        "/dev/nvme1n1p1",
        "/dev/nvme1n1p2",
        "/dev/nvme1n1p10",
    ]


def test_unknown_generic_fat_is_not_labeled_fat32() -> None:
    volume = _volume(fs_version="")
    # The common FAT32 backend remains the compatibility routing fallback, but
    # the GUI must not turn incomplete Linux metadata into a false FAT32 claim.
    assert volume.normalized_fstype == "vfat"
    assert volume.display_fstype == "fat"
    assert "— FAT —" in volume.display_name


def test_rebuilt_same_path_does_not_reuse_cached_map() -> None:
    store = VolumeStore()
    original = _volume()
    assert store.refresh([original]) == 0
    store.remember_map({"filesystem": "FAT12", "marker": "old"})
    assert store.cached_map() is not None

    # Rediscovering the same filesystem instance may safely reuse its map.
    same = _volume()
    assert store.refresh([same], preserve_path=same.path) == 0
    assert store.cached_map() is not None

    # Test Media destroys/recreates both the filesystem UUID and GPT partition
    # UUID while reusing /dev/mmcblk0p1.  The old map must not survive that.
    rebuilt = _volume(
        filesystem_uuid="9999-AAAA",
        partition_uuid="99999999-8888-7777-6666-555555555555",
    )
    assert store.refresh([rebuilt], preserve_path=rebuilt.path) == 0
    assert store.cached_map() is None


def test_selection_revalidates_rebuilt_path_without_manual_refresh() -> None:
    original = _volume()
    rebuilt = _volume(
        filesystem_uuid="BEEF-1170",
        partition_uuid="11701170-2222-3333-4444-555555555555",
    )
    state = {"volume": original}

    def discover(_catalog: BackendCatalog) -> list[Volume]:
        return [state["volume"]]

    coordinator = VolumeCoordinator(_catalog(), discover=discover)
    assert coordinator.refresh() == 0
    coordinator.store.select(0)
    coordinator.remember_map({"filesystem": "FAT12", "marker": "old"})
    assert coordinator.cached_map() is not None

    # No explicit refresh call here: selecting the old in-memory path must
    # rediscover it, see the new identities, discard the old map and analyse.
    state["volume"] = rebuilt
    selection = coordinator.select(0)
    assert selection.volume is not None
    assert selection.volume.filesystem_uuid == "BEEF-1170"
    assert selection.cached_map is None


def test_uuidless_devices_use_refresh_ephemeral_cache_identity() -> None:
    store = VolumeStore()
    first = _volume(filesystem_uuid="", partition_uuid="")
    store.refresh([first])
    store.remember_map({"filesystem": "FAT12", "marker": "old"})
    assert store.cached_map() is not None

    rediscovered = _volume(filesystem_uuid="", partition_uuid="")
    store.refresh([rediscovered], preserve_path=rediscovered.path)
    assert store.cached_map() is None


def test_replaced_image_at_same_path_does_not_reuse_cached_map() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "same.img"
        path.write_bytes(b"A" * 4096)
        image = _volume(
            path=str(path),
            fstype="fat12",
            fs_version="FAT12",
            filesystem_uuid="",
            partition_uuid="",
            image=True,
        )
        store = VolumeStore()
        store.add_image(image)
        store.remember_map({"filesystem": "FAT12", "marker": "old"})
        assert store.cached_map() is not None

        path.write_bytes(b"B" * 8192)
        assert store.cached_map() is None


def test_invalidate_removes_every_identity_for_one_path() -> None:
    store = VolumeStore()
    first = _volume()
    store.refresh([first])
    store.remember_map({"filesystem": "FAT12"})
    assert store.map_cache
    store.invalidate(first.path)
    assert not store.map_cache


def main() -> None:
    test_lsblk_metadata_refines_generic_vfat()
    test_amiga_discovery_does_not_depend_on_lsblk_fstype()
    test_block_devices_sort_by_numeric_partition_number()
    test_unknown_generic_fat_is_not_labeled_fat32()
    test_rebuilt_same_path_does_not_reuse_cached_map()
    test_selection_revalidates_rebuilt_path_without_manual_refresh()
    test_uuidless_devices_use_refresh_ephemeral_cache_identity()
    test_replaced_image_at_same_path_does_not_reuse_cached_map()
    test_invalidate_removes_every_identity_for_one_path()
    print("GUI volume identity and raw-filesystem discovery regression tests passed")


if __name__ == "__main__":
    main()
