# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure physical-position mapping for the allocation raster and tooltips."""

from __future__ import annotations

from bisect import bisect_right
from collections.abc import Sequence


def physical_unit_at_pixel(
    first_unit: int,
    last_unit: int,
    width: int,
    height: int,
    x: int,
    y: int,
) -> int | None:
    """Map one display pixel to the physical allocation unit it represents.

    Pixels are row-major and monotonic: the first display pixel represents the
    lowest on-disk unit and the last represents the highest.  No space-filling
    curve or visual reordering is permitted here because the allocation map is
    a positional view of the real volume.
    """

    width = max(1, int(width))
    height = max(1, int(height))
    if (
        first_unit < 0
        or last_unit < first_unit
        or x < 0
        or y < 0
        or x >= width
        or y >= height
    ):
        return None
    pixel_count = width * height
    pixel_index = int(y) * width + int(x)
    unit_count = last_unit - first_unit + 1
    return first_unit + min(
        unit_count - 1,
        (pixel_index * unit_count) // pixel_count,
    )


def source_cell_for_unit(
    starts: Sequence[int],
    ends: Sequence[int],
    unit: int,
) -> int | None:
    """Return the ordered source cell containing one physical unit."""

    if not starts or len(starts) != len(ends):
        return None
    index = bisect_right(starts, int(unit)) - 1
    if index < 0 or int(unit) > int(ends[index]):
        return None
    return index


def source_cell_at_pixel(
    starts: Sequence[int],
    ends: Sequence[int],
    width: int,
    height: int,
    x: int,
    y: int,
) -> int | None:
    """Map a pointer pixel to the exact physical source cell rendered there."""

    if not starts or len(starts) != len(ends):
        return None
    unit = physical_unit_at_pixel(
        int(starts[0]),
        int(ends[-1]),
        width,
        height,
        x,
        y,
    )
    if unit is None:
        return None
    return source_cell_for_unit(starts, ends, unit)
