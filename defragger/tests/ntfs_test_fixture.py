#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Linux Defragger
# Author: Shannon Smith
# Purpose: Verify native NTFS pack, defragment, bitmap updates and recovery.

from __future__ import annotations

import contextlib
import hashlib
import io
import os
import struct
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "gui"))

BPS = 512
SPC = 1
CLUSTER_SIZE = 512
RECORD_SIZE = 1024
TOTAL_CLUSTERS = 4096
MFT_LCN = 4
MFT_RECORDS = 32
MFT_CLUSTERS = MFT_RECORDS * RECORD_SIZE // CLUSTER_SIZE
BITMAP_LCN = 100
DATA_LCN = 3500
DATA_CLUSTERS = 16
BLOCKER_LCN = 3800
FIXED_USER_LCN = 3650
FIXED_USER_CLUSTERS = 3
ADS_LCN = 3560
ADS_SECOND_LCN = 3600
ADS_CLUSTERS = 4
MIRROR_CLUSTERS = 4 * RECORD_SIZE // CLUSTER_SIZE


def runlist(runs: list[tuple[int, int]]) -> bytes:
    output = bytearray()
    previous = 0
    for lcn, length in runs:
        length_bytes = length.to_bytes(max(1, (length.bit_length() + 7) // 8), "little")
        delta = lcn - previous
        for size in range(1, 9):
            try:
                offset_bytes = delta.to_bytes(size, "little", signed=True)
            except OverflowError:
                continue
            if int.from_bytes(offset_bytes, "little", signed=True) == delta:
                break
        output.append((len(offset_bytes) << 4) | len(length_bytes))
        output += length_bytes + offset_bytes
        previous = lcn
    output.append(0)
    return bytes(output)


def nonresident(atype: int, runs: list[tuple[int, int]], data_size: int,
                mapping_slack: int = 0, name: str = "") -> bytes:
    mapping = runlist(runs)
    name_bytes = name.encode("utf-16le")
    run_offset = (64 + len(name_bytes) + 7) & ~7
    length = (run_offset + len(mapping) + mapping_slack + 7) & ~7
    attr = bytearray(length)
    struct.pack_into("<I", attr, 0, atype)
    struct.pack_into("<I", attr, 4, length)
    attr[8] = 1
    attr[9] = len(name)
    if name_bytes:
        struct.pack_into("<H", attr, 10, 64)
        attr[64:64 + len(name_bytes)] = name_bytes
    clusters = sum(size for _lcn, size in runs)
    struct.pack_into("<Q", attr, 16, 0)
    struct.pack_into("<Q", attr, 24, clusters - 1)
    struct.pack_into("<H", attr, 32, run_offset)
    struct.pack_into("<Q", attr, 40, clusters * CLUSTER_SIZE)
    struct.pack_into("<Q", attr, 48, data_size)
    struct.pack_into("<Q", attr, 56, data_size)
    attr[run_offset:run_offset + len(mapping)] = mapping
    return bytes(attr)


def resident(atype: int, value: bytes) -> bytes:
    length = (24 + len(value) + 7) & ~7
    attr = bytearray(length)
    struct.pack_into("<I", attr, 0, atype)
    struct.pack_into("<I", attr, 4, length)
    struct.pack_into("<I", attr, 16, len(value))
    struct.pack_into("<H", attr, 20, 24)
    attr[24:24 + len(value)] = value
    return bytes(attr)


def record(number: int, attrs: list[bytes], flags: int = 1) -> bytes:
    fixed = bytearray(RECORD_SIZE)
    fixed[:4] = b"FILE"
    struct.pack_into("<H", fixed, 4, 0x30)
    struct.pack_into("<H", fixed, 6, 3)
    struct.pack_into("<H", fixed, 16, 1)
    struct.pack_into("<H", fixed, 18, 1)
    struct.pack_into("<H", fixed, 20, 0x38)
    struct.pack_into("<H", fixed, 22, flags)
    struct.pack_into("<I", fixed, 28, RECORD_SIZE)
    struct.pack_into("<I", fixed, 44, number)
    pos = 0x38
    for attr in attrs:
        fixed[pos:pos + len(attr)] = attr
        pos += len(attr)
    struct.pack_into("<I", fixed, pos, 0xFFFFFFFF)
    pos += 8
    struct.pack_into("<I", fixed, 24, pos)
    # The engine's writer is used here so the synthetic record follows exactly
    # the same update-sequence layout as a real on-disk record.
    fixed[0x30:0x36] = b"\x01\x00\x00\x00\x00\x00"
    usn = b"\xA5\x5A"
    fixed[0x30:0x32] = usn
    fixed[0x32:0x34] = fixed[BPS - 2:BPS]
    fixed[0x34:0x36] = fixed[RECORD_SIZE - 2:RECORD_SIZE]
    fixed[BPS - 2:BPS] = usn
    fixed[RECORD_SIZE - 2:RECORD_SIZE] = usn
    return bytes(fixed)


def make_image(path: Path, volume_flags: int = 0, high_mftmirr_blocker: bool = False,
               split_destinations: bool = False,
               fragmented_data: bool = False,
               occupied_tail: bool = False,
               directory_data: bool = False,
               fixed_attribute_list_stream: bool = False,
               named_ads: bool = False) -> bytes:
    image = bytearray(TOTAL_CLUSTERS * CLUSTER_SIZE)
    boot = memoryview(image)[:BPS]
    boot[0:3] = b"\xeb\x52\x90"
    boot[3:11] = b"NTFS    "
    struct.pack_into("<H", boot, 11, BPS)
    boot[13] = SPC
    struct.pack_into("<Q", boot, 40, TOTAL_CLUSTERS)
    struct.pack_into("<Q", boot, 48, MFT_LCN)
    mirror_lcn = BLOCKER_LCN if high_mftmirr_blocker else 3000
    struct.pack_into("<Q", boot, 56, mirror_lcn)
    boot[64] = 0xF6
    boot[68] = 1
    boot[72:80] = bytes.fromhex("0123456789abcdef")
    boot[510:512] = b"\x55\xaa"

    records = [bytes(RECORD_SIZE) for _ in range(MFT_RECORDS)]
    records[0] = record(0, [nonresident(0x80, [(MFT_LCN, MFT_CLUSTERS)], MFT_RECORDS * RECORD_SIZE)])
    volume_info = bytearray(12)
    volume_info[8] = 3
    volume_info[9] = 1
    struct.pack_into("<H", volume_info, 10, volume_flags)
    if high_mftmirr_blocker:
        records[1] = record(1, [nonresident(0x80, [(mirror_lcn, MIRROR_CLUSTERS)], 4 * RECORD_SIZE)])
    records[3] = record(3, [resident(0x70, bytes(volume_info))])
    records[6] = record(6, [nonresident(0x80, [(BITMAP_LCN, 1)], (TOTAL_CLUSTERS + 7) // 8)])
    data_runs = ([(DATA_LCN, DATA_CLUSTERS // 2),
                  (DATA_LCN + 32, DATA_CLUSTERS // 2)]
                 if fragmented_data else [(DATA_LCN, DATA_CLUSTERS)])
    user_attrs = [nonresident(
        0x80, data_runs, DATA_CLUSTERS * CLUSTER_SIZE,
        mapping_slack=0,
    )]
    if named_ads:
        user_attrs.append(nonresident(
            0x80,
            [(ADS_LCN, ADS_CLUSTERS // 2),
             (ADS_SECOND_LCN, ADS_CLUSTERS // 2)],
            ADS_CLUSTERS * CLUSTER_SIZE,
            mapping_slack=32,
            name="DefragADS",
        ))
    records[24] = record(24, user_attrs)
    if fixed_attribute_list_stream:
        # Model the real-world case that revision 90 rejected: a user record
        # has an $ATTRIBUTE_LIST, but its one unnamed $DATA segment is already
        # physically contiguous. The native canonical writer cannot rewrite
        # that record yet, but it can safely preserve the stream byte-for-byte
        # as a fixed obstacle while compacting supported streams around it.
        records[29] = record(29, [
            resident(0x20, bytes(32)),
            nonresident(0x80, [(FIXED_USER_LCN, FIXED_USER_CLUSTERS)],
                        FIXED_USER_CLUSTERS * CLUSTER_SIZE),
        ])
    directory_lcn = 3720
    directory_clusters = 4
    if directory_data:
        records[25] = record(25, [nonresident(
            0xA0,
            [(directory_lcn, directory_clusters)],
            directory_clusters * CLUSTER_SIZE,
            mapping_slack=32,
        )], flags=0x0001 | 0x0002)
    mft_offset = MFT_LCN * CLUSTER_SIZE
    for number, raw in enumerate(records):
        image[mft_offset + number * RECORD_SIZE:mft_offset + (number + 1) * RECORD_SIZE] = raw
    mirror_offset = mirror_lcn * CLUSTER_SIZE
    for number in range(4):
        image[mirror_offset + number * RECORD_SIZE:mirror_offset + (number + 1) * RECORD_SIZE] = records[number]

    bitmap = bytearray((TOTAL_CLUSTERS + 7) // 8)
    # Reserve the boot area independently so Pack does not encounter an
    # artificial three-cluster hole before the synthetic MFT.
    used = set(range(0, MFT_LCN + MFT_CLUSTERS))
    used.add(BITMAP_LCN)
    for lcn, length in data_runs:
        used.update(range(lcn, lcn + length))
    if named_ads:
        used.update(range(ADS_LCN, ADS_LCN + ADS_CLUSTERS // 2))
        used.update(range(ADS_SECOND_LCN,
                          ADS_SECOND_LCN + ADS_CLUSTERS // 2))
    if fixed_attribute_list_stream:
        used.update(range(FIXED_USER_LCN, FIXED_USER_LCN + FIXED_USER_CLUSTERS))
    if directory_data:
        used.update(range(directory_lcn, directory_lcn + directory_clusters))
    if split_destinations:
        holes = set(range(200, 204)) | set(range(500, 505)) | set(range(1000, 1007))
        used.update(cluster for cluster in range(DATA_LCN) if cluster not in holes)
    if high_mftmirr_blocker:
        used.update(range(mirror_lcn, mirror_lcn + MIRROR_CLUSTERS))
    if occupied_tail:
        # Simulate a volume whose physical tail is completely occupied while
        # large internal free runs still exist. Defragment must use those
        # internal destinations rather than incorrectly requiring tail space.
        used.update(range(DATA_LCN + 64, TOTAL_CLUSTERS - 1))
    for cluster in used:
        bitmap[cluster >> 3] |= 1 << (cluster & 7)
    image[BITMAP_LCN * CLUSTER_SIZE:(BITMAP_LCN + 1) * CLUSTER_SIZE] = bitmap

    payload = bytes((index * 37 + 11) & 0xFF for index in range(DATA_CLUSTERS * CLUSTER_SIZE))
    offset = 0
    for lcn, length in data_runs:
        count = length * CLUSTER_SIZE
        image[lcn * CLUSTER_SIZE:(lcn + length) * CLUSTER_SIZE] = payload[offset:offset + count]
        offset += count
    if named_ads:
        ads_payload = bytes((index * 29 + 13) & 0xFF
                            for index in range(ADS_CLUSTERS * CLUSTER_SIZE))
        first_bytes = (ADS_CLUSTERS // 2) * CLUSTER_SIZE
        image[ADS_LCN * CLUSTER_SIZE:
              ADS_LCN * CLUSTER_SIZE + first_bytes] = ads_payload[:first_bytes]
        image[ADS_SECOND_LCN * CLUSTER_SIZE:
              ADS_SECOND_LCN * CLUSTER_SIZE +
              (ADS_CLUSTERS - ADS_CLUSTERS // 2) * CLUSTER_SIZE] = (
                  ads_payload[first_bytes:]
              )
    if directory_data:
        directory_payload = bytes((index * 19 + 7) & 0xFF
                                  for index in range(directory_clusters * CLUSTER_SIZE))
        image[directory_lcn * CLUSTER_SIZE:
              (directory_lcn + directory_clusters) * CLUSTER_SIZE] = directory_payload
    if fixed_attribute_list_stream:
        fixed_payload = bytes((index * 23 + 5) & 0xFF
                              for index in range(FIXED_USER_CLUSTERS * CLUSTER_SIZE))
        image[FIXED_USER_LCN * CLUSTER_SIZE:
              (FIXED_USER_LCN + FIXED_USER_CLUSTERS) * CLUSTER_SIZE] = fixed_payload
    path.write_bytes(image)
    return payload



__all__ = [
    "make_image", "ADS_CLUSTERS", "ADS_LCN", "ADS_SECOND_LCN",
    "CLUSTER_SIZE",
]
