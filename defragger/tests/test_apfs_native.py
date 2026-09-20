#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end bounded APFS checkpoint/spaceman/catalog qualification."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("LINUX_DEFRAGGER_BUILD_DIR", ROOT / "build"))
WORKER = BUILD / "linux-defragger-apfs-worker"
MAKER = ROOT / "tests" / "make_apfs_fixture.py"


def make(path: Path, *options: str) -> None:
    subprocess.run([sys.executable, str(MAKER), str(path), *options], check=True)


def run(*args: object, check: bool = True) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        [str(WORKER), *(str(arg) for arg in args)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if check:
        assert completed.returncode == 0, (completed.stdout, completed.stderr)
    return completed


def main() -> None:
    assert WORKER.is_file(), WORKER
    with tempfile.TemporaryDirectory(prefix="defragger-apfs-") as directory:
        work = Path(directory)
        image = work / "apfs.img"
        make(image)
        analysis = json.loads(run("analyse-json", image).stdout)
        assert analysis["filesystem"] == "apfs"
        assert analysis["block_size"] == 4096
        assert analysis["block_count"] == 16384
        assert analysis["xid"] == 7
        assert analysis["active_nx_block"] == 2
        assert analysis["spaceman_block"] == 9
        assert analysis["regular_files"] == 1
        assert analysis["directories"] == 1
        assert analysis["fragmented_files"] == 1
        assert analysis["free_blocks"] + analysis["used_blocks"] == analysis["block_count"]

        mapped = json.loads(run("map", image, "--cells", 128).stdout)
        assert mapped["schema"] == 1
        assert mapped["map_accuracy"] == "exact-bounded-spaceman"
        assert mapped["unknown_bytes"] == 0
        assert mapped["free_bytes"] + mapped["used_bytes"] == 16384 * 4096
        assert mapped["fragmented_files"] == 1
        assert any(cell["fragmented"] for cell in mapped["cells"])

        corrupt = work / "corrupt.img"
        make(corrupt, "--corrupt-bitmap")
        failed = run("analyse-json", corrupt, check=False)
        assert failed.returncode != 0
        assert "spaceman" in failed.stderr.lower() or "free" in failed.stderr.lower()

        multi = work / "multi.img"
        make(multi, "--second-volume")
        failed = run("analyse-json", multi, check=False)
        assert failed.returncode != 0
        assert "one volume" in failed.stderr.lower()

        sparse = work / "sparse.img"
        make(sparse, "--sparse")
        failed = run("analyse-json", sparse, check=False)
        assert failed.returncode != 0
        assert "non-sparse" in failed.stderr.lower()

    print("bounded APFS checkpoint/spaceman/catalog tests passed")


if __name__ == "__main__":
    main()
