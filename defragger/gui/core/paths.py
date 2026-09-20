#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Single executable-path registry shared by the GUI and operation dispatcher."""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True, slots=True)
class ProgramPath:
    id: str
    environment: str
    installed: str
    source_relative: str | None = None


PROGRAMS: dict[str, ProgramPath] = {
    "hfsplus-native": ProgramPath(
        "hfsplus-native", "LINUX_DEFRAGGER_HFSPLUS_WORKER",
        "/usr/lib/linux-defragger/filesystems/hfsplus/linux-defragger-hfsplus-worker",
        "../../build/linux-defragger-hfsplus-worker"
    ),
    "hfs-native": ProgramPath(
        "hfs-native", "LINUX_DEFRAGGER_HFS_ANALYSER",
        "/usr/lib/linux-defragger/filesystems/hfs/hfs_analyser",
        "../../build/hfs_analyser"
    ),
    "affs-native": ProgramPath(
        "affs-native", "LINUX_DEFRAGGER_AFFS_WORKER",
        "/usr/lib/linux-defragger/filesystems/affs/linux-defragger-affs-worker",
        "../../build/linux-defragger-affs-worker"
    ),
    "btrfs-native": ProgramPath(
        "btrfs-native", "LINUX_DEFRAGGER_BTRFS_WORKER",
        "/usr/lib/linux-defragger/filesystems/btrfs/linux-defragger-btrfs-worker",
        "../../build/linux-defragger-btrfs-worker"
    ),
    "fat-native": ProgramPath(
        "fat-native", "LINUX_DEFRAGGER_FAT_WORKER",
        "/usr/lib/linux-defragger/filesystems/fat/linux-defragger-fat-worker",
        "../../build/linux-defragger-fat-worker"
    ),
    "exfat-native": ProgramPath(
        "exfat-native", "LINUX_DEFRAGGER_EXFAT_WORKER",
        "/usr/lib/linux-defragger/filesystems/exfat/linux-defragger-exfat-worker",
        "../../build/linux-defragger-exfat-worker"
    ),
    "ntfs-native": ProgramPath(
        "ntfs-native", "LINUX_DEFRAGGER_NTFS_WORKER",
        "/usr/lib/linux-defragger/filesystems/ntfs/linux-defragger-ntfs-worker",
        "../../build/linux-defragger-ntfs-worker"
    ),
    "ext-native": ProgramPath(
        "ext-native", "LINUX_DEFRAGGER_EXT_WORKER",
        "/usr/lib/linux-defragger/filesystems/ext4/linux-defragger-ext-worker",
        "../../build/linux-defragger-ext-worker"
    ),
    "xfs-native": ProgramPath(
        "xfs-native", "LINUX_DEFRAGGER_XFS_WORKER",
        "/usr/lib/linux-defragger/filesystems/xfs/linux-defragger-xfs-worker",
        "../../build/linux-defragger-xfs-worker"
    ),
    "apfs-native": ProgramPath(
        "apfs-native", "LINUX_DEFRAGGER_APFS_WORKER",
        "/usr/lib/linux-defragger/filesystems/apfs/linux-defragger-apfs-worker",
        "../../build/linux-defragger-apfs-worker"
    ),
    "minix-native": ProgramPath(
        "minix-native", "LINUX_DEFRAGGER_MINIX_WORKER",
        "/usr/lib/linux-defragger/filesystems/minix/linux-defragger-minix-worker",
        "../../build/linux-defragger-minix-worker"
    ),
    "sfs-native": ProgramPath(
        "sfs-native", "LINUX_DEFRAGGER_SFS_WORKER",
        "/usr/lib/linux-defragger/filesystems/sfs/linux-defragger-sfs-worker",
        "../../build/linux-defragger-sfs-worker"
    ),
    "pfs3-native": ProgramPath(
        "pfs3-native", "LINUX_DEFRAGGER_PFS3_WORKER",
        "/usr/lib/linux-defragger/filesystems/pfs3/linux-defragger-pfs3-worker",
        "../../build/linux-defragger-pfs3-worker"
    ),
    "swap-native": ProgramPath(
        "swap-native", "LINUX_DEFRAGGER_SWAP_WORKER",
        "/usr/lib/linux-defragger/filesystems/swap/linux-defragger-swap-worker",
        "../../build/linux-defragger-swap-worker"
    ),
    "ufs-native": ProgramPath(
        "ufs-native", "LINUX_DEFRAGGER_UFS_WORKER",
        "/usr/lib/linux-defragger/filesystems/ufs/linux-defragger-ufs-worker",
        "../../build/linux-defragger-ufs-worker"
    ),
    "zfs-native": ProgramPath(
        "zfs-native", "LINUX_DEFRAGGER_ZFS_WORKER",
        "/usr/lib/linux-defragger/filesystems/zfs/linux-defragger-zfs-worker",
        "../../build/linux-defragger-zfs-worker"
    ),
    "mapper": ProgramPath(
        "mapper", "LINUX_DEFRAGGER_MAPPER",
        "/usr/lib/linux-defragger/linux-defragger-mapper",
        "../../build/linux-defragger-mapper"
    ),
    "operation-engine": ProgramPath(
        "operation-engine", "LINUX_DEFRAGGER_OPERATION_ENGINE",
        "/usr/lib/linux-defragger/linux-defragger-operation-engine",
        "../../build/linux-defragger-operation-engine"
    ),
    "helper": ProgramPath(
        "helper", "LINUX_DEFRAGGER_HELPER",
        "/usr/lib/linux-defragger/linux-defragger-privileged-helper",
        "../../build/linux-defragger-privileged-helper"
    ),

}


def resolve_program(program_id: str, *, anchor: Path | None = None) -> str:
    """Resolve an executable from an override, source tree or installed path."""

    try:
        spec = PROGRAMS[program_id]
    except KeyError as exc:
        raise FileNotFoundError(f"unknown Defragmenter program: {program_id}") from exc

    override = os.environ.get(spec.environment)
    candidates: list[Path] = []
    if override:
        candidates.append(Path(override))
    if anchor is not None and spec.source_relative:
        candidates.append((anchor / spec.source_relative).resolve())
    candidates.append(Path(spec.installed))

    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    rendered = ", ".join(str(candidate) for candidate in candidates)
    raise FileNotFoundError(f"could not locate {program_id}; checked: {rendered}")
