#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression for the first-party classic-HFS native analyser."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("LINUX_DEFRAGGER_BUILD_DIR", ROOT / "build"))
ANALYSER = BUILD / "hfs_analyser"
EXPECTED_EXTENTS = [[10, 1], [20, 1], [30, 1]]


def _be16(buffer: bytearray, offset: int, value: int) -> None:
    struct.pack_into(">H", buffer, offset, value)


def _be32(buffer: bytearray, offset: int, value: int) -> None:
    struct.pack_into(">I", buffer, offset, value)


def _node_record(node: bytearray, index: int, start: int, end: int) -> None:
    _be16(node, 512 - 2 * (index + 1), start)
    _be16(node, 512 - 2 * (index + 2), end)


def _btree_header_node() -> bytes:
    node = bytearray(512)
    node[8] = 1  # header node
    _be16(node, 10, 1)
    start, end = 14, 64
    _node_record(node, 0, start, end)
    _be32(node, start + 10, 1)  # first leaf
    _be32(node, start + 14, 1)  # last leaf
    _be16(node, start + 18, 512)
    _be32(node, start + 22, 2)  # total nodes
    return bytes(node)


def _empty_leaf_node() -> bytes:
    node = bytearray(512)
    node[8] = 0xFF  # leaf node (-1)
    _be16(node, 10, 0)
    return bytes(node)


def _catalog_leaf_node() -> bytes:
    node = bytearray(512)
    node[8] = 0xFF
    _be16(node, 10, 1)
    start, end = 14, 124
    _node_record(node, 0, start, end)
    record = memoryview(node)[start:end]
    record[0] = 6  # catalog key length; padded key occupies 8 bytes
    _be32(node, start + 2, 2)  # parent CNID
    data = start + 8
    node[data] = 2  # file record
    _be32(node, data + 20, 16)  # file CNID
    _be32(node, data + 30, 3 * 512)  # physical data-fork length
    _be32(node, data + 40, 0)  # resource-fork length
    for index, block in enumerate((10, 20, 30)):
        _be16(node, data + 74 + index * 4, block)
        _be16(node, data + 76 + index * 4, 1)
    return bytes(node)


def _make_fragmented_hfs(path: Path) -> None:
    image = bytearray(128 * 1024)
    mdb = memoryview(image)[1024:1024 + 162]
    _be16(image, 1024, 0x4244)
    _be16(image, 1024 + 10, 0x0100)  # cleanly unmounted
    _be16(image, 1024 + 14, 3)  # allocation bitmap sector
    _be16(image, 1024 + 18, 200)  # allocation blocks
    _be32(image, 1024 + 20, 512)  # allocation block size
    _be16(image, 1024 + 28, 4)  # first allocation block in 512-byte sectors
    _be16(image, 1024 + 34, 193)  # free allocation blocks
    _be32(image, 1024 + 84, 1)  # file count
    _be32(image, 1024 + 88, 0)  # directory count (catalog records only here)
    _be32(image, 1024 + 130, 1024)  # extents-overflow file size
    _be16(image, 1024 + 134, 0)
    _be16(image, 1024 + 136, 2)
    _be32(image, 1024 + 146, 1024)  # catalog file size
    _be16(image, 1024 + 150, 2)
    _be16(image, 1024 + 152, 2)
    del mdb

    allocation_base = 4 * 512
    image[allocation_base:allocation_base + 512] = _btree_header_node()
    image[allocation_base + 512:allocation_base + 1024] = _empty_leaf_node()
    catalog_base = allocation_base + 2 * 512
    image[catalog_base:catalog_base + 512] = _btree_header_node()
    image[catalog_base + 512:catalog_base + 1024] = _catalog_leaf_node()

    # Mark the two Extents B-tree blocks, two Catalog B-tree blocks and the
    # three deliberately fragmented payload blocks allocated.
    for block in (0, 1, 2, 3, 10, 20, 30):
        byte = 3 * 512 + block // 8
        image[byte] |= 0x80 >> (block & 7)
    for ordinal, block in enumerate((10, 20, 30)):
        start = allocation_base + block * 512
        image[start:start + 512] = bytes([0x41 + ordinal]) * 512
    path.write_bytes(image)


def _run(*args: object, check: bool = True) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        [str(ANALYSER), *map(str, args)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=30,
    )
    if check:
        assert completed.returncode == 0, (completed.stdout, completed.stderr)
    return completed


def _scan(path: Path) -> dict:
    return json.loads(_run("scan-json", path).stdout)


def _data_extent(path: Path) -> tuple[int, int]:
    data = path.read_bytes()
    allocation_base = 4 * 512
    catalog_leaf = allocation_base + 3 * 512
    record = catalog_leaf + 14
    payload = record + 8
    return struct.unpack_from(">HH", data, payload + 74)


