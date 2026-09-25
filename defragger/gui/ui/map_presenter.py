# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate allocation-map results and build presentation-ready text."""

from __future__ import annotations

from collections.abc import Iterable
from dataclasses import dataclass
from typing import Any

from .formatting import human_bytes


class AllocationMapError(ValueError):
    """An analyser returned an invalid or incomplete allocation-map contract."""


@dataclass(frozen=True, slots=True)
class MapPresentation:
    cells: list[dict[str, int]]
    cell_count: int
    capacity_title: str
    capacity_value: str
    free_title: str
    free_value: str
    files_title: str
    files_value: str
    fragmentation_title: str
    fragmentation_value: str
    unit_label: str
    caption: str
    status: str
    analysis_log: str


def operation_labels(operations: Iterable[str]) -> tuple[str, ...]:
    available = frozenset(operations)
    labels: list[str] = []
    if "defrag" in available:
        labels.append("Defragment")
    if "growth-defrag" in available:
        labels.append("Growth Defrag")
    if "recover" in available:
        labels.append("Recover")
    return tuple(labels)


def _required_int(data: dict[str, Any], name: str, *, minimum: int = 0) -> int:
    if name not in data:
        raise AllocationMapError(f"allocation map is missing {name!r}")
    try:
        value = int(data[name])
    except (TypeError, ValueError) as exc:
        raise AllocationMapError(f"allocation map {name!r} is not an integer") from exc
    if value < minimum:
        raise AllocationMapError(f"allocation map {name!r} is below {minimum}")
    return value


def _optional_int(data: dict[str, Any], name: str, default: int = 0) -> int:
    if name not in data:
        return default
    return _required_int(data, name)


def _validated_cells(data: dict[str, Any]) -> tuple[list[dict[str, int]], int]:
    raw_cells = data.get("cells")
    if not isinstance(raw_cells, list) or not raw_cells:
        raise AllocationMapError("allocation map has no cells")
    cells: list[dict[str, int]] = []
    previous_end = -1
    for index, raw_cell in enumerate(raw_cells):
        if not isinstance(raw_cell, dict):
            raise AllocationMapError(f"allocation map cell {index} is not an object")
        try:
            start = int(raw_cell["start"])
            end = int(raw_cell["end"])
        except (KeyError, TypeError, ValueError) as exc:
            raise AllocationMapError(
                f"allocation map cell {index} has invalid bounds"
            ) from exc
        if start < 0 or end < start or start <= previous_end:
            raise AllocationMapError(f"allocation map cell {index} is out of order")
        previous_end = end
        normalized = dict(raw_cell)
        normalized["start"] = start
        normalized["end"] = end
        span = end - start + 1
        for field in (
            "free",
            "used",
            "unknown",
            "outside",
            "fragmented",
            "directory",
            "bad",
        ):
            try:
                value = int(raw_cell.get(field, 0))
            except (TypeError, ValueError) as exc:
                raise AllocationMapError(
                    f"allocation map cell {index} field {field!r} is not an integer"
                ) from exc
            if value < 0 or value > span:
                raise AllocationMapError(
                    f"allocation map cell {index} field {field!r} is outside its bounds"
                )
            normalized[field] = value
        cells.append(normalized)
    cell_count = _required_int(data, "cell_count", minimum=1)
    if cell_count != len(cells):
        raise AllocationMapError(
            f"allocation map declares {cell_count} cells but returned {len(cells)}"
        )
    return cells, cell_count


