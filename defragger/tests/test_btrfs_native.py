#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native Btrfs allocation, fragmentation and adapter regression tests."""

from __future__ import annotations

import hashlib
import json
import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("LINUX_DEFRAGGER_BUILD_DIR", ROOT / "build"))
WORKER = Path(os.environ.get(
    "LINUX_DEFRAGGER_BTRFS_WORKER", BUILD / "linux-defragger-btrfs-worker"
))
FIXTURE = ROOT / "tests" / "make_btrfs_fixture.py"
sys.path.insert(0, str(ROOT / "gui"))

from filesystems.btrfs.plugin import BtrfsBackend  # noqa: E402


def run(*args: object, check: bool = True) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        [str(WORKER), *map(str, args)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=30,
    )
    if check:
        assert completed.returncode == 0, (completed.stdout, completed.stderr)
    return completed


def make(path: Path, *options: str) -> None:
    subprocess.run([sys.executable, str(FIXTURE), str(path), *options], check=True)


def test_worker_contract(work: Path) -> None:
    image = work / "valid.img"
    make(image)
    identified = json.loads(run("identify", image).stdout)
    assert identified == {"filesystem": "btrfs"}

    summary = json.loads(run("analyse-json", image).stdout)
    assert summary["filesystem"] == "btrfs"
    assert summary["sector_size"] == 4096
    assert summary["node_size"] == 4096
    assert summary["filesystem_bytes"] == 8 * 1024 * 1024
    assert summary["physical_bytes"] == 10 * 1024 * 1024
    assert summary["regular_files"] == 1
    assert summary["directories"] == 1
    assert summary["fragmented_files"] == 1
    assert summary["fragmented_directories"] == 0
    assert summary["malformed_items"] == 0

    result = json.loads(run("map", image, "--cells", "16").stdout)
    assert result["schema"] == 1
    assert result["backend"] == "read-only-domain"
    assert result["filesystem"] == "btrfs"
    assert result["map_accuracy"] == "exact-single-device"
    assert result["unit_size"] == 4096
    assert result["filesystem_units"] == 2048
    assert result["total_units"] == 2560
    assert result["outside_bytes"] == 2 * 1024 * 1024
    assert result["unknown_bytes"] == 0
    assert result["used_bytes"] == 9 * 4096
    assert result["free_bytes"] == (2048 - 9) * 4096
    assert result["regular_files"] == 1
    assert result["directories"] == 1
    assert result["fragmented_files"] == 1
    assert result["details"]["fragmentation_available"] is True
    assert result["details"]["fragmented_sectors_mapped"] == 2
    assert sum(cell["outside"] for cell in result["cells"]) == 512
    assert sum(cell["fragmented"] for cell in result["cells"]) == 2


def test_python_adapter_is_native_only(work: Path) -> None:
    image = work / "adapter.img"
    make(image)
    old = os.environ.get("LINUX_DEFRAGGER_BTRFS_WORKER")
    os.environ["LINUX_DEFRAGGER_BTRFS_WORKER"] = str(WORKER)
    try:
        backend = BtrfsBackend()
        assert backend.probe(str(image))
        result = backend.map(str(image), 12)
    finally:
        if old is None:
            os.environ.pop("LINUX_DEFRAGGER_BTRFS_WORKER", None)
        else:
            os.environ["LINUX_DEFRAGGER_BTRFS_WORKER"] = old
    assert result["fragmented_files"] == 1
    assert result["outside_bytes"] == 2 * 1024 * 1024

    source = (ROOT / "gui" / "filesystems" / "btrfs" / "plugin.py").read_text()
    for forbidden in (
        "Reader", "u16le", "u32le", "u64le", "bisect", "_TreeReader",
        "_Mapper", "_CHUNK_ITEM", "_EXTENT_ITEM", "_FILE_EXTENT_REG",
        "aggregate_ranges", "complement_ranges", "overlay_ranges",
    ):
        assert forbidden not in source, forbidden
    assert 'resolve_program("btrfs-native"' in source
    assert len(source.splitlines()) < 180


