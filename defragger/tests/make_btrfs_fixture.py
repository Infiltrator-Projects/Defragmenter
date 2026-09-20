#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a deterministic small single-device Btrfs writer fixture."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

SECTOR = 4096
NODE = 4096
IMAGE_SIZE = 10 * 1024 * 1024
FILESYSTEM_SIZE = 8 * 1024 * 1024
SUPER = 64 * 1024

SYSTEM_LOGICAL = 1 * 1024 * 1024
SYSTEM_LENGTH = 1 * 1024 * 1024
MIXED_LOGICAL = 2 * 1024 * 1024
MIXED_LENGTH = 4 * 1024 * 1024

CHUNK_TREE = SYSTEM_LOGICAL
ROOT_TREE = MIXED_LOGICAL
EXTENT_TREE = MIXED_LOGICAL + NODE
FS_TREE = MIXED_LOGICAL + 2 * NODE
DEV_TREE = MIXED_LOGICAL + 3 * NODE
CSUM_TREE = MIXED_LOGICAL + 4 * NODE
DATA1 = 3 * 1024 * 1024
DATA2 = DATA1 + 2 * SECTOR

FSID = bytes.fromhex("00112233445566778899aabbccddeeff")
CHUNK_UUID = bytes.fromhex("102132435465768798a9bacbdcedfe0f")
DEV_UUID = bytes.fromhex("ffeeddccbbaa99887766554433221100")

BTRFS_EXTENT_FLAG_DATA = 1
BTRFS_EXTENT_FLAG_TREE = 2
BTRFS_TREE_BLOCK_REF_KEY = 176
BTRFS_EXTENT_DATA_REF_KEY = 178
BTRFS_INODE_NODATASUM = 1
BTRFS_INCOMPAT_MIXED_GROUPS = 1 << 2
BTRFS_INCOMPAT_SKINNY_METADATA = 1 << 8


def le16(value: int) -> bytes:
    return struct.pack("<H", value)


def le32(value: int) -> bytes:
    return struct.pack("<I", value)


def le64(value: int) -> bytes:
    return struct.pack("<Q", value)


def crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return (~crc) & 0xFFFFFFFF


def checksum(block: bytearray) -> None:
    block[:32] = bytes(32)
    block[:4] = le32(crc32c(bytes(block[32:])))


def key(objectid: int, item_type: int, offset: int) -> bytes:
    return le64(objectid) + bytes([item_type]) + le64(offset)


def chunk_data(logical: int, length: int, kind: int, *, striped: bool = False) -> bytes:
    data = bytearray(48 + 32)
    data[0:8] = le64(length)
    data[8:16] = le64(3)
    data[16:24] = le64(64 * 1024)
    data[24:32] = le64((8 if striped else 0) | kind)
    data[32:36] = le32(SECTOR)
    data[36:40] = le32(SECTOR)
    data[40:44] = le32(SECTOR)
    data[44:46] = le16(1)
    data[48:56] = le64(1)
    data[56:64] = le64(logical)
    data[64:80] = DEV_UUID
    return bytes(data)


def root_item(bytenr: int) -> bytes:
    data = bytearray(247)
    data[160:168] = le64(1)
    data[176:184] = le64(bytenr)
    data[216:220] = le32(1)
    data[238] = 0
    data[239:247] = le64(1)
    return bytes(data)


def inode_item(mode: int, *, nodatasum: bool = False) -> bytes:
    data = bytearray(160)
    data[52:56] = le32(mode)
    data[64:72] = le64(BTRFS_INODE_NODATASUM if nodatasum else 0)
    return bytes(data)


def file_extent(disk_bytenr: int, *, encoded: bool = False) -> bytes:
    data = bytearray(53)
    data[0:8] = le64(1)
    data[8:16] = le64(SECTOR)
    data[16] = 1 if encoded else 0
    data[20] = 1
    data[21:29] = le64(disk_bytenr)
    data[29:37] = le64(SECTOR)
    data[37:45] = le64(0)
    data[45:53] = le64(SECTOR)
    return bytes(data)


def metadata_extent(owner: int) -> bytes:
    data = bytearray(33)
    data[0:8] = le64(1)
    data[8:16] = le64(1)
    data[16:24] = le64(BTRFS_EXTENT_FLAG_TREE)
    data[24] = BTRFS_TREE_BLOCK_REF_KEY
    data[25:33] = le64(owner)
    return bytes(data)


def data_extent(inode: int, file_offset: int) -> bytes:
    data = bytearray(53)
    data[0:8] = le64(1)
    data[8:16] = le64(1)
    data[16:24] = le64(BTRFS_EXTENT_FLAG_DATA)
    data[24] = BTRFS_EXTENT_DATA_REF_KEY
    data[25:33] = le64(5)
    data[33:41] = le64(inode)
    data[41:49] = le64(file_offset)
    data[49:53] = le32(1)
    return bytes(data)


def block_group(used: int, flags: int) -> bytes:
    return le64(used) + le64(256) + le64(flags)


def leaf(bytenr: int, owner: int, records: list[tuple[bytes, bytes]]) -> bytes:
    raw = bytearray(NODE)
    raw[32:48] = FSID
    raw[48:56] = le64(bytenr)
    raw[64:80] = CHUNK_UUID
    raw[80:88] = le64(1)
    raw[88:96] = le64(owner)
    records = sorted(records, key=lambda item: item[0])
    raw[96:100] = le32(len(records))
    raw[100] = 0
    table_end = 101 + 25 * len(records)
    cursor = NODE
    for index, (item_key, data) in enumerate(records):
        cursor -= len(data)
        if cursor < table_end:
            raise ValueError("fixture leaf overflow")
        raw[cursor:cursor + len(data)] = data
        pos = 101 + 25 * index
        raw[pos:pos + 17] = item_key
        raw[pos + 17:pos + 21] = le32(cursor - 101)
        raw[pos + 21:pos + 25] = le32(len(data))
    checksum(raw)
    return bytes(raw)


