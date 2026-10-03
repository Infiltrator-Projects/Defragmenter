# SPDX-License-Identifier: GPL-3.0-or-later
"""Artwork helpers for retained Python presentation fixtures."""

from __future__ import annotations

from pathlib import Path

from gi.repository import GdkPixbuf, GLib, Gtk

from .window_view import APP_ICON_NAME


_MODULE_PATH = Path(__file__).resolve()
_PRIVATE_ICON_PATH = _MODULE_PATH.parents[1] / "defragmenter-icon.png"
_CANONICAL_PRIVATE_ICON_PATH = Path(
    "/usr/lib/linux-defragger/defragmenter-icon.png"
)
_HICOLOR_ICON_PATH = Path(
    "/usr/share/icons/hicolor/96x96/apps/io.github.linuxdefragger.png"
)
_SOURCE_ICON_PATH = (
    _MODULE_PATH.parents[2] / "packaging" / "io.github.linuxdefragger.png"
)


def app_icon_paths() -> tuple[Path, ...]:
    """Return deterministic icon candidates in runtime preference order."""
    ordered = (
        _PRIVATE_ICON_PATH,
        _CANONICAL_PRIVATE_ICON_PATH,
        _HICOLOR_ICON_PATH,
        _SOURCE_ICON_PATH,
    )
    unique: list[Path] = []
    seen: set[str] = set()
    for path in ordered:
        key = str(path)
        if key not in seen:
            seen.add(key)
            unique.append(path)
    return tuple(unique)


def load_app_icon_pixbuf(size: int):
    """Load the approved artwork directly before consulting the icon theme."""
    for path in app_icon_paths():
        if not path.is_file():
            continue
        try:
            return GdkPixbuf.Pixbuf.new_from_file_at_scale(
                str(path),
                size,
                size,
                True,
            )
        except GLib.Error:
            continue

    theme = Gtk.IconTheme.get_default()
    if theme is None:
        return None
    try:
        return theme.load_icon(
            APP_ICON_NAME,
            size,
            Gtk.IconLookupFlags.FORCE_SIZE,
        )
    except GLib.Error:
        return None


def apply_window_icon(window: Gtk.Window) -> None:
    """Set one GTK window icon from the same approved artwork."""
    icon = load_app_icon_pixbuf(256)
    if icon is not None:
        window.set_icon(icon)
        return
    window.set_icon_name(APP_ICON_NAME)
