#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the authoritative native registry and mapper interface."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("DEFRAGMENTER_BUILD_DIR", str(ROOT / "build")))
MAPPER = BUILD / "defragmenter-mapper"
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
    fat_image = Path(temp_dir) / "fat.img"
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

        mapped = subprocess.run(
            [str(MAPPER), str(fat_image), "--fstype", variant, "--cells", "128"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True,
        )
        allocation = json.loads(mapped.stdout)
        assert allocation["backend_id"] == variant
        assert allocation["data_clusters"] > 0
        assert allocation["cell_count"] == len(allocation["cells"])

        # Generic FAT names are routing hints only. The native parser remains
        # authoritative for the actual FAT width and an explicit wrong width
        # must fail closed.
        for generic in ("vfat", "fat", "msdos"):
            detected = subprocess.run(
                [str(MAPPER), str(fat_image), "--fstype", generic, "--cells", "128"],
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
                str(MAPPER), str(large_image), "--fstype", "vfat",
                "--cells", "800000",
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

print("authoritative native registry and mapper tests passed")
