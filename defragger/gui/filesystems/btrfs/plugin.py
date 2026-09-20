#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Defragmenter
# Author: Shannon Smith
# Purpose: Thin GUI adapter for the native C Btrfs physical analyser.

"""Native Btrfs allocation, fragmentation and bounded offline-write backend.

All on-disk superblock, chunk-tree, root-tree, extent-tree and filesystem-tree
parsing lives in the native C worker. Python only launches that worker and
validates its stable GUI map contract.
"""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path
from typing import Any

from backends.base import (
    BackendError, BackendInfo, CAP_ANALYSE, CAP_DEFRAG, CAP_GROWTH_DEFRAG,
    CAP_LIVE_MAP, CAP_MAP, CAP_RECOVER, FilesystemBackend, operation,
)
from core.paths import resolve_program

INFO = BackendInfo(
    "btrfs", "Btrfs", ("btrfs",),
    CAP_ANALYSE | CAP_MAP | CAP_DEFRAG | CAP_GROWTH_DEFRAG | CAP_RECOVER | CAP_LIVE_MAP,
    "exact",
    operations=(
        operation(
            "defrag", "btrfs-native",
            warning="Btrfs writing is offline and fail-closed to the qualified single-device, CRC32C, level-0, mixed data/metadata, NODATASUM regular-file subset.",
        ),
        operation(
            "growth-defrag", "btrfs-native",
            warning="Btrfs Growth Defrag leaves an exact 10% free-sector reserve after each supported regular file.",
        ),
        operation("recover", "btrfs-native"),
    ),
)


def _mark_outside_tail(
    result: dict,
    filesystem_units: int,
    physical_units: int,
    unit_size: int,
) -> dict:
    """Compatibility helper for callers that post-process an allocation map."""

    physical_units = max(0, int(physical_units))
    filesystem_units = max(0, min(physical_units, int(filesystem_units)))
    outside_total = 0
    for cell in result.get("cells", []):
        start = int(cell.get("start", 0))
        end_exclusive = int(cell.get("end", start - 1)) + 1
        if end_exclusive <= start:
            continue
        outside = max(0, end_exclusive - max(start, filesystem_units))
        outside = min(outside, end_exclusive - start)
        cell["outside"] = outside
        if outside:
            cell["unknown"] = max(0, int(cell.get("unknown", 0)) - outside)
            outside_total += outside

    outside_bytes = outside_total * unit_size
    result["filesystem_units"] = filesystem_units
    result["filesystem_bytes"] = filesystem_units * unit_size
    result["outside_bytes"] = outside_bytes
    result["unknown_bytes"] = max(0, int(result.get("unknown_bytes", 0)) - outside_bytes)
    result.setdefault("details", {}).update({
        "physical_units": physical_units,
        "filesystem_units": filesystem_units,
        "outside_filesystem_units": outside_total,
    })
    return result


class BtrfsBackend(FilesystemBackend):
    info = INFO

    @staticmethod
    def _run_native(path: str, mode: str, *args: object) -> subprocess.CompletedProcess[str]:
        anchor = Path(__file__).resolve().parents[2] / "core"
        worker = resolve_program("btrfs-native", anchor=anchor)
        return subprocess.run(
            [worker, mode, path, *map(str, args)],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={**os.environ, "LC_ALL": "C", "LANG": "C"},
        )

    @staticmethod
    def _json_result(completed: subprocess.CompletedProcess[str]) -> dict[str, Any]:
        try:
            payload = json.loads(completed.stdout)
        except json.JSONDecodeError as exc:
            detail = completed.stderr.strip()
            if detail:
                raise BackendError(detail) from exc
            raise BackendError("native Btrfs worker returned invalid JSON") from exc
        if not isinstance(payload, dict):
            raise BackendError("native Btrfs worker returned a non-object result")
        return payload

    def probe(self, path: str) -> bool:
        try:
            completed = self._run_native(path, "identify")
            return (
                completed.returncode == 0
                and self._json_result(completed).get("filesystem") == "btrfs"
            )
        except (BackendError, FileNotFoundError, OSError):
            return False

    def map(self, path: str, cells: int) -> dict:
        completed = self._run_native(path, "map", "--cells", max(1, int(cells)))
        if completed.returncode != 0:
            detail = completed.stderr.strip() or completed.stdout.strip()
            raise BackendError(detail or "native Btrfs analyser failed")
        payload = self._json_result(completed)
        if payload.get("filesystem") != "btrfs":
            raise BackendError("native Btrfs analyser returned the wrong filesystem identity")
        if payload.get("schema") != 1 or payload.get("backend") != "read-only-domain":
            raise BackendError("native Btrfs analyser returned an incompatible map schema")
        if payload.get("map_accuracy") != "exact-single-device":
            raise BackendError("native Btrfs analyser returned an unexpected accuracy contract")
        if not isinstance(payload.get("cells"), list) or not isinstance(payload.get("details"), dict):
            raise BackendError("native Btrfs analyser returned an incomplete map")
        return payload


BACKEND = BtrfsBackend()
