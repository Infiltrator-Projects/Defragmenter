# SPDX-License-Identifier: GPL-3.0-or-later
"""Reusable GTK widgets for the main window."""

from __future__ import annotations

import math
from typing import Any

from gi.repository import Gdk, GdkPixbuf, GLib, Gtk

from .theme_tokens import TYPOGRAPHY

MIN_MAP_CELLS = 256
MAX_MAP_CELLS = 1048576


class DiskMap(Gtk.DrawingArea):
    """Render allocation data as a dense locality-preserving pixel image."""

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
        self._logical_layout: tuple[int, int, int, int] | None = None
        self._pixbuf: GdkPixbuf.Pixbuf | None = None
        self._pixbuf_key: tuple[int, int, int] | None = None
        self.set_size_request(-1, 250)
        self.set_has_tooltip(True)
        self.connect("draw", self._draw)
        self.connect("query-tooltip", self._query_tooltip)

    def set_cells(self, cells: list[dict[str, int]]) -> None:
        self.cells = cells
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

    @staticmethod
    def _layout_bits(
        cell_count: int,
        width: int,
        height: int,
    ) -> tuple[int, int]:
        bits = max(1, int(math.ceil(math.log2(max(1, cell_count)))))
        aspect = max(0.25, min(4.0, width / max(1.0, float(height))))
        aspect_bits = int(round(math.log2(aspect)))
        x_bits = max(0, min(bits, (bits + aspect_bits + 1) // 2))
        y_bits = bits - x_bits
        if x_bits == 0:
            x_bits, y_bits = 1, max(0, bits - 1)
        if y_bits == 0 and height > width // 4:
            y_bits, x_bits = 1, max(1, bits - 1)
        return x_bits, y_bits

    @staticmethod
    def _morton_xy(index: int, x_bits: int, y_bits: int) -> tuple[int, int]:
        x = 0
        y = 0
        source_bit = 0
        x_bit = 0
        y_bit = 0
        while x_bit < x_bits or y_bit < y_bits:
            if x_bit < x_bits:
                x |= ((index >> source_bit) & 1) << x_bit
                source_bit += 1
                x_bit += 1
            if y_bit < y_bits:
                y |= ((index >> source_bit) & 1) << y_bit
                source_bit += 1
                y_bit += 1
        return x, y

    @staticmethod
    def _morton_index(x: int, y: int, x_bits: int, y_bits: int) -> int:
        index = 0
        target_bit = 0
        for bit in range(max(x_bits, y_bits)):
            if bit < x_bits:
                index |= ((x >> bit) & 1) << target_bit
                target_bit += 1
            if bit < y_bits:
                index |= ((y >> bit) & 1) << target_bit
                target_bit += 1
        return index

    def _build_pixbuf(self, width: int, height: int) -> GdkPixbuf.Pixbuf:
        x_bits, y_bits = self._layout_bits(len(self.cells), width, height)
        logical_width = 1 << x_bits
        logical_height = 1 << y_bits
        self._logical_layout = (
            logical_width,
            logical_height,
            x_bits,
            y_bits,
        )
        key = (len(self.cells), logical_width, logical_height)
        if self._pixbuf is not None and self._pixbuf_key == key:
            return self._pixbuf

        background = tuple(
            max(0, min(255, int(round(channel * 255.0))))
            for channel in self.COLORS["background"]
        )
        pixels = bytearray(background * (logical_width * logical_height))
        for source_index, cell in enumerate(self.cells):
            x, y = self._morton_xy(source_index, x_bits, y_bits)
            if x >= logical_width or y >= logical_height:
                continue
            colour = tuple(
                max(0, min(255, int(round(channel * 255.0))))
                for channel in self._cell_colour(cell)
            )
            offset = (y * logical_width + x) * 3
            pixels[offset : offset + 3] = bytes(colour)

        rowstride = logical_width * 3
        data = GLib.Bytes.new(bytes(pixels))
        self._pixbuf = GdkPixbuf.Pixbuf.new_from_bytes(
            data,
            GdkPixbuf.Colorspace.RGB,
            False,
            8,
            logical_width,
            logical_height,
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
        logical_width = max(1, pixbuf.get_width())
        logical_height = max(1, pixbuf.get_height())
        cr.save()
        cr.scale(width / logical_width, height / logical_height)
        Gdk.cairo_set_source_pixbuf(cr, pixbuf, 0.0, 0.0)
        pattern = cr.get_source()
        try:
            pattern.set_filter(0)  # cairo.FILTER_FAST / nearest-like scaling.
        except (AttributeError, TypeError):
            pass
        cr.paint()
        cr.restore()

        # Subtle luminous overlay gives the allocation raster depth without
        # inventing data or drawing a visible cell grid.
        gradient = cr.get_source()
        cr.set_source_rgba(0.20, 0.55, 1.0, 0.045)
        cr.rectangle(0, 0, width, max(1, height // 3))
        cr.fill()
        del gradient
        return False

    def _source_at(self, x: int, y: int) -> int | None:
        if (
            self._raster_size is None
            or self._logical_layout is None
            or not self.cells
        ):
            return None
        width, height = self._raster_size
        logical_width, logical_height, x_bits, y_bits = self._logical_layout
        if x < 0 or y < 0 or x >= width or y >= height:
            return None
        logical_x = min(
            logical_width - 1,
            int(x * logical_width / max(1, width)),
        )
        logical_y = min(
            logical_height - 1,
            int(y * logical_height / max(1, height)),
        )
        source_index = self._morton_index(
            logical_x,
            logical_y,
            x_bits,
            y_bits,
        )
        return source_index if source_index < len(self.cells) else None

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
