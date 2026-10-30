#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify shipped C fixtures through real native readers and writers.

Python orchestrates independent processes and corrupts opaque payload bytes;
filesystem creation and complete verification remain in the shipped C code.
"""
from __future__ import annotations

import json
import mmap
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zlib

BUILD = Path(sys.argv[1]).resolve()
FIXTURE = BUILD / "defragmenter-test-media-raw-test"


def run(*args: object, fails: bool = False,
        timeout: int = 240, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    try:
        result = subprocess.run([str(a) for a in args], capture_output=True,
                                text=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired as exc:
        # Preserve bounded native phase/progress evidence when a slow runner
        # times out. Without this, GitHub prints only the command arguments.
        def tail(value: str | bytes | None) -> str:
            if isinstance(value, bytes):
                value = value.decode("utf-8", errors="replace")
            return (value or "")[-8192:]
        raise AssertionError(
            f"native command exceeded {timeout}s: {args}\\n"
            f"stdout tail:\\n{tail(exc.stdout)}\\n"
            f"stderr tail:\\n{tail(exc.stderr)}") from exc
    if (result.returncode != 0) != fails:
        raise AssertionError(f"{args}\n{result.stdout}\n{result.stderr}")
    return result


def mapping(image: Path, fs: str) -> dict:
    # A mapper launched from CTest must never fall back to the production
    # EXT metadata executable and attempt to load /System/Filesystems.
    environment = dict(os.environ)
    if environment.get("IFS_TEST_MODULE_PARENT"):
        metadata_worker = BUILD / "defragmenter-ext-metadata-test-worker"
        if not metadata_worker.is_file() or not os.access(metadata_worker, os.X_OK):
            raise AssertionError(f"missing trusted EXT metadata test worker: {metadata_worker}")
        environment["DEFRAGMENTER_EXT_METADATA_WORKER"] = str(metadata_worker)
    return json.loads(run(BUILD / "defragmenter-mapper", image,
                          "--fstype", fs, "--cells", 8, env=environment).stdout)


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


def reject_unavailable_fat(fs: str, parent: Path) -> None:
    modules = Path(os.environ["IFS_TEST_MODULE_PARENT"]) / "Filesystems"
    with tempfile.TemporaryDirectory(prefix="missing-module-", dir=parent) as temporary:
        work = Path(temporary)
        directory = work / "Filesystems"
        directory.mkdir(mode=0o755)
        module = directory / (fs.upper() + ".so")
        image = work / "unchanged.img"
        # Same geometry as the shipped corpus; sparse data makes refusal cheap.
        size = (255 if fs == "fat12" else 2048) * 1024 * 1024
        marker = b"must survive failed module validation" * 128
        with image.open("wb") as stream:
            stream.truncate(size)
            stream.write(marker)
            stream.seek(size - len(marker))
            stream.write(marker)
        initial = image.stat().st_mtime_ns
        environment = dict(os.environ, IFS_TEST_MODULE_PARENT=str(work))
        wrong = modules / ("FAT16.so" if fs == "fat12" else "FAT12.so")
        for kind in ("missing", "wrong-width", "missing-interface", "untrusted"):
            if module.exists():
                module.unlink()
            if kind != "missing":
                shutil.copyfile(modules / "OFS.so" if kind == "missing-interface" else wrong, module)
                module.chmod(0o666 if kind == "untrusted" else 0o644)
            run(FIXTURE, "recreate", fs, image, fails=True, env=environment)
            assert image.stat().st_mtime_ns == initial, (fs, kind, "target was written")
            with image.open("rb") as stream:
                assert stream.read(len(marker)) == marker
                stream.seek(size - len(marker))
                assert stream.read() == marker


def qualify(fs: str, parent: Path) -> None:
    if fs in ("fat12", "fat16"):
        reject_unavailable_fat(fs, parent)
    with tempfile.TemporaryDirectory(prefix=f"{fs}-", dir=parent) as temporary:
        work = Path(temporary)
        image = work / "fixture.img"
        if fs == "ufs":
            source = work / "source"
            source.mkdir()
            run(FIXTURE, "ufs-source", source)
            run(os.environ.get("DEFRAGMENTER_UFS_MKFS", str(BUILD / "test-media-mkfs-ufs")), "-t", "ffs", "-B", "little",
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
        # The CI workspace contains both production-only and test-only
        # loaders. Select the exact same trusted SO code with the explicitly
        # isolated module root, never fall back to a production loader.
        if fs.startswith("fat"):
            variable, default = "DEFRAGMENTER_FAT_WORKER", "defragmenter-fat-worker"
        elif fs.startswith("ext"):
            variable, default = "DEFRAGMENTER_EXT_WORKER", "defragmenter-ext-worker"
        elif fs == "hfs":
            variable, default = "DEFRAGMENTER_HFS_WORKER", "hfs_analyser"
        else:
            variable, default = f"DEFRAGMENTER_{fs.upper()}_WORKER", f"defragmenter-{fs}-worker"
        worker = Path(os.environ.get(variable, BUILD / default))
        assert worker.is_file() and os.access(worker, os.X_OK), worker
        for operation in ("defrag", "growth-defrag"):
            journal = work / f"{operation}.journal"
            # These full-corpus writes retain the production durability barriers.
            # Allow slower runner storage fifteen minutes per operation while
            # keeping creation, analysis and verification on the shorter budget.
            run(worker, operation, image, "--write", "--confirm", image,
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
