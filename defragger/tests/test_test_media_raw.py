#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify shipped C fixtures through real native readers and writers.

Python orchestrates independent processes and corrupts opaque payload bytes;
filesystem creation and complete verification remain in the shipped C code.
"""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

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
        # The production C host is the sole filesystem authority. Require
        # complete extent and payload evidence, never an obsolete worker JSON
        # schema or an exact map fabricated from an incomplete catalogue.
        run(FIXTURE, "qualify-create", fs, image, timeout=900)
        run(FIXTURE, "verify", fs, image, timeout=900)
        if fs != "swap":
            run(FIXTURE, "verify-corruption", fs, image, timeout=900)
        if fs not in ("swap", "zfs"):
            run(FIXTURE, "qualify-defrag", fs, image, timeout=900)
            growth = work / "growth.img"
            run(FIXTURE, "qualify-create", fs, growth, timeout=900)
            run(FIXTURE, "qualify-growth", fs, growth, timeout=900)
        print(f"{fs}: Common V2 corpus and applicable mutation qualification passed", flush=True)


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
