#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""All installed native filesystem identifiers must fail closed on malformed media."""

from __future__ import annotations

import os
import random
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("LINUX_DEFRAGGER_BUILD_DIR", ROOT / "build"))

WORKERS = (
    "linux-defragger-ext-worker",
    "linux-defragger-ntfs-worker",
    "linux-defragger-exfat-worker",
    "linux-defragger-xfs-worker",
    "linux-defragger-fat-worker",
    "linux-defragger-apfs-worker",
    "linux-defragger-minix-worker",
    "linux-defragger-btrfs-worker",
    "linux-defragger-affs-worker",
    "linux-defragger-hfsplus-worker",
    "hfs_analyser",
    "linux-defragger-swap-worker",
    "linux-defragger-ufs-worker",
    "linux-defragger-zfs-worker",
    "linux-defragger-sfs-worker",
    "linux-defragger-pfs3-worker",
)


def deterministic_noise(length: int) -> bytes:
    rng = random.Random(0xDEF6A66E)
    return bytes(rng.getrandbits(8) for _ in range(length))


CASES = (
    ("empty", b""),
    ("one-byte", deterministic_noise(1)),
    ("block-minus-one", deterministic_noise(511)),
    ("short-zero", bytes(512)),
    ("block-plus-one", deterministic_noise(513)),
    ("page-minus-one", deterministic_noise(4095)),
    ("all-ff", bytes([0xFF]) * 4096),
    ("page-plus-one", deterministic_noise(4097)),
    ("small-noise", deterministic_noise(64 * 1024)),
    ("deterministic-noise", deterministic_noise(256 * 1024)),
)


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="defragger-malformed-") as directory:
        root = Path(directory)
        for worker_name in WORKERS:
            worker = BUILD / worker_name
            assert worker.is_file() and os.access(worker, os.X_OK), worker
            for case_name, payload in CASES:
                image = root / f"{worker_name}-{case_name}.img"
                image.write_bytes(payload)
                try:
                    completed = subprocess.run(
                        [str(worker), "identify", str(image)],
                        cwd=root,
                        text=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        timeout=5,
                        check=False,
                    )
                except subprocess.TimeoutExpired as exc:
                    raise AssertionError(
                        f"{worker_name} hung on malformed input {case_name}"
                    ) from exc
                assert completed.returncode >= 0, (
                    f"{worker_name} died from signal {-completed.returncode} "
                    f"on malformed input {case_name}"
                )
                assert completed.returncode != 0, (
                    f"{worker_name} accepted malformed input {case_name}: "
                    f"{completed.stdout.strip()}"
                )

    print("native malformed-image fail-closed matrix passed")


if __name__ == "__main__":
    main()
