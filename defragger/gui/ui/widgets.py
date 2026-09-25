# SPDX-License-Identifier: GPL-3.0-or-later
"""Reusable GTK widgets for the main window."""

from __future__ import annotations

from typing import Any

from gi.repository import Gtk

from .theme_tokens import TYPOGRAPHY

MIN_MAP_CELLS = 256
MAX_MAP_CELLS = 1048576


class DiskMap(Gtk.DrawingArea):
    """Render allocation data as a dense pixel raster, never a visible grid."""

    COLORS = {
        "free": (0.035, 0.055, 0.090),
        "outside": (0.015, 0.022, 0.035),
        "used": (0.045, 0.535, 0.980),
        "fragmented": (1.000, 0.205, 0.235),
        "directory": (0.565, 0.240, 0.930),
        "unknown": (0.310, 0.355, 0.420),
        "bad": (1.000, 0.650, 0.080),
        "background": (0.020, 0.030, 0.050),
    }

    def __init__(self) -> None:
        super().__init__()
        self.cells: list[dict[str, int]] = []
        self.unit_label = "clusters"
        self._raster_size: tuple[int, int] | None = None
        # Keep a useful compact minimum while allowing the expanding map panel
        # to consume all additional space.  A 260-pixel hard minimum combined
        # with the log made the top-level frame taller than some work areas and
        # caused window managers to reject otherwise valid maximise requests.
        self.set_size_request(-1, 210)
        self.set_has_tooltip(True)
        self.connect("draw", self._draw)
        self.connect("query-tooltip", self._query_tooltip)

    def set_cells(self, cells: list[dict[str, int]]) -> None:
        self.cells = cells
        self.queue_draw()

    def set_unit_label(self, label: str) -> None:
        self.unit_label = label

    def desired_cell_count(self, width: int | None = None, height: int | None = None) -> int:
        if width is None or height is None:
            allocation = self.get_allocation()
            width, height = allocation.width, allocation.height
        drawable_pixels = max(1, int(width)) * max(1, int(height))
        return max(MIN_MAP_CELLS, min(MAX_MAP_CELLS, drawable_pixels))

    @staticmethod
    def _mix(a: tuple[float, float, float], b: tuple[float, float, float], ratio: float):
        ratio = max(0.0, min(1.0, ratio))
        return tuple(a[index] * (1.0 - ratio) + b[index] * ratio for index in range(3))

    def _cell_colour(self, cell: dict[str, int]) -> tuple[float, float, float]:
        free = int(cell.get("free", 0))
        outside = int(cell.get("outside", 0))
        used = int(cell.get("used", 0))
        free_like = free + outside
        known_total = max(1, free_like + used)
        free_colour = self._mix(
            self.COLORS["free"],
            self.COLORS["outside"],
            outside / max(1, free_like),
        )
        colour = self._mix(free_colour, self.COLORS["used"], used / known_total)
        overlay_minimum = {"directory": 0.58, "fragmented": 0.62, "bad": 0.52}
        for name in ("directory", "fragmented", "bad"):
            amount = int(cell.get(name, 0))
            if amount:
                ratio = min(1.0, (amount / known_total) ** 0.5)
                # A directory or metadata extent can occupy only one unit inside
                # a many-unit display cell.  Keep it visibly identifiable rather
                # than blending it into ordinary blue allocation.
                ratio = max(overlay_minimum[name], ratio)
                colour = self._mix(colour, self.COLORS[name], ratio)
        unknown = int(cell.get("unknown", 0))
        total = free + outside + used + unknown
        if unknown and total:
            colour = self._mix(colour, self.COLORS["unknown"], unknown / total)
        return colour

    @staticmethod
    def _source_index(
        cell_count: int,
        width: int,
        height: int,
        x: int,
        y: int,
    ) -> int:
        pixel_count = max(1, width * height)
        pixel_index = y * width + x
        return min(cell_count - 1, (pixel_index * cell_count) // pixel_count)

    def _draw(self, widget: Gtk.Widget, cr: Any) -> bool:
        allocation = widget.get_allocation()
        width, height = max(1, allocation.width), max(1, allocation.height)
        self._raster_size = (width, height)
        cr.set_source_rgb(*self.COLORS["background"])
        cr.rectangle(0, 0, width, height)
        cr.fill()
        if not self.cells:
            cr.set_source_rgb(0.56, 0.62, 0.70)
            cr.select_font_face(TYPOGRAPHY["ui_family"], 0, 0)
            cr.set_font_size(15)
            message = "Select a supported volume and click Analyse"
            extents = cr.text_extents(message)
            cr.move_to(
                (width - extents.width) / 2 - extents.x_bearing,
                (height - extents.height) / 2 - extents.y_bearing,
            )
            cr.show_text(message)
            return False

        # The map is intentionally a raster rather than a chessboard of UI
        # controls.  Cells are streamed row-major into the physical drawing
        # pixels, with adjacent pixels of the same source cell coalesced into
        # one Cairo run.  That keeps the display truthful while making a large
        # allocation map read as a continuous image instead of an old-style
        # block grid.
        cell_count = len(self.cells)
        total_pixels = width * height
        for row in range(height):
            row_start = row * width
            column = 0
            while column < width:
                pixel_index = row_start + column
                source_index = min(
                    cell_count - 1,
                    (pixel_index * cell_count) // total_pixels,
                )
                next_source_pixel = (
                    ((source_index + 1) * total_pixels + cell_count - 1)
                    // cell_count
                )
                run_end = min(
                    width,
                    max(column + 1, next_source_pixel - row_start),
                )
                cr.set_source_rgb(*self._cell_colour(self.cells[source_index]))
                cr.rectangle(
                    float(column),
                    float(row),
                    float(run_end - column),
                    1.0,
                )
                cr.fill()
                column = run_end
        return False


    def _query_tooltip(
        self,
        _widget: Gtk.Widget,
        x: int,
        y: int,
        _keyboard_mode: bool,
        tooltip: Gtk.Tooltip,
    ) -> bool:
        if not self.cells or self._raster_size is None:
            return False
        width, height = self._raster_size
        if x < 0 or y < 0 or x >= width or y >= height:
            return False
        index = self._source_index(len(self.cells), width, height, x, y)
        cell = self.cells[index]
        tooltip.set_text(
            f"{self.unit_label.capitalize()} {cell['start']:,}–{cell['end']:,}\n"
            f"Used {cell['used']:,} · Free {cell['free']:,} · "
            f"Outside filesystem {cell.get('outside', 0):,} · Unknown {cell.get('unknown', 0):,}\n"
            f"Fragmented {cell['fragmented']:,} · Directory {cell['directory']:,} · "
            f"Metadata/reserved {cell.get('bad', 0):,}"
        )
        return True



class SummaryCard(Gtk.Frame):
    def __init__(self, title: str, accent_class: str = "") -> None:
        super().__init__()
        self.set_shadow_type(Gtk.ShadowType.NONE)
        self.get_style_context().add_class("summary-card")
        if accent_class:
            self.get_style_context().add_class(accent_class)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=3)
        box.set_border_width(12)
        self.title = Gtk.Label(label=title)
        self.title.set_xalign(0)
        self.title.get_style_context().add_class("summary-title")
        self.value = Gtk.Label(label="—")
        self.value.set_xalign(0)
        self.value.get_style_context().add_class("summary-value")
        box.pack_start(self.title, False, False, 0)
        box.pack_start(self.value, False, False, 0)
        self.add(box)

    def set_title(self, title: str) -> None:
        self.title.set_text(title)

    def set_value(self, value: str) -> None:
        self.value.set_text(value)
