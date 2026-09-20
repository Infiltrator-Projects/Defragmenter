#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independently construct a bounded APFS image for native qualification."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

BLOCK = 4096
BLOCKS = 16384
XID = 7
DESC_BASE = 1
DESC_BLOCKS = 8
CPM = 1
NX = 2
SPACEMAN = 9
CIB = 32
BITMAP = 33
CONTAINER_OMAP = 40
CONTAINER_OMAP_TREE = 41
VOLUME = 42
VOLUME_OMAP = 43
VOLUME_OMAP_TREE = 44
EXTREF = 45
CATALOG = 46
DATA_A = 512
DATA_B = 520
FS_OID = 200
CATALOG_OID = 300
DSTREAM = 500
SPACEMAN_OID = 100

OBJ_PHYSICAL = 0x40000000
OBJ_EPHEMERAL = 0x80000000
TYPE_BTREE = 2
TYPE_SPACEMAN = 5
TYPE_CIB = 7
TYPE_OMAP = 11
TYPE_CPM = 12
TYPE_FS = 13
TYPE_FSTREE = 14
TYPE_BLOCKREFTREE = 15


def u16(value: int) -> bytes: return struct.pack("<H", value)
def u32(value: int) -> bytes: return struct.pack("<I", value)
def u64(value: int) -> bytes: return struct.pack("<Q", value)


def fletcher(block: bytearray) -> None:
    mod = 0xFFFFFFFF
    sum1 = 0
    sum2 = 0
    for off in range(8, len(block), 4):
        word = struct.unpack_from("<I", block, off)[0]
        sum1 = (sum1 + word) % mod
        sum2 = (sum2 + sum1) % mod
    c1 = mod - ((sum1 + sum2) % mod)
    c2 = mod - ((sum1 + c1) % mod)
    block[:8] = u64((c2 << 32) | c1)


def object_header(block: bytearray, oid: int, obj_type: int, subtype: int = 0) -> None:
    block[8:16] = u64(oid)
    block[16:24] = u64(XID)
    block[24:28] = u32(obj_type)
    block[28:32] = u32(subtype)


def node(paddr: int, oid: int, subtype: int,
         records: list[tuple[bytes, bytes]], *, fixed: bool, virtual: bool = False) -> bytes:
    raw = bytearray(BLOCK)
    object_header(raw, oid, TYPE_BTREE if virtual else OBJ_PHYSICAL | TYPE_BTREE, subtype)
    flags = 0x0001 | 0x0002 | (0x0004 if fixed else 0)
    raw[32:34] = u16(flags)
    raw[34:36] = u16(0)
    raw[36:40] = u32(len(records))
    entry_size = 4 if fixed else 8
    table_len = len(records) * entry_size
    raw[40:42] = u16(0)
    raw[42:44] = u16(table_len)
    key_base = 56 + table_len
    value_base = BLOCK - 40
    key_cursor = key_base
    value_cursor = value_base
    for index, (key, value) in enumerate(records):
        value_cursor -= len(value)
        raw[key_cursor:key_cursor + len(key)] = key
        raw[value_cursor:value_cursor + len(value)] = value
        pos = 56 + index * entry_size
        if fixed:
            raw[pos:pos + 2] = u16(key_cursor - key_base)
            raw[pos + 2:pos + 4] = u16(value_base - value_cursor)
        else:
            raw[pos:pos + 2] = u16(key_cursor - key_base)
            raw[pos + 2:pos + 4] = u16(len(key))
            raw[pos + 4:pos + 6] = u16(value_base - value_cursor)
            raw[pos + 6:pos + 8] = u16(len(value))
        key_cursor += len(key)
    footer = BLOCK - 40
    raw[footer:footer + 4] = u32(0x10 if not virtual else 0)
    raw[footer + 4:footer + 8] = u32(BLOCK)
    raw[footer + 8:footer + 12] = u32(16 if fixed else 0)
    raw[footer + 12:footer + 16] = u32(16 if fixed else 0)
    raw[footer + 16:footer + 20] = u32(max((len(k) for k, _ in records), default=0))
    raw[footer + 20:footer + 24] = u32(max((len(v) for _, v in records), default=0))
    raw[footer + 24:footer + 32] = u64(len(records))
    raw[footer + 32:footer + 40] = u64(1)
    fletcher(raw)
    return bytes(raw)


