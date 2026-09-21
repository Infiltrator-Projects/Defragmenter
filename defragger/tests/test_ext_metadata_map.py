#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify EXT filesystem metadata is mapped separately from ordinary allocation."""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("LINUX_DEFRAGGER_BUILD_DIR", ROOT / "build"))


def run_json(program: Path, *arguments: str) -> dict:
    completed = subprocess.run(
        [str(program), *arguments],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return json.loads(completed.stdout)


def ranges_overlap(left: tuple[int, int], right: tuple[int, int]) -> bool:
    return max(left[0], right[0]) < min(left[1], right[1])


def main() -> None:
    primary = BUILD / "linux-defragger-ext-worker"
    metadata = BUILD / "linux-defragger-ext-metadata-worker"
    mapper = BUILD / "linux-defragger-mapper"
    fixture = BUILD / "linux-defragger-ext-fixture"
    for program in (primary, metadata, mapper, fixture):
        assert program.is_file(), f"missing native EXT test executable: {program}"

    os.environ["LINUX_DEFRAGGER_EXT_WORKER"] = str(primary)
    os.environ["LINUX_DEFRAGGER_EXT_METADATA_WORKER"] = str(metadata)

    with tempfile.TemporaryDirectory(prefix="linux-defragger-ext-map-") as directory:
        image = Path(directory) / "ext4.img"
        subprocess.run([str(fixture), str(image)], check=True)

        allocation = run_json(primary, "analyse-json", str(image))
        classified = run_json(metadata, "analyse-json", str(image))
        assert classified["block_size"] == allocation["block_size"]
        assert classified["total_blocks"] == allocation["total_blocks"]

        free_ranges = [tuple(item) for item in allocation["free_ranges"]]
        metadata_ranges = [tuple(item) for item in classified["metadata_ranges"]]
        assert metadata_ranges, "EXT classifier returned no filesystem metadata"
        assert len(metadata_ranges) >= 3, metadata_ranges
        for reserved in metadata_ranges:
            assert reserved[0] < reserved[1]
            assert all(
                not ranges_overlap(reserved, free_range)
                for free_range in free_ranges
            ), (reserved, free_ranges)

        expected_metadata_blocks = sum(end - start for start, end in metadata_ranges)
        mapped = run_json(
            mapper, str(image), "--fstype", "ext4", "--cells", "8192"
        )
        metadata_cells = [cell for cell in mapped["cells"] if int(cell.get("bad", 0))]
        assert metadata_cells, "EXT GUI map did not expose metadata/reserved cells"
        mapped_metadata_blocks = sum(int(cell["bad"]) for cell in mapped["cells"])
        assert mapped_metadata_blocks == expected_metadata_blocks
        assert int(mapped["details"]["metadata_blocks_mapped"]) == expected_metadata_blocks
        assert mapped["details"]["metadata_basis"].startswith("native C EXT")
        assert all(int(cell["bad"]) <= int(cell["used"]) for cell in mapped["cells"])

    mapper_source = (ROOT / "native" / "map.cpp").read_text()
    assert '"ext-metadata"' in mapper_source
    assert 'overlay_ranges(result.at("cells").array()' in mapper_source
    assert '"bad"' in mapper_source
    print("EXT metadata/reserved allocation-map classification tests passed")


if __name__ == "__main__":
    main()
