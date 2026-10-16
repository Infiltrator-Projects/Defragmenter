#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native filesystem workers must be directly executable from any cwd."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("DEFRAGMENTER_BUILD_DIR", ROOT / "build"))


def _run_version(name: str) -> None:
    completed = subprocess.run(
        [str(BUILD / name), "--version"],
        cwd="/",
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=20,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr


def main() -> None:
    for worker in (
        "defragmenter-ext-worker",
        "defragmenter-ntfs-worker",
        "defragmenter-exfat-worker",
        "defragmenter-xfs-worker",
        "defragmenter-fat-worker",
        "defragmenter-affs-worker",
        "defragmenter-hfsplus-worker",
    ):
        _run_version(worker)
    print("native worker entry-point/version tests passed")


if __name__ == "__main__":
    main()
