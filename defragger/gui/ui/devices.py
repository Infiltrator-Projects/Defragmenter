# SPDX-License-Identifier: GPL-3.0-or-later
"""Block-device discovery and filesystem-neutral volume model."""

from __future__ import annotations

import json
import os
import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterable, Mapping

from core.paths import resolve_program

from .formatting import human_bytes
from .backend_catalog import BackendCatalog


RunCommand = Callable[..., subprocess.CompletedProcess[str]]
UnknownFilesystemProbe = Callable[[str], str]
TestMediaFilesystemProbe = Callable[[str, str], str]
_GENERIC_FAT_TYPES = frozenset({"vfat", "fat", "msdos"})
_EXT_TYPES = frozenset({"ext2", "ext3", "ext4"})
_AMIGA_TYPES = frozenset({"ofs", "ffs"})
_TEST_MEDIA_RAW_PARTLABELS = {
    "ld_ofs": "ofs",
    "ld_ffs": "ffs",
    "ld_sfs": "sfs",
    "ld_pfs3": "pfs3",
    "ld_apfs": "apfs",
}
_NATURAL_DEVICE_PARTS = re.compile(r"(\d+)")


def json_bool(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    return str(value or "").strip().lower() in {"1", "true", "yes", "on"}


def flatten_lsblk(nodes: Iterable[dict[str, Any]]) -> Iterable[dict[str, Any]]:
    for node in nodes:
        yield node
        yield from flatten_lsblk(node.get("children") or [])


def natural_device_sort_key(path: str) -> tuple[tuple[int, object], ...]:
    """Sort Linux block-device paths by numeric components, not text order.

    This keeps partitions grouped in the order humans expect, for example
    ``mmcblk0p1, mmcblk0p2, ... mmcblk0p10`` and
    ``nvme0n1p1, nvme0n1p2, ... nvme0n1p12``.
    """

    parts: list[tuple[int, object]] = []
    for part in _NATURAL_DEVICE_PARTS.split(path.casefold()):
        if not part:
            continue
        if part.isdigit():
            parts.append((1, int(part)))
        else:
            parts.append((0, part))
    return tuple(parts)


def fat_variant(fstype: str, fs_version: str) -> str:
    """Return FAT12/FAT16/FAT32 when Linux metadata is precise enough."""

    raw = str(fstype or "").strip().lower()
    if raw in {"fat12", "fat16", "fat32"}:
        return raw
    if raw not in _GENERIC_FAT_TYPES:
        return ""
    version = str(fs_version or "").strip().lower().replace(" ", "")
    for variant in ("fat12", "fat16", "fat32"):
        if version in {variant, variant[3:]}:
            return variant
    return ""


def first_party_unknown_filesystem_probe(path: str) -> str:
    """Probe formats that the host discovery stack commonly leaves unnamed.

    Device enumeration stays independent from Linux filesystem support.  The
    native Amiga parser is consulted when lsblk/libblkid cannot classify a
    device.  Permission failures are expected for some physical devices and
    simply leave the result unknown; deterministic Test Media partition labels
    provide a second, non-authoritative hint for those root-only devices.
    """

    if not path:
        return ""
    anchor = Path(__file__).resolve().parents[1] / "core"
    try:
        worker = resolve_program("affs-native", anchor=anchor)
        completed = subprocess.run(
            [worker, "identify", path],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=2.0,
            env={**os.environ, "LC_ALL": "C", "LANG": "C"},
        )
    except (FileNotFoundError, OSError, subprocess.TimeoutExpired):
        return ""
    if completed.returncode != 0:
        return ""
    try:
        payload = json.loads(completed.stdout)
    except json.JSONDecodeError:
        return ""
    if not isinstance(payload, dict) or payload.get("filesystem") != "affs":
        return ""
    variant = str(payload.get("variant") or "").strip().lower()
    return variant if variant in _AMIGA_TYPES else "affs"


_TEST_MEDIA_NATIVE_PROBES = {
    "apfs": ("apfs-native", "apfs"),
}


def first_party_test_media_probe(path: str, expected: str) -> str:
    """Verify raw Test Media identities that cannot safely rely on GPT labels.

    LD_APFS is merely a partition slot label. Older or partially rebuilt
    qualification media may carry that label while the partition still
    contains another filesystem or no filesystem at all.
    """

    spec = _TEST_MEDIA_NATIVE_PROBES.get(expected)
    if not path or spec is None:
        return ""
    program_id, filesystem = spec
    anchor = Path(__file__).resolve().parents[1] / "core"
    try:
        worker = resolve_program(program_id, anchor=anchor)
        completed = subprocess.run(
            [worker, "identify", path],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=2.0,
            env={**os.environ, "LC_ALL": "C", "LANG": "C"},
        )
    except (FileNotFoundError, OSError, subprocess.TimeoutExpired):
        return ""
    if completed.returncode != 0:
        return ""
    try:
        payload = json.loads(completed.stdout)
    except json.JSONDecodeError:
        return ""
    if not isinstance(payload, dict):
        return ""
    return filesystem if str(payload.get("filesystem") or "").lower() == filesystem else ""


def _resolved_discovery_fstype(
    catalog: BackendCatalog,
    node: dict[str, Any],
    probe_unknown: UnknownFilesystemProbe,
    probe_test_media: TestMediaFilesystemProbe,
) -> str:
    """Resolve one lsblk node without turning partition labels into evidence."""

    raw = str(node.get("fstype") or "").strip().lower()
    if raw and catalog.supports(raw):
        return raw

    path = str(node.get("path") or "")
    partlabel = str(node.get("partlabel") or "").strip().lower()
    hinted = _TEST_MEDIA_RAW_PARTLABELS.get(partlabel, "")

    # APFS Test Media has its own positive first-party identity probe. Skip the
    # unrelated AFFS subprocess entirely for this known slot; doing both probes
    # only adds process/timeout latency and cannot improve the APFS decision.
    # A stale label still proves nothing: only apfs-native identify can accept it.
    if hinted == "apfs" and catalog.supports(hinted):
        verified = str(probe_test_media(path, hinted) or "").strip().lower()
        return verified if verified == hinted else ""

    probed = str(probe_unknown(path) or "").strip().lower()
    if probed and catalog.supports(probed):
        return probed

    if not hinted or not catalog.supports(hinted):
        return ""

    # OFS/FFS/SFS/PFS3 deliberately retain native AFFS probing ahead of their
    # deterministic Test Media slot hints, so stale labels can never override a
    # real Amiga filesystem identity.
    return hinted


@dataclass(slots=True)
class Volume:
    catalog: BackendCatalog
    path: str
    name: str
    fstype: str
    label: str
    size: int
    mountpoints: list[str]
    removable: bool
    readonly: bool
    model: str
    transport: str
    image: bool = False
    fs_version: str = ""
    filesystem_uuid: str = ""
    partition_uuid: str = ""
    _cache_nonce: object = field(
        default_factory=object,
        init=False,
        repr=False,
        compare=False,
    )

    @property
    def mounted(self) -> bool:
        return any(self.mountpoints)

    @property
    def normalized_fstype(self) -> str:
        variant = fat_variant(self.fstype, self.fs_version)
        if not variant and self.fstype.strip().lower() in _GENERIC_FAT_TYPES:
            # Preserve uncertainty until the native FAT geometry probe can
            # identify the width; vfat is not evidence of FAT32.
            return self.fstype.strip().lower()
        return variant or self.catalog.normalize(self.fstype)

    @property
    def display_fstype(self) -> str:
        """Return the real filesystem identity rather than its shared backend id."""

        raw = self.fstype.strip().lower()
        variant = fat_variant(raw, self.fs_version)
        if variant:
            return variant
        if raw in _GENERIC_FAT_TYPES:
            return "fat"
        # EXT2/3/4 deliberately share one backend, historically named ext4.
        # Routing may therefore normalize ext2/ext3 to ext4, but the selector
        # must continue to show the filesystem that was actually discovered.
        if raw in _EXT_TYPES:
            return raw
        # OFS and FFS likewise share the AFFS backend while retaining their
        # actual DOS\0/DOS\1 identity in user-facing text.
        if raw in _AMIGA_TYPES:
            return raw
        return self.normalized_fstype

    @property
    def operations(self) -> Mapping[str, Mapping[str, Any]]:
        """Return operations from the authoritative native backend manifest."""

        return self.catalog.operations_for(self.normalized_fstype)

    @property
    def cache_key(self) -> tuple[object, ...]:
        """Identify the filesystem instance, not merely its reusable path."""

        if self.image:
            try:
                stat = os.stat(self.path)
            except OSError:
                return ("image-ephemeral", self.path, self._cache_nonce)
            return (
                "image",
                self.path,
                stat.st_dev,
                stat.st_ino,
                stat.st_size,
                stat.st_mtime_ns,
                self.normalized_fstype,
            )

        filesystem_uuid = self.filesystem_uuid.strip().lower()
        partition_uuid = self.partition_uuid.strip().lower()
        if filesystem_uuid or partition_uuid:
            return (
                "device",
                self.path,
                filesystem_uuid,
                partition_uuid,
                self.normalized_fstype,
                self.size,
            )

        # A filesystem with no stable UUID must not inherit analysis from a
        # different object discovered later at the same device path.
        return ("device-ephemeral", self.path, self._cache_nonce)

    @property
    def display_name(self) -> str:
        label = self.label or self.model or self.name
        status = "mounted" if self.mounted else "unmounted"
        kind = "image" if self.image else (self.transport or "device")
        filesystem = self.display_fstype.upper()
        return (
            f"{self.path} — {label} — {filesystem} — {human_bytes(self.size)} — "
            f"{kind}, {status}"
        )


def discover_volumes(
    catalog: BackendCatalog,
    *,
    run: RunCommand = subprocess.run,
    probe_unknown: UnknownFilesystemProbe = first_party_unknown_filesystem_probe,
    probe_test_media: TestMediaFilesystemProbe = first_party_test_media_probe,
) -> list[Volume]:
    columns = (
        "NAME,PATH,TYPE,FSTYPE,FSVER,LABEL,PARTLABEL,UUID,PARTUUID,SIZE,"
        "MOUNTPOINTS,RM,RO,MODEL,TRAN"
    )
    result = run(
        ["lsblk", "--json", "--bytes", "--output", columns],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={**os.environ, "LC_ALL": "C"},
    )
    data = json.loads(result.stdout)
    volumes: list[Volume] = []
    for node in flatten_lsblk(data.get("blockdevices", [])):
        fstype = _resolved_discovery_fstype(
            catalog, node, probe_unknown, probe_test_media
        )
        if not fstype:
            continue
        partlabel = str(node.get("partlabel") or "")
        volumes.append(
            Volume(
                catalog=catalog,
                path=str(node.get("path") or ""),
                name=str(node.get("name") or ""),
                fstype=fstype,
                label=str(node.get("label") or partlabel or ""),
                size=int(node.get("size") or 0),
                mountpoints=[
                    str(item)
                    for item in (node.get("mountpoints") or [])
                    if item
                ],
                removable=json_bool(node.get("rm")),
                readonly=json_bool(node.get("ro")),
                model=str(node.get("model") or "").strip(),
                transport=str(node.get("tran") or ""),
                fs_version=str(node.get("fsver") or ""),
                filesystem_uuid=str(node.get("uuid") or ""),
                partition_uuid=str(node.get("partuuid") or ""),
            )
        )
    volumes.sort(
        key=lambda volume: (
            not volume.removable,
            natural_device_sort_key(volume.path),
        )
    )
    return volumes