def test_malformed_metadata_fails_closed(work: Path) -> None:
    image = work / "malformed.img"
    make(image, "--malformed")
    completed = run("map", image, "--cells", "8", check=False)
    assert completed.returncode != 0
    assert "overlaps its item table" in completed.stderr


def test_unsupported_layouts_fail_closed(work: Path) -> None:
    image = work / "multi.img"
    make(image, "--multi-device")
    completed = run("map", image, "--cells", "8", check=False)
    assert completed.returncode != 0
    assert "single-device" in completed.stderr

    image = work / "striped.img"
    make(image, "--striped")
    completed = run("map", image, "--cells", "8", check=False)
    assert completed.returncode != 0
    assert "striped Btrfs profiles" in completed.stderr




def payload(path: Path) -> bytes:
    data = path.read_bytes()
    return b"A" * 4096 + b"B" * 4096


def mutate(path: Path, operation: str) -> subprocess.CompletedProcess[str]:
    journal = Path(str(path) + f".{operation}.journal")
    args: list[object] = [
        operation, path, "--write", "--confirm", path, "--journal", journal,
        "--live-updates",
    ]
    if operation == "growth-defrag":
        args += ["--growth-percent", "10"]
    completed = run(*args)
    assert f'@@RESULT {{"operation":"{operation}","status":"completed"' in completed.stdout
    assert not journal.exists()
    assert not Path(str(journal) + ".btrfs-stage").exists()
    return completed


def test_native_writer(work: Path) -> None:
    image = work / "defrag.img"
    make(image)
    mutate(image, "defrag")
    summary = json.loads(run("analyse-json", image).stdout)
    assert summary["fragmented_files"] == 0
    assert image.read_bytes().count(b"A" * 4096) >= 1
    assert image.read_bytes().count(b"B" * 4096) >= 1

    growth = work / "growth.img"
    make(growth)
    mutate(growth, "growth-defrag")
    assert json.loads(run("analyse-json", growth).stdout)["fragmented_files"] == 0

    encoded = work / "encoded.img"
    make(encoded, "--encoded")
    before = hashlib.sha256(encoded.read_bytes()).digest()
    failed = run(
        "defrag", encoded, "--write", "--confirm", encoded,
        "--journal", work / "encoded.journal", check=False,
    )
    assert failed.returncode != 0
    assert "unencoded regular file extents" in failed.stderr
    assert hashlib.sha256(encoded.read_bytes()).digest() == before


def test_recovery(work: Path) -> None:
    image = work / "recover.img"
    make(image)
    journal = work / "recover.journal"
    image.chmod(stat.S_IRUSR)
    failed = run(
        "defrag", image, "--write", "--confirm", image,
        "--journal", journal, check=False,
    )
    image.chmod(stat.S_IRUSR | stat.S_IWUSR)
    assert failed.returncode != 0
    stage = Path(str(journal) + ".btrfs-stage")
    assert journal.exists() and stage.exists()
    recovered = run(
        "recover", image, "--write", "--confirm", image,
        "--journal", journal,
    )
    assert '@@RESULT {"operation":"recover","status":"completed"' in recovered.stdout
    assert not journal.exists() and not stage.exists()
    assert json.loads(run("analyse-json", image).stdout)["fragmented_files"] == 0


def main() -> None:
    assert WORKER.is_file(), f"missing native Btrfs worker: {WORKER}"
    with tempfile.TemporaryDirectory(prefix="linux-defragger-btrfs-") as directory:
        work = Path(directory)
        test_worker_contract(work)
        test_python_adapter_is_native_only(work)
        test_malformed_metadata_fails_closed(work)
        test_unsupported_layouts_fail_closed(work)
        test_native_writer(work)
        test_recovery(work)
    print("native Btrfs allocation/fragmentation/writer/recovery tests passed")


if __name__ == "__main__":
    main()
