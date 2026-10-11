# SPDX-License-Identifier: GPL-3.0-or-later
"""Filesystem-neutral application of worker live-map events."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True, slots=True)
class LiveRangeSummary:
    moved_total_bytes: int
    pass_number: int
    objects_done: int
    objects_total: int
    sequence: int


class LiveMapUpdater:
    """Mutate the standard allocation-map schema from typed engine events."""

    def __init__(self, map_data: dict[str, Any]):
        cells = map_data.get("cells")
        if not isinstance(cells, list):
            raise ValueError("allocation map has no cell list")
        self.map_data = map_data
        self.cells: list[dict[str, int]] = cells

    @staticmethod
    def first_overlapping_cell(cells: list[dict[str, int]], unit: int) -> int:
        low, high = 0, len(cells)
        while low < high:
            middle = (low + high) // 2
            if int(cells[middle]["end"]) < unit:
                low = middle + 1
            else:
                high = middle
        return low

    def _unit_size(self) -> int:
        unit_size = int(
            self.map_data.get("unit_size")
            or self.map_data.get("cluster_size")
            or 0
        )
        if unit_size <= 0:
            raise ValueError("allocation map has no valid unit size")
        return unit_size

    def _apply_range(self, start_byte: int, length_byte: int, make_used: bool) -> None:
        if length_byte <= 0:
            return
        unit_size = self._unit_size()
        start_unit = start_byte // unit_size
        end_unit = (start_byte + length_byte + unit_size - 1) // unit_size
        index = self.first_overlapping_cell(self.cells, start_unit)
        while index < len(self.cells):
            cell = self.cells[index]
            cell_start = int(cell["start"])
            if cell_start >= end_unit:
                break
            cell_end = int(cell["end"]) + 1
            overlap = max(0, min(end_unit, cell_end) - max(start_unit, cell_start))
            if overlap:
                if make_used:
                    moved = min(overlap, int(cell.get("free", 0)))
                    cell["free"] = max(0, int(cell.get("free", 0)) - moved)
                    cell["used"] = int(cell.get("used", 0)) + moved
                else:
                    old_used = max(1, int(cell.get("used", 0)))
                    moved = min(overlap, int(cell.get("used", 0)))
                    fragmented = int(
                        round(moved * int(cell.get("fragmented", 0)) / old_used)
                    )
                    directory = int(
                        round(moved * int(cell.get("directory", 0)) / old_used)
                    )
                    cell["used"] = max(0, int(cell.get("used", 0)) - moved)
                    cell["free"] = int(cell.get("free", 0)) + moved
                    cell["fragmented"] = max(
                        0,
                        min(int(cell.get("fragmented", 0)) - fragmented, cell["used"]),
                    )
                    cell["directory"] = max(
                        0,
                        min(int(cell.get("directory", 0)) - directory, cell["used"]),
                    )
            index += 1

    def reset(self, payload: dict[str, Any]) -> None:
        if "reason" in payload and "filesystem_units" not in payload:
            return
        unit_size = int(payload.get("unit_size") or self._unit_size())
        filesystem_units = int(payload.get("filesystem_units") or 0)
        if filesystem_units <= 0:
            raise ValueError("live reset has no filesystem unit count")
        for cell in self.cells:
            start = int(cell["start"])
            end_exclusive = int(cell["end"]) + 1
            inside = max(0, min(end_exclusive, filesystem_units) - start)
            outside = max(0, end_exclusive - max(start, filesystem_units))
            cell.update(
                free=inside,
                used=0,
                unknown=0,
                outside=outside,
                fragmented=0,
                directory=0,
                bad=0,
            )

        def mark_used(start_byte: int, length_byte: int) -> None:
            if length_byte <= 0:
                return
            start_unit = start_byte // unit_size
            end_unit = (start_byte + length_byte + unit_size - 1) // unit_size
            index = self.first_overlapping_cell(self.cells, start_unit)
            while index < len(self.cells):
                cell = self.cells[index]
                cell_start = int(cell["start"])
                if cell_start >= end_unit:
                    break
                cell_end = int(cell["end"]) + 1
                overlap = max(
                    0,
                    min(end_unit, cell_end, filesystem_units) - max(start_unit, cell_start),
                )
                if overlap:
                    moved = min(overlap, int(cell.get("free", 0)))
                    cell["free"] = max(0, int(cell.get("free", 0)) - moved)
                    cell["used"] = int(cell.get("used", 0)) + moved
                index += 1

        used_ranges = payload.get("used_ranges")
        if not isinstance(used_ranges, list):
            raise ValueError("live reset has no used-range list")
        for entry in used_ranges:
            if not isinstance(entry, list) or len(entry) != 2:
                raise ValueError("invalid live reset used range")
            mark_used(int(entry[0]), int(entry[1]))

        self.map_data["filesystem_units"] = filesystem_units
        self.map_data["filesystem_bytes"] = filesystem_units * unit_size
        self.map_data["outside_bytes"] = max(
            0,
            int(self.map_data.get("total_units", 0)) - filesystem_units,
        ) * unit_size

    def ranges(self, payload: dict[str, Any], *, plural: bool) -> LiveRangeSummary:
        if plural:
            physical_ranges = payload.get("ranges", [])
        else:
            physical_ranges = [[
                payload["source_start_byte"],
                payload["destination_start_byte"],
                payload["length_bytes"],
            ]]
        allocate_only = str(payload.get("mode", "")) == "allocate-only"
        for physical_range in physical_ranges:
            if not isinstance(physical_range, list) or len(physical_range) != 3:
                continue
            source, destination, length = (int(value) for value in physical_range)
            if not allocate_only:
                self._apply_range(source, length, False)
            self._apply_range(destination, length, True)
        return LiveRangeSummary(
            moved_total_bytes=int(payload.get("moved_total_bytes", 0)),
            pass_number=int(payload.get("pass", 1)),
            objects_done=int(payload.get("objects_done", 0)),
            objects_total=int(payload.get("objects_total", 0)),
            sequence=int(payload.get("sequence", 0)),
        )

    def cells_delta(self, payload: dict[str, Any]) -> None:
        for changed in payload.get("cells", []):
            index = int(changed["i"])
            if 0 <= index < len(self.cells):
                self.cells[index] = {
                    "start": int(changed["start"]),
                    "end": int(changed["end"]),
                    "free": int(changed["free"]),
                    "used": int(changed["used"]),
                    "unknown": int(changed.get("unknown", 0)),
                    "outside": int(changed.get("outside", 0)),
                    "fragmented": int(changed["fragmented"]),
                    "directory": int(changed["directory"]),
                    "bad": int(changed["bad"]),
                }
        for name in (
            "fragmented_files",
            "fragmented_directories",
            "free_clusters",
            "free_gaps_below_highest",
        ):
            if name in payload:
                self.map_data[name] = int(payload[name])