def build(path: Path, *, malformed: bool = False, multi_device: bool = False,
          striped: bool = False, encoded: bool = False) -> None:
    image = bytearray(IMAGE_SIZE)
    system_chunk = chunk_data(SYSTEM_LOGICAL, SYSTEM_LENGTH, 2)
    mixed_chunk = chunk_data(MIXED_LOGICAL, MIXED_LENGTH, 5, striped=striped)
    system = key(256, 228, SYSTEM_LOGICAL) + system_chunk

    superblock = bytearray(4096)
    superblock[32:48] = FSID
    superblock[48:56] = le64(SUPER)
    superblock[0x40:0x48] = b"_BHRfS_M"
    superblock[72:80] = le64(1)
    superblock[80:88] = le64(ROOT_TREE)
    superblock[88:96] = le64(CHUNK_TREE)
    superblock[96:104] = le64(0)
    superblock[112:120] = le64(FILESYSTEM_SIZE)
    superblock[120:128] = le64(8 * SECTOR)
    superblock[136:144] = le64(2 if multi_device else 1)
    superblock[144:148] = le32(SECTOR)
    superblock[148:152] = le32(NODE)
    superblock[156:160] = le32(SECTOR)
    superblock[160:164] = le32(len(system))
    superblock[164:172] = le64(1)
    superblock[188:196] = le64(
        BTRFS_INCOMPAT_MIXED_GROUPS | BTRFS_INCOMPAT_SKINNY_METADATA
    )
    superblock[196:198] = le16(0)
    superblock[198] = 0
    superblock[199] = 0
    superblock[201:209] = le64(1)
    superblock[209:217] = le64(FILESYSTEM_SIZE)
    superblock[217:225] = le64(8 * SECTOR)
    superblock[233:237] = le32(SECTOR)
    superblock[245:253] = le64(1)
    superblock[267:283] = DEV_UUID
    superblock[283:299] = FSID
    superblock[555:563] = le64(0xFFFFFFFFFFFFFFFF)
    superblock[811:811 + len(system)] = system
    checksum(superblock)
    image[SUPER:SUPER + len(superblock)] = superblock

    image[CHUNK_TREE:CHUNK_TREE + NODE] = leaf(
        CHUNK_TREE, 3,
        [
            (key(256, 228, SYSTEM_LOGICAL), system_chunk),
            (key(256, 228, MIXED_LOGICAL), mixed_chunk),
        ],
    )
    image[ROOT_TREE:ROOT_TREE + NODE] = leaf(
        ROOT_TREE, 1,
        [
            (key(2, 132, 1), root_item(EXTENT_TREE)),
            (key(4, 132, 1), root_item(DEV_TREE)),
            (key(5, 132, 1), root_item(FS_TREE)),
            (key(7, 132, 1), root_item(CSUM_TREE)),
        ],
    )
    image[EXTENT_TREE:EXTENT_TREE + NODE] = leaf(
        EXTENT_TREE, 2,
        [
            (key(CHUNK_TREE, 169, 0), metadata_extent(3)),
            (key(ROOT_TREE, 169, 0), metadata_extent(1)),
            (key(EXTENT_TREE, 169, 0), metadata_extent(2)),
            (key(FS_TREE, 169, 0), metadata_extent(5)),
            (key(DEV_TREE, 169, 0), metadata_extent(4)),
            (key(CSUM_TREE, 169, 0), metadata_extent(7)),
            (key(DATA1, 168, SECTOR), data_extent(256, 0)),
            (key(DATA2, 168, SECTOR), data_extent(256, SECTOR)),
            (key(SYSTEM_LOGICAL, 192, SYSTEM_LENGTH),
             block_group(SECTOR, 2)),
            (key(MIXED_LOGICAL, 192, MIXED_LENGTH),
             block_group(7 * SECTOR, 5)),
        ],
    )
    image[FS_TREE:FS_TREE + NODE] = leaf(
        FS_TREE, 5,
        [
            (key(256, 1, 0), inode_item(0o100644, nodatasum=True)),
            (key(256, 108, 0), file_extent(DATA1, encoded=encoded)),
            (key(256, 108, SECTOR), file_extent(DATA2)),
            (key(257, 1, 0), inode_item(0o040755)),
        ],
    )
    image[DEV_TREE:DEV_TREE + NODE] = leaf(DEV_TREE, 4, [])
    image[CSUM_TREE:CSUM_TREE + NODE] = leaf(CSUM_TREE, 7, [])

    image[DATA1:DATA1 + SECTOR] = b"A" * SECTOR
    image[DATA2:DATA2 + SECTOR] = b"B" * SECTOR

    if malformed:
        first_item = ROOT_TREE + 101
        image[first_item + 17:first_item + 21] = le32(0)

    path.write_bytes(image)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("path", type=Path)
    parser.add_argument("--malformed", action="store_true")
    parser.add_argument("--multi-device", action="store_true")
    parser.add_argument("--striped", action="store_true")
    parser.add_argument("--encoded", action="store_true")
    args = parser.parse_args()
    build(args.path, malformed=args.malformed, multi_device=args.multi_device,
          striped=args.striped, encoded=args.encoded)


if __name__ == "__main__":
    main()
