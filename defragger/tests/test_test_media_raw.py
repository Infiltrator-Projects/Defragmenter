#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify shipped C fixtures through real native readers and writers.

Python orchestrates independent processes and corrupts opaque payload bytes;
filesystem creation and complete verification remain in the shipped C code.
"""
from __future__ import annotations

import json
import mmap
from pathlib import Path
import subprocess
import sys
import tempfile
import zlib

BUILD = Path(sys.argv[1]).resolve()
FIXTURE = BUILD / "defragmenter-test-media-raw-test"


def run(*args: object, fails: bool = False,
        timeout: int = 240) -> subprocess.CompletedProcess[str]:
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True,
                            timeout=timeout)
    if (result.returncode != 0) != fails:
        raise AssertionError(f"{args}\n{result.stdout}\n{result.stderr}")
    return result


def mapping(image: Path, fs: str) -> dict:
    return json.loads(run(BUILD / "defragmenter-mapper", image,
                          "--fstype", fs, "--cells", 8).stdout)


def opaque_marker(image: Path) -> bytes:
    # Creation starts from a zero-filled sparse image. Incompressible pages
    # identify opaque test payload without importing a filesystem layout or
    # manufacturing a second expected-data implementation in this test.
    with image.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            for start in range(0, len(chunk), 65536):
                page = chunk[start:start + 65536]
                if len(page) == 65536 and len(zlib.compress(page)) > 65500:
                    return page
    raise AssertionError("no opaque payload page in newly created fixture")


def reject_corruption(image: Path, fs: str, marker: bytes) -> None:
    # Relocation can leave stale source copies. Damage every occurrence so
    # that the test necessarily reaches the live file, then restore exactly.
    with image.open("r+b") as stream, mmap.mmap(stream.fileno(), 0) as data:
        positions = []
        at = data.find(marker)
        while at >= 0:
            positions.append(at + 37)
            at = data.find(marker, at + len(marker))
        assert positions, fs
        for at in positions:
            data[at] ^= 0x40
        data.flush()
        run(FIXTURE, "verify", fs, image, fails=True)
        for at in positions:
            data[at] ^= 0x40
        data.flush()


def qualify(fs: str, parent: Path) -> None:
    with tempfile.TemporaryDirectory(prefix=f"{fs}-", dir=parent) as temporary:
        work = Path(temporary)
        image = work / "fixture.img"
        if fs == "ufs":
            source = work / "source"
            source.mkdir()
            run(FIXTURE, "ufs-source", source)
            run(BUILD / "test-media-mkfs-ufs", "-t", "ffs", "-B", "little",
                "-s", "2047m", "-o", "version=2,bsize=8192,fsize=1024,minfree=5",
                image, source)
        else:
            run(FIXTURE, "create", fs, image)
        run(FIXTURE, "verify", fs, image)
        before = mapping(image, fs)
        accuracy = "exact-single-device" if fs == "btrfs" else "exact"
        assert before["map_accuracy"] == accuracy, before
        assert before.get("unknown_bytes", 0) == 0, before
        if fs == "swap":
            print("swap: native header creation and exact page-map verification passed", flush=True)
            return
        marker = opaque_marker(image)
        assert before["fragmented_files"] >= 8, before
        if fs == "zfs":
            reject_corruption(image, fs, marker)
            print("zfs: native creation, exact analysis, every payload byte and corruption rejection passed", flush=True)
            return  # ZFS is analysis-only by design.
        assert before["defrag_qualified"] and before["growth_qualified"], before
        worker = ("hfs_analyser" if fs == "hfs" else
                  "defragmenter-fat-worker" if fs.startswith("fat") else
                  "defragmenter-ext-worker" if fs.startswith("ext") else
                  f"defragmenter-{fs}-worker")
        for operation in ("defrag", "growth-defrag"):
            journal = work / f"{operation}.journal"
            # These full-corpus writes retain the production durability barriers.
            # Allow slower runner storage fifteen minutes per operation while
            # keeping creation, analysis and verification on the shorter budget.
            run(BUILD / worker, operation, image, "--write", "--confirm", image,
                "--journal", journal, timeout=900)
            assert not journal.exists(), (fs, operation)
            run(FIXTURE, "verify", fs, image)
            after = mapping(image, fs)
            assert after["fragmented_files"] == 0, after
            assert after["fragmented_directories"] == 0, after
        reject_corruption(image, fs, marker)
        print(f"{fs}: native creation, both relocation modes, every payload byte and corruption rejection passed", flush=True)


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_test_media_raw.py <build-directory> <filesystem>")
    fs = sys.argv[2]
    run(FIXTURE)
    with tempfile.TemporaryDirectory(prefix="defragmenter-native-fixtures-") as temporary:
        qualify(fs, Path(temporary))
    print(f"{fs}: native Test Media qualification complete", flush=True)


if __name__ == "__main__":
    main()