def _payload(path: Path) -> bytes:
    start, count = _data_extent(path)
    data = path.read_bytes()
    base = 4 * 512 + start * 512
    return data[base:base + count * 512]


def _bitmap_used(path: Path, block: int) -> bool:
    data = path.read_bytes()
    return bool(data[3 * 512 + block // 8] & (0x80 >> (block & 7)))


def _mutate(path: Path, operation: str) -> None:
    journal = Path(str(path) + f".{operation}.journal")
    args: list[object] = [
        operation, path, "--write", "--confirm", path, "--journal", journal,
        "--live-updates",
    ]
    if operation == "growth-defrag":
        args += ["--growth-percent", "10"]
    completed = _run(*args)
    assert f'@@RESULT {{"operation":"{operation}","status":"completed"' in completed.stdout
    assert not journal.exists()
    assert not Path(str(journal) + ".hfs-stage").exists()


def _write_recovery_journal(journal: Path, image: Path, stage: Path) -> None:
    header = image.read_bytes()[1024:1536]
    st = image.stat()
    journal.write_text(
        "LINUX-DEFRAGGER-HFS-JOURNAL-1\n"
        f"device={image.resolve()}\n"
        f"target_identity=file:{st.st_dev}:{st.st_ino}\n"
        f"stage={stage}\n"
        "operation=defrag\n"
        "phase=committing\n"
        f"volume_token={hashlib.sha256(header).hexdigest()}\n"
        f"source_sha256={hashlib.sha256(image.read_bytes()).hexdigest()}\n"
        f"stage_sha256={hashlib.sha256(stage.read_bytes()).hexdigest()}\n"
        f"physical_bytes={st.st_size}\n"
        "block_size=512\n"
        "total_blocks=200\n"
        "allocation_start=4\n",
        encoding="utf-8",
    )


def main() -> None:
    assert ANALYSER.is_file() and os.access(ANALYSER, os.X_OK), ANALYSER
    assert not (ROOT / "vendor").exists()
    with tempfile.TemporaryDirectory(prefix="linux-defragger-hfs-test.") as tmp:
        image = Path(tmp) / "fragmented-hfs.img"
        _make_fragmented_hfs(image)
        completed = subprocess.run(
            [str(ANALYSER), "scan-json", str(image)],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        result = json.loads(completed.stdout)
        assert result["files"] == 1
        assert result["directories"] == 0
        assert result["fragmented_files"] == 1
        assert result["fragmented_directories"] == 0
        assert result["fragmented_extents"] == EXPECTED_EXTENTS

        original_payload = (
            bytes([0x41]) * 512 + bytes([0x42]) * 512 + bytes([0x43]) * 512
        )
        assert _payload(image) != original_payload  # fragmented; helper expects contiguous

        _mutate(image, "defrag")
        assert _scan(image)["fragmented_files"] == 0
        assert _payload(image) == original_payload

        growth = Path(tmp) / "growth-hfs.img"
        _make_fragmented_hfs(growth)
        _mutate(growth, "growth-defrag")
        assert _scan(growth)["fragmented_files"] == 0
        start, count = _data_extent(growth)
        reserve = (count * 10 + 99) // 100
        assert all(not _bitmap_used(growth, start + count + offset)
                   for offset in range(reserve))
        assert _payload(growth) == original_payload

        recover = Path(tmp) / "recover-hfs.img"
        _make_fragmented_hfs(recover)
        _mutate(recover, "defrag")
        stage = Path(tmp) / "recover.journal.hfs-stage"
        shutil.copyfile(recover, stage)
        start, _ = _data_extent(recover)
        with recover.open("r+b") as stream:
            stream.seek(4 * 512 + start * 512)
            stream.write(b"Z" * 512)
            stream.flush()
            os.fsync(stream.fileno())
        journal = Path(tmp) / "recover.journal"
        _write_recovery_journal(journal, recover, stage)
        completed = _run(
            "recover", recover, "--write", "--confirm", recover,
            "--journal", journal,
        )
        assert '@@RESULT {"operation":"recover","status":"completed"' in completed.stdout
        assert _payload(recover) == original_payload
        assert not journal.exists() and not stage.exists()

        unclean = Path(tmp) / "unclean-hfs.img"
        _make_fragmented_hfs(unclean)
        data = bytearray(unclean.read_bytes())
        _be16(data, 1024 + 10, 0)
        unclean.write_bytes(data)
        before = hashlib.sha256(unclean.read_bytes()).digest()
        failed = _run(
            "defrag", unclean, "--write", "--confirm", unclean,
            "--journal", Path(tmp) / "unclean.journal", check=False,
        )
        assert failed.returncode != 0
        assert "cleanly unmounted" in failed.stderr
        assert hashlib.sha256(unclean.read_bytes()).digest() == before

    print("Classic HFS native analyser/Defrag/Growth Defrag/Recover regression passed")


if __name__ == "__main__":
    main()