def omap_object(paddr: int, tree: int) -> bytes:
    raw = bytearray(BLOCK)
    object_header(raw, paddr, OBJ_PHYSICAL | TYPE_OMAP)
    raw[48:56] = u64(tree)
    fletcher(raw)
    return bytes(raw)


def omap_record(oid: int, paddr: int) -> tuple[bytes, bytes]:
    return u64(oid) + u64(XID), u32(0) + u32(BLOCK) + u64(paddr)


def cat_header(obj_id: int, kind: int) -> bytes:
    return u64(obj_id | (kind << 60))


def inode(private_id: int, mode: int) -> bytes:
    value = bytearray(92)
    value[8:16] = u64(private_id)
    value[48:56] = u64(0)
    value[80:82] = u16(mode)
    return bytes(value)


def file_extent(logical: int, paddr: int) -> tuple[bytes, bytes]:
    key = cat_header(DSTREAM, 8) + u64(logical)
    value = u64(BLOCK) + u64(paddr) + u64(0)
    return key, value


def phys_extent(paddr: int) -> tuple[bytes, bytes]:
    key = cat_header(paddr, 2)
    value = u64(1) + u64(DSTREAM) + u32(1)
    return key, value


def build(path: Path, *, corrupt_bitmap: bool = False,
          second_volume: bool = False, sparse: bool = False) -> None:
    image = bytearray(BLOCK * BLOCKS)
    used = {
        0, CPM, NX, SPACEMAN, CIB, BITMAP, CONTAINER_OMAP,
        CONTAINER_OMAP_TREE, VOLUME, VOLUME_OMAP, VOLUME_OMAP_TREE,
        EXTREF, CATALOG, DATA_A, DATA_B,
    }

    nx = bytearray(BLOCK)
    object_header(nx, 1, OBJ_PHYSICAL | 1)
    nx[32:36] = b"NXSB"
    nx[36:40] = u32(BLOCK)
    nx[40:48] = u64(BLOCKS)
    nx[72:88] = bytes.fromhex("00112233445566778899aabbccddeeff")
    nx[96:104] = u64(XID + 1)
    nx[104:108] = u32(DESC_BLOCKS)
    nx[108:112] = u32(8)
    nx[112:120] = u64(DESC_BASE)
    nx[120:128] = u64(9)
    nx[136:140] = u32(0)
    nx[140:144] = u32(2)
    nx[152:160] = u64(SPACEMAN_OID)
    nx[160:168] = u64(CONTAINER_OMAP)
    nx[180:184] = u32(100)
    nx[184:192] = u64(FS_OID)
    if second_volume:
        nx[192:200] = u64(FS_OID + 1)
    fletcher(nx)
    image[0:BLOCK] = nx
    image[NX * BLOCK:(NX + 1) * BLOCK] = nx

    cpm = bytearray(BLOCK)
    object_header(cpm, CPM, OBJ_PHYSICAL | TYPE_CPM)
    cpm[32:36] = u32(1)
    cpm[36:40] = u32(1)
    cpm[40:44] = u32(OBJ_EPHEMERAL | TYPE_SPACEMAN)
    cpm[48:52] = u32(BLOCK)
    cpm[64:72] = u64(SPACEMAN_OID)
    cpm[72:80] = u64(SPACEMAN)
    fletcher(cpm)
    image[CPM * BLOCK:(CPM + 1) * BLOCK] = cpm

    sm = bytearray(BLOCK)
    object_header(sm, SPACEMAN_OID, OBJ_EPHEMERAL | TYPE_SPACEMAN)
    sm[32:36] = u32(BLOCK)
    sm[36:40] = u32(BLOCKS)
    sm[40:44] = u32(1)
    sm[48:56] = u64(BLOCKS)
    sm[56:64] = u64(1)
    sm[64:68] = u32(1)
    sm[68:72] = u32(0)
    sm[80:84] = u32(1024)
    sm[1024:1032] = u64(CIB)
    sm[152:160] = u64(0)
    sm[176:184] = u64(0)
    sm[72:80] = u64(BLOCKS - len(used))
    fletcher(sm)
    image[SPACEMAN * BLOCK:(SPACEMAN + 1) * BLOCK] = sm

    bitmap = bytearray(BLOCK)
    for block in used:
        bitmap[block >> 3] |= 1 << (block & 7)
    if corrupt_bitmap:
        bitmap[DATA_A >> 3] &= ~(1 << (DATA_A & 7))
    image[BITMAP * BLOCK:(BITMAP + 1) * BLOCK] = bitmap

    cib = bytearray(BLOCK)
    object_header(cib, CIB, OBJ_PHYSICAL | TYPE_CIB)
    cib[32:36] = u32(0)
    cib[36:40] = u32(1)
    cib[40:48] = u64(XID)
    cib[48:56] = u64(0)
    cib[56:60] = u32(BLOCKS)
    cib[60:64] = u32(BLOCKS - len(used))
    cib[64:72] = u64(BITMAP)
    fletcher(cib)
    image[CIB * BLOCK:(CIB + 1) * BLOCK] = cib

    image[CONTAINER_OMAP * BLOCK:(CONTAINER_OMAP + 1) * BLOCK] = omap_object(
        CONTAINER_OMAP, CONTAINER_OMAP_TREE
    )
    image[CONTAINER_OMAP_TREE * BLOCK:(CONTAINER_OMAP_TREE + 1) * BLOCK] = node(
        CONTAINER_OMAP_TREE, CONTAINER_OMAP_TREE, TYPE_OMAP,
        [omap_record(FS_OID, VOLUME)], fixed=True
    )

    volume = bytearray(BLOCK)
    object_header(volume, FS_OID, TYPE_FS)
    volume[32:36] = b"APSB"
    volume[36:40] = u32(0)
    volume[128:136] = u64(VOLUME_OMAP)
    volume[136:144] = u64(CATALOG_OID)
    volume[144:152] = u64(EXTREF)
    volume[184:192] = u64(1)
    volume[192:200] = u64(1)
    volume[216:224] = u64(0)
    volume[240:256] = bytes.fromhex("ffeeddccbbaa99887766554433221100")
    volume[264:272] = u64(1)
    fletcher(volume)
    image[VOLUME * BLOCK:(VOLUME + 1) * BLOCK] = volume

    image[VOLUME_OMAP * BLOCK:(VOLUME_OMAP + 1) * BLOCK] = omap_object(
        VOLUME_OMAP, VOLUME_OMAP_TREE
    )
    image[VOLUME_OMAP_TREE * BLOCK:(VOLUME_OMAP_TREE + 1) * BLOCK] = node(
        VOLUME_OMAP_TREE, VOLUME_OMAP_TREE, TYPE_OMAP,
        [omap_record(CATALOG_OID, CATALOG)], fixed=True
    )

    second_paddr = 0 if sparse else DATA_B
    catalog_records = [
        (cat_header(2, 3), inode(2, 0o040755)),
        (cat_header(16, 3), inode(DSTREAM, 0o100644)),
        file_extent(0, DATA_A),
        file_extent(BLOCK, second_paddr),
    ]
    image[CATALOG * BLOCK:(CATALOG + 1) * BLOCK] = node(
        CATALOG, CATALOG_OID, TYPE_FSTREE, catalog_records,
        fixed=False, virtual=True
    )
    image[EXTREF * BLOCK:(EXTREF + 1) * BLOCK] = node(
        EXTREF, EXTREF, TYPE_BLOCKREFTREE,
        [phys_extent(DATA_A), phys_extent(DATA_B)], fixed=False
    )
    image[DATA_A * BLOCK:(DATA_A + 1) * BLOCK] = b"A" * BLOCK
    image[DATA_B * BLOCK:(DATA_B + 1) * BLOCK] = b"B" * BLOCK
    path.write_bytes(image)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("path", type=Path)
    parser.add_argument("--corrupt-bitmap", action="store_true")
    parser.add_argument("--second-volume", action="store_true")
    parser.add_argument("--sparse", action="store_true")
    args = parser.parse_args()
    build(args.path, corrupt_bitmap=args.corrupt_bitmap,
          second_volume=args.second_volume, sparse=args.sparse)


if __name__ == "__main__":
    main()
