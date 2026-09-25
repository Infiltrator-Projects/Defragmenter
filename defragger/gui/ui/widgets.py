# SPDX-License-Identifier: GPL-3.0-or-later
"""Reusable GTK widgets for the main window."""

from __future__ import annotations

import math
from typing import Any

from gi.repository import Gdk, GdkPixbuf, GLib, Gtk

from .map_geometry import physical_unit_at_pixel, source_cell_at_pixel
from .theme_tokens import TYPOGRAPHY

MIN_MAP_CELLS = 256
MAX_MAP_CELLS = 1048576


class DiskMap(Gtk.DrawingArea):
    """Render allocation data as a dense physically ordered pixel image."""

    COLORS = {
        "free": (0.018, 0.050, 0.105),
        "outside": (0.006, 0.014, 0.030),
        "used": (0.020, 0.520, 1.000),
        "fragmented": (1.000, 0.145, 0.235),
        "directory": (0.620, 0.170, 0.980),
        "unknown": (0.285, 0.330, 0.410),
        "bad": (1.000, 0.625, 0.040),
        "background": (0.010, 0.020, 0.040),
    }

    def __init__(self) -> None:
        super().__init__()
        self.cells: list[dict[str, int]] = []
        self.unit_label = "clusters"
        self._raster_size: tuple[int, int] | None = None
        self._cell_starts: list[int] = []
        self._cell_ends: list[int] = []
        self._pixbuf: GdkPixbuf.Pixbuf | None = None
        self._pixbuf_key: tuple[int, int, int] | None = None
        self.set_size_request(-1, 250)
        self.set_has_tooltip(True)
        self.connect("draw", self._draw)
        self.connect("query-tooltip", self._query_tooltip)

    def set_cells(self, cells: list[dict[str, int]]) -> None:
        self.cells = cells
        self._cell_starts = [int(cell["start"]) for cell in cells]
        self._cell_ends = [int(cell["end"]) for cell in cells]
        self._pixbuf = None
        self._pixbuf_key = None
        self.queue_draw()

    def set_unit_label(self, label: str) -> None:
        self.unit_label = label

    def desired_cell_count(
        self,
        width: int | None = None,
        height: int | None = None,
    ) -> int:
        if width is None or height is None:
            allocation = self.get_allocation()
            width, height = allocation.width, allocation.height
        drawable_pixels = max(1, int(width)) * max(1, int(height))
        return max(MIN_MAP_CELLS, min(MAX_MAP_CELLS, drawable_pixels))

    @staticmethod
    def _mix(
        a: tuple[float, float, float],
        b: tuple[float, float, float],
        ratio: float,
    ) -> tuple[float, float, float]:
        ratio = max(0.0, min(1.0, ratio))
        return tuple(
            a[index] * (1.0 - ratio) + b[index] * ratio
            for index in range(3)
        )

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
        colour = self._mix(
            free_colour,
            self.COLORS["used"],
            used / known_total,
        )
        overlay_minimum = {
            "directory": 0.58,
            "fragmented": 0.70,
            "bad": 0.58,
        }
        for name in ("directory", "fragmented", "bad"):
            amount = int(cell.get(name, 0))
            if amount:
                ratio = min(1.0, (amount / known_total) ** 0.5)
                ratio = max(overlay_minimum[name], ratio)
                colour = self._mix(colour, self.COLORS[name], ratio)
        unknown = int(cell.get("unknown", 0))
        total = free + outside + used + unknown
        if unknown and total:
            colour = self._mix(
                colour,
                self.COLORS["unknown"],
                unknown / total,
            )
        return colour

    def _build_pixbuf(self, width: int, height: int) -> GdkPixbuf.Pixbuf:
        """Build one exact positional raster from physical allocation order."""

        width = max(1, int(width))
        height = max(1, int(height))
        key = (len(self.cells), width, height)
        if self._pixbuf is not None and self._pixbuf_key == key:
            return self._pixbuf

        pixel_count = width * height
        pixels = bytearray(pixel_count * 3)
        first_unit = self._cell_starts[0]
        last_unit = self._cell_ends[-1]

        # Both the analyser contract and map presenter guarantee monotonically
        # ordered physical cell bounds.  Walk them once while the display
        # pixels advance monotonically through the real on-disk unit range.
        # This is deliberately row-major: no Hilbert/Morton/other spatial curve
        # may move an allocation unit to a different apparent disk position.
        cell_index = 0
        for pixel_index in range(pixel_count):
            x = pixel_index % width
            y = pixel_index // width
            unit = physical_unit_at_pixel(
                first_unit,
                last_unit,
                width,
                height,
                x,
                y,
            )
            assert unit is not None
            while (
                cell_index + 1 < len(self.cells)
                and unit > self._cell_ends[cell_index]
            ):
                cell_index += 1

            if (
                unit < self._cell_starts[cell_index]
                or unit > self._cell_ends[cell_index]
            ):
                colour = self.COLORS["background"]
            else:
                colour = self._cell_colour(self.cells[cell_index])

            rgb = bytes(
                max(0, min(255, int(round(channel * 255.0))))
                for channel in colour
            )
            offset = pixel_index * 3
            pixels[offset : offset + 3] = rgb

        rowstride = width * 3
        data = GLib.Bytes.new(bytes(pixels))
        self._pixbuf = GdkPixbuf.Pixbuf.new_from_bytes(
            data,
            GdkPixbuf.Colorspace.RGB,
            False,
            8,
            width,
            height,
            rowstride,
        )
        self._pixbuf_key = key
        return self._pixbuf

    def _draw(self, widget: Gtk.Widget, cr: Any) -> bool:
        allocation = widget.get_allocation()
        width = max(1, allocation.width)
        height = max(1, allocation.height)
        self._raster_size = (width, height)
        cr.set_source_rgb(*self.COLORS["background"])
        cr.paint()

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

        pixbuf = self._build_pixbuf(width, height)

        # Pixbuf dimensions equal the drawing surface.  Paint 1:1 with no
        # interpolation so category boundaries stay on their exact physical
        # pixels instead of being blurred into neighbouring disk positions.
        Gdk.cairo_set_source_pixbuf(cr, pixbuf, 0.0, 0.0)
        cr.paint()

        # Data-neutral lighting only; it never changes allocation placement.
        cr.set_source_rgba(0.05, 0.58, 1.0, 0.045)
        cr.rectangle(0, 0, width, max(1, height // 5))
        cr.fill()
        return False

    def _source_at(self, x: int, y: int) -> int | None:
        if self._raster_size is None or not self.cells:
            return None
        width, height = self._raster_size
        return source_cell_at_pixel(
            self._cell_starts,
            self._cell_ends,
            width,
            height,
            x,
            y,
        )

    def _query_tooltip(
        self,
        _widget: Gtk.Widget,
        x: int,
        y: int,
        _keyboard_mode: bool,
        tooltip: Gtk.Tooltip,
    ) -> bool:
        index = self._source_at(x, y)
        if index is None:
            return False
        cell = self.cells[index]
        tooltip.set_text(
            f"{self.unit_label.capitalize()} {cell['start']:,}–{cell['end']:,}\n"
            f"Used {cell['used']:,} · Free {cell['free']:,} · "
            f"Outside filesystem {cell.get('outside', 0):,} · "
            f"Unknown {cell.get('unknown', 0):,}\n"
            f"Fragmented {cell['fragmented']:,} · "
            f"Directory {cell['directory']:,} · "
            f"Metadata/reserved {cell.get('bad', 0):,}"
        )
        return True


class HeroArtwork(Gtk.DrawingArea):
    """Decorative data-neutral hero artwork for the selected-volume banner."""

    def __init__(self) -> None:
        super().__init__()
        self.set_size_request(-1, 136)
        self.connect("draw", self._draw)

    @staticmethod
    def _draw(_widget: Gtk.Widget, cr: Any) -> bool:
        allocation = _widget.get_allocation()
        width = max(1, allocation.width)
        height = max(1, allocation.height)

        # Deep blue field.
        cr.set_source_rgb(0.025, 0.070, 0.145)
        cr.paint()

        # Sunset glow at the right, built from translucent concentric discs.
        cx = width * 0.82
        cy = height * 0.42
        for radius, alpha in ((120, 0.025), (86, 0.045), (54, 0.070), (22, 0.16)):
            cr.set_source_rgba(1.0, 0.55, 0.16, alpha)
            cr.arc(cx, cy, radius, 0.0, math.tau)
            cr.fill()

        # Stylised mountain silhouettes and reflected water bands.
        cr.set_source_rgba(0.02, 0.04, 0.10, 0.88)
        cr.move_to(width * 0.43, height * 0.72)
        points = (
            (0.50, 0.50), (0.56, 0.61), (0.62, 0.35),
            (0.67, 0.57), (0.72, 0.42), (0.78, 0.64),
            (0.85, 0.46), (0.91, 0.62), (1.00, 0.50),
        )
        for px, py in points:
            cr.line_to(width * px, height * py)
        cr.line_to(width, height)
        cr.line_to(width * 0.43, height)
        cr.close_path()
        cr.fill()

        for index, alpha in enumerate((0.20, 0.14, 0.10, 0.07)):
            y = height * (0.73 + index * 0.06)
            cr.set_source_rgba(0.05, 0.58, 1.0, alpha)
            cr.rectangle(width * 0.42, y, width * 0.58, max(1.0, height * 0.018))
            cr.fill()
        cr.set_source_rgba(1.0, 0.64, 0.18, 0.18)
        cr.rectangle(width * 0.68, height * 0.78, width * 0.30, max(1.0, height * 0.02))
        cr.fill()
        return False


class CheckerBall(Gtk.DrawingArea):
    """Small Workbench-inspired decorative checker sphere."""

    def __init__(self) -> None:
        super().__init__()
        self.set_size_request(150, 150)
        self.connect("draw", self._draw)

    @staticmethod
    def _draw(widget: Gtk.Widget, cr: Any) -> bool:
        allocation = widget.get_allocation()
        size = min(allocation.width, allocation.height)
        radius = max(10.0, size * 0.40)
        cx = allocation.width / 2.0
        cy = allocation.height / 2.0

        cr.save()
        cr.arc(cx, cy, radius, 0.0, math.tau)
        cr.clip()
        tile = max(10.0, radius / 3.0)
        left = cx - radius
        top = cy - radius
        rows = int((radius * 2.0) / tile) + 2
        cols = rows
        for row in range(rows):
            for col in range(cols):
                red = (row + col) % 2 == 0
                cr.set_source_rgb(
                    0.92 if red else 0.95,
                    0.10 if red else 0.95,
                    0.12 if red else 0.95,
                )
                cr.rectangle(left + col * tile, top + row * tile, tile, tile)
                cr.fill()
        cr.restore()

        cr.set_line_width(3.0)
        cr.set_source_rgba(0.18, 0.55, 1.0, 0.75)
        cr.arc(cx, cy, radius, 0.0, math.tau)
        cr.stroke()
        return False


class GaugeCard(Gtk.Frame):
    """Compact graphical summary card with a truthful radial proportion."""

    def __init__(
        self,
        title: str,
        accent: tuple[float, float, float],
        accent_class: str = "",
    ) -> None:
        super().__init__()
        self._fraction = 0.0
        self._accent = accent
        self.set_shadow_type(Gtk.ShadowType.NONE)
        self.get_style_context().add_class("summary-card")
        self.get_style_context().add_class("gauge-card")
        if accent_class:
            self.get_style_context().add_class(accent_class)

        box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        box.set_border_width(10)
        self.gauge = Gtk.DrawingArea()
        self.gauge.set_size_request(82, 82)
        self.gauge.connect("draw", self._draw_gauge)
        box.pack_start(self.gauge, False, False, 0)

        labels = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        labels.set_valign(Gtk.Align.CENTER)
        self.title = Gtk.Label(label=title)
        self.title.set_xalign(0)
        self.title.get_style_context().add_class("summary-title")
        self.value = Gtk.Label(label="—")
        self.value.set_xalign(0)
        self.value.set_line_wrap(True)
        self.value.get_style_context().add_class("summary-value")
        self.detail = Gtk.Label(label="")
        self.detail.set_xalign(0)
        self.detail.set_line_wrap(True)
        self.detail.get_style_context().add_class("summary-detail")
        labels.pack_start(self.title, False, False, 0)
        labels.pack_start(self.value, False, False, 0)
        labels.pack_start(self.detail, False, False, 0)
        box.pack_start(labels, True, True, 0)
        self.add(box)

    def _draw_gauge(self, widget: Gtk.Widget, cr: Any) -> bool:
        allocation = widget.get_allocation()
        width = max(1, allocation.width)
        height = max(1, allocation.height)
        cx = width / 2.0
        cy = height / 2.0
        radius = max(8.0, min(width, height) / 2.0 - 8.0)
        line = max(7.0, radius * 0.22)
        cr.set_line_width(line)
        cr.set_line_cap(1)
        cr.set_source_rgba(0.35, 0.42, 0.52, 0.30)
        cr.arc(cx, cy, radius, -math.pi / 2.0, 3.0 * math.pi / 2.0)
        cr.stroke()

        if self._fraction > 0.0:
            cr.set_source_rgba(*self._accent, 0.18)
            cr.set_line_width(line * 1.75)
            cr.arc(
                cx,
                cy,
                radius,
                -math.pi / 2.0,
                -math.pi / 2.0 + 2.0 * math.pi * self._fraction,
            )
            cr.stroke()
            cr.set_line_width(line)
            cr.set_source_rgb(*self._accent)
            cr.arc(
                cx,
                cy,
                radius,
                -math.pi / 2.0,
                -math.pi / 2.0 + 2.0 * math.pi * self._fraction,
            )
            cr.stroke()

        cr.select_font_face(TYPOGRAPHY["ui_family"], 0, 1)
        cr.set_font_size(15)
        percent = f"{self._fraction * 100.0:.0f}%"
        extents = cr.text_extents(percent)
        cr.move_to(
            cx - extents.width / 2.0 - extents.x_bearing,
            cy - extents.height / 2.0 - extents.y_bearing,
        )
        cr.set_source_rgb(0.92, 0.95, 0.99)
        cr.show_text(percent)
        return False

    def set_title(self, title: str) -> None:
        self.title.set_text(title)

    def set_value(self, value: str) -> None:
        self.value.set_text(value)

    def set_detail(self, detail: str) -> None:
        self.detail.set_text(detail)

    def set_fraction(self, fraction: float) -> None:
        self._fraction = max(0.0, min(1.0, float(fraction)))
        self.gauge.queue_draw()


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