def _domain_presentation(
    data: dict[str, Any],
    operations: Iterable[str],
    cells: list[dict[str, int]],
    cell_count: int,
) -> MapPresentation:
    filesystem = str(data.get("filesystem") or "unknown").upper()
    total_bytes = _required_int(data, "total_bytes")
    free_bytes = _required_int(data, "free_bytes")
    used_bytes = _required_int(data, "used_bytes")
    unknown_bytes = _optional_int(data, "unknown_bytes")
    total_units = _required_int(data, "total_units", minimum=1)
    outside_bytes = _optional_int(data, "outside_bytes")
    filesystem_bytes = _optional_int(data, "filesystem_bytes", total_bytes)
    raw_details = data.get("details")
    details: dict[str, Any] = raw_details if isinstance(raw_details, dict) else {}
    is_swap = filesystem == "SWAP"
    summary_fields = (
        "regular_files",
        "directories",
        "fragmented_files",
        "fragmented_directories",
    )
    has_fragmentation = all(name in data for name in summary_fields)
    labels = operation_labels(operations)
    full_allocation_unknown = (
        not is_swap
        and str(data.get("map_accuracy") or "").lower() == "summary"
        and used_bytes == 0
        and free_bytes == 0
        and filesystem_bytes > 0
        and unknown_bytes >= filesystem_bytes
    )

    if full_allocation_unknown:
        free_value = "Unknown"
    elif outside_bytes:
        free_value = (
            f"{human_bytes(free_bytes)} usable · "
            f"{human_bytes(outside_bytes)} outside filesystem"
        )
    else:
        free_value = (
            f"{human_bytes(free_bytes)} "
            f"({free_bytes * 100.0 / max(1, filesystem_bytes):.1f}%)"
        )

    files_title = "Files"
    fragmentation_value: str
    if is_swap:
        files_title = "Usage"
        swap_page_size = int(details.get("page_size") or data.get("unit_size") or 512)
        if swap_page_size <= 0:
            raise AllocationMapError("swap page size must be positive")
        used_pages = (
            (used_bytes + swap_page_size - 1) // swap_page_size
            if used_bytes
            else 0
        )
        page_word = "page" if used_pages == 1 else "pages"
        files_value = f"{human_bytes(used_bytes)} used · {used_pages:,} {page_word}"
        fragmentation_value = "N/A · swap has no files"
    elif has_fragmentation:
        files_value = (
            f"{_required_int(data, 'regular_files'):,} files · "
            f"{_required_int(data, 'directories'):,} dirs"
        )
        fragmented_files = _required_int(data, "fragmented_files")
        if "fragmentation_percent" in data:
            try:
                fragmentation_percent = float(data["fragmentation_percent"])
            except (TypeError, ValueError) as exc:
                raise AllocationMapError(
                    "allocation map fragmentation percentage is not numeric"
                ) from exc
            fragmentation_value = (
                f"{fragmentation_percent:.1f}% · "
                f"{fragmented_files:,} files"
            )
        else:
            fragmentation_value = (
                f"{fragmented_files:,} files · "
                f"{_required_int(data, 'fragmented_directories'):,} dirs"
            )
    else:
        if full_allocation_unknown:
            files_value = "Unknown"
            fragmentation_value = "Not available"
        else:
            files_value = f"{human_bytes(used_bytes)} allocated"
            fragmentation_value = "Not calculated" if labels else "Not available"

    unit_size = _optional_int(data, "unit_size", 512)
    if unit_size <= 0:
        raise AllocationMapError("allocation-map unit size must be positive")
    if unit_size == 512:
        unit_label = "sectors"
    elif unit_size == 4096:
        unit_label = "4 KB units"
    else:
        unit_label = f"{human_bytes(unit_size)} units"
    units_per_cell = total_units / cell_count

    if is_swap and bool(details.get("active")):
        caption = (
            f"Active swap · {human_bytes(used_bytes)} used of "
            f"{human_bytes(filesystem_bytes)} · aggregate usage only; "
            "physical slot locations unavailable"
        )
    elif is_swap:
        caption = (
            f"Inactive swap area · approximately {units_per_cell:,.1f} "
            f"{unit_label} per cell"
        )
    elif full_allocation_unknown:
        caption = (
            f"Allocation image: {cell_count:,} cells · exact allocated/free "
            "locations are not decoded yet"
        )
    elif outside_bytes:
        caption = (
            f"Allocation image: {cell_count:,} cells · dark tail is outside the "
            "active filesystem boundary"
        )
    else:
        caption = (
            f"Allocation image: {cell_count:,} cells · approximately "
            f"{units_per_cell:,.1f} {unit_label} per cell"
        )

    unknown = f" · {human_bytes(unknown_bytes)} location unknown" if unknown_bytes else ""
    if is_swap and bool(details.get("active")):
        status = (
            f"SWAP active · {human_bytes(used_bytes)} used of "
            f"{human_bytes(filesystem_bytes)} · {used_pages:,} {page_word} · "
            "aggregate usage only; physical slot locations unavailable"
        )
    elif is_swap:
        status = (
            f"SWAP inactive · {human_bytes(free_bytes)} usable free · "
            "exact page map"
        )
    elif has_fragmentation:
        status = (
            f"{filesystem} · {_required_int(data, 'fragmented_files')} fragmented files · "
            f"{_required_int(data, 'fragmented_directories')} fragmented directories"
        )
    elif full_allocation_unknown:
        status = (
            f"{filesystem} read-only summary · exact allocation totals not decoded"
        )
    elif labels:
        status = (
            f"{filesystem} allocation map · available: {', '.join(labels)} · "
            f"{human_bytes(used_bytes)} allocated{unknown}"
        )
    else:
        status = (
            f"{filesystem} read-only allocation map · "
            f"{human_bytes(used_bytes)} allocated{unknown}"
        )

    if is_swap and bool(details.get("active")):
        analysis_log = (
            f"Analysis complete: active SWAP uses {human_bytes(used_bytes)} "
            f"across {used_pages:,} {page_word}. Aggregate usage is known; "
            "physical occupied slot locations are unavailable. Swap stores "
            "page slots rather than files, so file fragmentation does not apply."
        )
    elif is_swap:
        analysis_log = (
            "Analysis complete: inactive SWAP has no occupied pages; "
            "the header, bad pages, free pages and outside tail are mapped exactly. "
            "Swap stores page slots rather than files, so file fragmentation "
            "does not apply."
        )
    elif has_fragmentation:
        analysis_log = (
            f"Analysis complete: {_required_int(data, 'fragmented_files')} fragmented files, "
            f"{_required_int(data, 'fragmented_directories')} fragmented directories."
        )
    elif full_allocation_unknown:
        analysis_log = (
            f"Analysis complete: {filesystem} detected; exact allocated/free totals "
            "are not decoded by this read-only backend."
        )
    else:
        availability = (
            " Available: " + ", ".join(labels) + "."
            if labels
            else " Read-only analysis backend."
        )
        analysis_log = (
            f"Analysis complete: {human_bytes(used_bytes)} allocated, "
            f"{human_bytes(free_bytes)} free.{availability}"
        )

    return MapPresentation(
        cells=cells,
        cell_count=cell_count,
        capacity_title="Capacity",
        capacity_value=human_bytes(total_bytes),
        free_title="Free space",
        free_value=free_value,
        files_title=files_title,
        files_value=files_value,
        fragmentation_title="Fragmentation",
        fragmentation_value=fragmentation_value,
        unit_label=unit_label,
        caption=caption,
        status=status,
        analysis_log=analysis_log,
    )


