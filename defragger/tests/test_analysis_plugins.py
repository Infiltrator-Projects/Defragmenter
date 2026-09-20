#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Smoke-test the standard read-only analysis registry and mapper interface."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUI = ROOT / "gui"
sys.path.insert(0, str(GUI))

from backends.registry import Registry
from ui.backend_catalog import BackendCatalog
from ui.operation_planner import build_analysis_arguments
from ui.volume_coordinator import VolumeCoordinator

registry = Registry()
manifest = registry.manifest()
assert len(manifest) == 18
assert all(item["capabilities"] & 1 for item in manifest)
assert all(item["capabilities"] & 2 for item in manifest)

mapper = GUI / "allocation_mapper.py"
environment = {**os.environ, "PYTHONPATH": str(GUI), "PYTHONDONTWRITEBYTECODE": "1"}
result = subprocess.run(
    [sys.executable, str(mapper), "--list-backends"],
    text=True,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    env=environment,
    check=True,
)
listed = json.loads(result.stdout)["backends"]
assert [item["id"] for item in listed] == [item["id"] for item in manifest]

native_mapper = Path(os.environ.get("LINUX_DEFRAGGER_BUILD_DIR", str(ROOT / "build"))) / "linux-defragger-mapper"
if native_mapper.is_file():
    native_result = subprocess.run(
        [str(native_mapper), "--list-backends"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    native_listed = json.loads(native_result.stdout)["backends"]
    assert native_listed == manifest, "C++ mapper manifest drifted from established plugin contract"

    equals_form = subprocess.run(
        [
            str(native_mapper),
            "--list-backends",
            "--fstype=ntfs",
            "--cells=128",
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    assert json.loads(equals_form.stdout)["backends"] == manifest

    for bad_cells in ("-1", "0", "128junk", "+128", "1048577"):
        rejected = subprocess.run(
            [str(native_mapper), "--list-backends", "--cells", bad_cells],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        assert rejected.returncode == 2, (
            f"C++ mapper accepted invalid --cells value {bad_cells!r}"
        )

    rejected_equals = subprocess.run(
        [str(native_mapper), "--list-backends", "--cells=-1"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert rejected_equals.returncode == 2

    with tempfile.TemporaryDirectory(prefix="defragger-fat-map-") as temp_dir:
        fat_image = Path(temp_dir) / "fat12.img"
        subprocess.run(
            [
                sys.executable,
                str(ROOT / "tests" / "make_fat12_16_image.py"),
                "fat12",
                str(fat_image),
                "fragmented",
            ],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        fat_result = subprocess.run(
            [
                str(native_mapper),
                str(fat_image),
                "--fstype",
                "fat12",
                "--cells",
                "128",
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True,
        )
        fat_map = json.loads(fat_result.stdout)
        assert fat_map["filesystem"] == "FAT12"
        assert fat_map["backend_id"] == "fat12"
        assert fat_map["data_clusters"] > 0
        assert fat_map["cell_count"] == len(fat_map["cells"])

        for variant in ("fat12", "fat16"):
            subprocess.run(
                [sys.executable, str(ROOT / "tests" / "make_fat12_16_image.py"),
                 variant, str(fat_image), "fragmented"], check=True,
                stdout=subprocess.DEVNULL,
            )
            # Use the actual image-open and argument-construction path too:
            # blkid reports "vfat", without a FAT12/FAT16 FSVER field.
            volumes = VolumeCoordinator(BackendCatalog.from_manifest({"backends": manifest}))
            volume = volumes.open_image(str(fat_image))
            gui_arguments = build_analysis_arguments(
                str(native_mapper), volume, 128, minimum_cells=1,
                maximum_cells=1048576,
            )
            opened = subprocess.run(gui_arguments, check=True, capture_output=True, text=True)
            assert json.loads(opened.stdout)["backend_id"] == variant
            for generic in ("vfat", "fat", "msdos"):
                detected = subprocess.run(
                    [str(native_mapper), str(fat_image), "--fstype", generic,
                     "--cells", "128"], check=True, capture_output=True, text=True,
                )
                assert json.loads(detected.stdout)["backend_id"] == variant
            mismatch = subprocess.run(
                [str(native_mapper), str(fat_image), "--fstype", "fat32"],
                capture_output=True,
            )
            assert mismatch.returncode != 0, "explicit wrong FAT identity must fail"

        # Above the old 64 MiB capture ceiling, but within the GUI's limit.
        large_image = Path(temp_dir) / "large-fat32.img"
        subprocess.run(
            [sys.executable, str(ROOT / "tests" / "make_fragmented_image.py"),
             str(large_image), "2097152"], check=True, stdout=subprocess.DEVNULL,
        )
        with (Path(temp_dir) / "large-map.json").open("w+") as output:
            subprocess.run(
                [str(native_mapper), str(large_image), "--fstype", "vfat",
                 "--cells", "800000"], stdout=output, stderr=subprocess.PIPE,
                text=True, check=True, timeout=120,
            )
            assert output.tell() > 64 * 1024 * 1024
            output.seek(0)
            large_map = json.load(output)
        assert large_map["backend_id"] == "fat32"
        assert large_map["cell_count"] == len(large_map["cells"]) == 800000

invalid = subprocess.run(
    [sys.executable, str(mapper), "/dev/null", "--fstype", "ntfs", "--cells", "128"],
    text=True,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    env=environment,
)
assert invalid.returncode != 0
assert invalid.stderr.strip()


# Btrfs filesystems can be smaller than their containing partition.  The
# helper must remain part of the plugin when the analyser is slimmed down.
from backends import btrfs
synthetic = {
    "cells": [
        {"start": 0, "end": 4, "unknown": 0},
        {"start": 5, "end": 9, "unknown": 5},
    ],
    "unknown_bytes": 5 * 4096,
    "details": {},
}
marked = btrfs._mark_outside_tail(synthetic, 5, 10, 4096)
assert marked["cells"][0].get("outside", 0) == 0
assert marked["cells"][1]["outside"] == 5
assert marked["cells"][1]["unknown"] == 0
assert marked["outside_bytes"] == 5 * 4096
assert marked["unknown_bytes"] == 0

print("standard read-only plugin and mapper smoke tests passed")
