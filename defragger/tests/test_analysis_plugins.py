#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Smoke-test the authoritative native registry and mapper interface."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUI = ROOT / "gui"
if str(GUI) not in sys.path:
    sys.path.insert(0, str(GUI))

from ui.backend_catalog import BackendCatalog
from ui.engine_client import detect_image_fstype
from ui.operation_planner import build_analysis_arguments
from ui.volume_coordinator import VolumeCoordinator

BUILD = Path(os.environ.get("LINUX_DEFRAGGER_BUILD_DIR", str(ROOT / "build")))
MAPPER = BUILD / "linux-defragger-mapper"
assert MAPPER.is_file(), f"missing native mapper: {MAPPER}"

listed_result = subprocess.run(
    [str(MAPPER), "--list-backends"],
    text=True,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    check=True,
)
payload = json.loads(listed_result.stdout)
assert payload["schema"] == 2
manifest = payload["backends"]
assert len(manifest) == 18
assert all(int(item["capabilities"]) & 1 for item in manifest)
assert all(int(item["capabilities"]) & 2 for item in manifest)
assert len({item["id"] for item in manifest}) == 18

equals_form = subprocess.run(
    [str(MAPPER), "--list-backends", "--fstype=ntfs", "--cells=128"],
    text=True,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    check=True,
)
assert json.loads(equals_form.stdout)["backends"] == manifest

for bad_cells in ("-1", "0", "128junk", "+128", "1048577"):
    rejected = subprocess.run(
        [str(MAPPER), "--list-backends", "--cells", bad_cells],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert rejected.returncode == 2, f"native mapper accepted invalid --cells {bad_cells!r}"

rejected_equals = subprocess.run(
    [str(MAPPER), "--list-backends", "--cells=-1"],
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
        [str(MAPPER), str(fat_image), "--fstype", "fat12", "--cells", "128"],
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
            [
                sys.executable,
                str(ROOT / "tests" / "make_fat12_16_image.py"),
                variant,
                str(fat_image),
                "fragmented",
            ],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        catalog = BackendCatalog.from_manifest({"backends": manifest})
        volumes = VolumeCoordinator(
            catalog,
            detect_image=lambda path, current_catalog: detect_image_fstype(
                path, current_catalog, mapper=str(MAPPER)
            ),
        )
        volume = volumes.open_image(str(fat_image))
        assert volume.fstype == variant
        probe = subprocess.run(
            [str(MAPPER), str(fat_image), "--probe"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True,
        )
        probe_payload = json.loads(probe.stdout)
        assert probe_payload["filesystem"] == variant
        assert probe_payload["backend_id"] == variant
        gui_arguments = build_analysis_arguments(
            str(MAPPER),
            volume,
            128,
            minimum_cells=1,
            maximum_cells=1048576,
        )
        opened = subprocess.run(
            gui_arguments, check=True, capture_output=True, text=True
        )
        assert json.loads(opened.stdout)["backend_id"] == variant
        for generic in ("vfat", "fat", "msdos"):
            detected = subprocess.run(
                [
                    str(MAPPER),
                    str(fat_image),
                    "--fstype",
                    generic,
                    "--cells",
                    "128",
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            assert json.loads(detected.stdout)["backend_id"] == variant
        mismatch = subprocess.run(
            [str(MAPPER), str(fat_image), "--fstype", "fat32"],
            capture_output=True,
        )
        assert mismatch.returncode != 0, "explicit wrong FAT identity must fail"

    large_image = Path(temp_dir) / "large-fat32.img"
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "tests" / "make_fragmented_image.py"),
            str(large_image),
            "2097152",
        ],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    with (Path(temp_dir) / "large-map.json").open("w+") as output:
        subprocess.run(
            [
                str(MAPPER),
                str(large_image),
                "--fstype",
                "vfat",
                "--cells",
                "800000",
            ],
            stdout=output,
            stderr=subprocess.PIPE,
            text=True,
            check=True,
            timeout=120,
        )
        assert output.tell() > 64 * 1024 * 1024
        output.seek(0)
        large_map = json.load(output)
    assert large_map["backend_id"] == "fat32"
    assert large_map["cell_count"] == len(large_map["cells"]) == 800000

invalid = subprocess.run(
    [str(MAPPER), "/dev/null", "--fstype", "ntfs", "--cells", "128"],
    text=True,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)
assert invalid.returncode != 0
assert invalid.stderr.strip()

print("authoritative native registry and mapper smoke tests passed")