def _fat_presentation(
    data: dict[str, Any],
    cells: list[dict[str, int]],
    cell_count: int,
) -> MapPresentation:
    filesystem = str(data.get("filesystem") or "fat32").upper()
    cluster_size = _required_int(data, "cluster_size", minimum=1)
    data_clusters = _required_int(data, "data_clusters", minimum=1)
    free_clusters = _required_int(data, "free_clusters")
    if free_clusters > data_clusters:
        raise AllocationMapError("free cluster count exceeds filesystem capacity")
    files = _required_int(data, "regular_files")
    directories = _required_int(data, "directories")
    fragmented = _required_int(data, "fragmented_files")
    fragmented_directories = _required_int(data, "fragmented_directories")
    total_bytes = cluster_size * data_clusters
    free_bytes = cluster_size * free_clusters
    units_per_cell = data_clusters / cell_count
    return MapPresentation(
        cells=cells,
        cell_count=cell_count,
        capacity_title="Capacity",
        capacity_value=human_bytes(total_bytes),
        free_title="Free space",
        free_value=(
            f"{human_bytes(free_bytes)} "
            f"({free_clusters * 100.0 / data_clusters:.1f}%)"
        ),
        files_title="Files",
        files_value=f"{files:,} files · {directories:,} dirs",
        fragmentation_title="Fragmentation",
        fragmentation_value=(
            f"{fragmented:,} files · {fragmented_directories:,} dirs"
        ),
        unit_label="clusters",
        caption=(
            f"Allocation image: {cell_count:,} cells · approximately "
            f"{units_per_cell:,.1f} clusters per cell"
        ),
        status=(
            f"{filesystem} {data.get('volume_id', 'unknown')} · "
            f"{fragmented} fragmented files · "
            f"{_required_int(data, 'free_gaps_below_highest'):,} "
            "free clusters below the high-water mark"
        ),
        analysis_log=(
            f"Analysis complete: {fragmented} fragmented files, "
            f"{fragmented_directories} fragmented directories."
        ),
    )


def present_allocation_map(data: dict[str, Any], operations: Iterable[str] = ()) -> MapPresentation:
    """Return a validated, filesystem-neutral view model for the GTK window."""

    if not isinstance(data, dict):
        raise AllocationMapError("allocation-map result is not an object")
    cells, cell_count = _validated_cells(data)
    if str(data.get("backend", "")) == "read-only-domain":
        return _domain_presentation(data, operations, cells, cell_count)
    return _fat_presentation(data, cells, cell_count)
