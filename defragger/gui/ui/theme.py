# SPDX-License-Identifier: GPL-3.0-or-later
"""Runtime theme policy for Defragmenter.

Common owns semantic Day/Night palette values. This module owns only GTK
selector mechanics, user preference persistence and the platform-authoritative
Follow system mode.
"""

from __future__ import annotations

from enum import Enum
from pathlib import Path
import os

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import Gdk, Gtk

from .theme_tokens import DAY, METRICS, NIGHT, TYPOGRAPHY

_CONFIG_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "linux-defragger"
_CONFIG_FILE = _CONFIG_DIR / "theme"

_provider: Gtk.CssProvider | None = None
_gtk_settings: Gtk.Settings | None = None
_theme_signal_ids: list[int] = []



class ThemeMode(str, Enum):
    SYSTEM = "system"
    DAY = "day"
    NIGHT = "night"


def theme_label(mode: ThemeMode) -> str:
    return {
        ThemeMode.SYSTEM: "Follow system",
        ThemeMode.DAY: "Day",
        ThemeMode.NIGHT: "Night",
    }[mode]


def load_theme_mode() -> ThemeMode:
    try:
        value = _CONFIG_FILE.read_text(encoding="utf-8").strip()
        return ThemeMode(value)
    except (OSError, ValueError):
        return ThemeMode.SYSTEM


def save_theme_mode(mode: ThemeMode) -> None:
    try:
        _CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        _CONFIG_FILE.write_text(mode.value + "\n", encoding="utf-8")
    except OSError:
        # Appearance persistence must never stop a storage/safety tool starting.
        pass


def _system_prefers_dark() -> bool:
    settings = Gtk.Settings.get_default()
    if settings is None:
        return False
    try:
        if bool(settings.get_property("gtk-application-prefer-dark-theme")):
            return True
    except (TypeError, AttributeError):
        pass
    try:
        return "dark" in str(settings.get_property("gtk-theme-name") or "").lower()
    except (TypeError, AttributeError):
        return False


def _system_theme_changed(*_args: object) -> None:
    if load_theme_mode() is ThemeMode.SYSTEM:
        apply_theme(ThemeMode.SYSTEM)


def _ensure_system_theme_watch() -> None:
    global _gtk_settings
    if _gtk_settings is not None:
        return
    settings = Gtk.Settings.get_default()
    if settings is None:
        return
    _gtk_settings = settings
    _theme_signal_ids.extend([
        settings.connect(
            "notify::gtk-application-prefer-dark-theme",
            _system_theme_changed,
        ),
        settings.connect("notify::gtk-theme-name", _system_theme_changed),
    ])


def _base_css() -> str:
    """Return the application typography contract.

    Defragmenter ships the three approved MB Corpo faces in every supported
    package. Keep typography deterministic and do not add generic/system
    fallback families here.
    """
    body = TYPOGRAPHY["ui_family"]
    brand = TYPOGRAPHY["brand_family"]
    regular = TYPOGRAPHY["ui_regular_weight"]
    bold = TYPOGRAPHY["ui_bold_weight"]
    brand_weight = TYPOGRAPHY["brand_weight"]
    radius = METRICS["small_radius"]
    return f"""
    * {{ font-family: "{body}"; font-weight: {regular}; }}
    .app-title, .about-title {{
        font-family: "{brand}";
        font-weight: {brand_weight};
    }}
    .app-title {{ font-size: 23pt; }}
    .app-subtitle {{ font-size: 9.5pt; }}
    .summary-title {{ font-size: 8.75pt; }}
    .summary-value {{ font-size: 15pt; font-weight: {bold}; }}
    .section-title {{ font-size: 9.5pt; font-weight: {bold}; padding: 0 5px; }}
    button {{ border-radius: {radius}px; padding: 7px 13px; min-height: 30px; }}
    button.primary-action, button.operation-action {{ font-weight: {bold}; }}
    progressbar trough {{ min-height: 8px; }}
    .status-prefix {{ font-size: 8pt; font-weight: {bold}; }}
    .status-text {{ font-size: 8.75pt; }}
    """


def _night_css() -> str:
    p = NIGHT
    return f"""
    window, dialog, .background, .app-shell {{ background-color: {p["background"]}; color: {p["text"]}; }}
    headerbar, .titlebar {{ background-image: none; background-color: {p["titlebar"]}; color: {p["heading"]}; border-bottom: 1px solid {p["status_border"]}; }}
    menubar, .app-menubar {{ background-color: {p["connection"]}; border-bottom: 1px solid {p["connection_border"]}; }}
    menu {{ background-color: {p["panel"]}; border: 1px solid {p["border"]}; }}
    menuitem:hover {{ background-color: {p["operation_hover"]}; }}
    .app-title, .about-title, .summary-value {{ color: {p["heading"]}; }}
    .app-subtitle {{ color: {p["summary"]}; }}
    .summary-title, .map-caption {{ color: {p["detail_label"]}; }}
    .status-text {{ color: {p["note"]}; }}
    .section-title {{ color: {p["kicker"]}; }}
    .legend-item label, .log-expander {{ color: {p["detail_label"]}; }}
    .version-badge, frame.section-panel > border, frame.map-panel > border,
    frame.action-panel > border, frame.summary-card > border {{
        background-color: {p["surface"]}; border: 1px solid {p["border"]};
    }}
    button, combobox button, entry, spinbutton {{
        background-image: none; background-color: {p["card"]}; color: {p["text"]};
        border: 1px solid {p["neutral_accent"]}; box-shadow: none;
    }}
    button:hover {{ background-color: {p["card_hover"]}; border-color: {p["accent_hover"]}; }}
    button:active, button:checked {{ background-color: {p["selection_background"]}; border-color: {p["neutral_accent"]}; }}
    button:disabled {{ color: {p["subtle"]}; border-color: {p["border"]}; background-color: {p["input"]}; }}
    button.primary-action {{ background-color: {p["button_background"]}; color: {p["button_foreground"]}; border-color: {p["button_background"]}; }}
    button.primary-action:hover {{ background-color: {p["equals_hover"]}; color: {p["button_foreground"]}; }}
    button.destructive-action {{ border-color: {p["fault"]}; color: {p["fault"]}; }}
    button.destructive-action:hover {{ background-color: {p["surface_hover"]}; border-color: {p["fault"]}; }}
    progressbar trough {{ background-color: {p["panel"]}; border: 1px solid {p["border"]}; }}
    progressbar progress {{ background-color: {p["neutral_accent"]}; }}
    textview, textview text, treeview, viewport, scrolledwindow {{
        background-color: {p["input"]}; color: {p["text"]}; border-color: {p["border"]};
    }}
    textview.log-view, textview.log-view text {{ background-color: {p["background"]}; color: {p["text"]}; }}
    entry selection, textview text selection, treeview.view:selected {{
        background-color: {p["selection_background"]}; color: {p["selection_foreground"]};
    }}
    .status-strip {{ background-color: {p["connection"]}; border-top: 1px solid {p["status_border"]}; }}
    scrollbar slider {{ background-color: {p["neutral_accent"]}; }}
    scrollbar slider:hover {{ background-color: {p["accent_hover"]}; }}
    tooltip {{ background-color: {p["card"]}; color: {p["note"]}; border: 1px solid {p["status_border"]}; }}
    .link-about-dialog {{ background-color: {p["background"]}; color: {p["text"]}; }}
    .link-about-dialog label {{ color: {p["text"]}; }}
    """


def _day_css() -> str:
    p = DAY
    return f"""
    window, dialog, .background, .app-shell {{ background-color: {p["background"]}; color: {p["text"]}; }}
    headerbar, .titlebar {{ background-image: none; background-color: {p["titlebar"]}; color: {p["heading"]}; border-bottom: 1px solid {p["status_border"]}; }}
    menubar, .app-menubar {{ background-color: {p["connection"]}; border-bottom: 1px solid {p["connection_border"]}; }}
    menu {{ background-color: {p["panel"]}; border: 1px solid {p["border"]}; }}
    menuitem:hover {{ background-color: {p["surface"]}; }}
    .app-title, .about-title, .summary-value {{ color: {p["heading"]}; }}
    .app-subtitle {{ color: {p["summary"]}; }}
    .summary-title, .map-caption {{ color: {p["detail_label"]}; }}
    .status-text {{ color: {p["note"]}; }}
    .section-title {{ color: {p["kicker"]}; }}
    .legend-item label, .log-expander {{ color: {p["detail_label"]}; }}
    .version-badge, frame.section-panel > border, frame.map-panel > border,
    frame.action-panel > border, frame.summary-card > border {{
        background-color: {p["panel"]}; border: 1px solid {p["border"]};
    }}
    button, combobox button, entry, spinbutton {{
        background-image: none; background-color: {p["card"]}; color: {p["text"]};
        border: 1px solid {p["neutral_accent"]}; box-shadow: none;
    }}
    button:hover {{ background-color: {p["card_hover"]}; border-color: {p["accent_hover"]}; }}
    button:active, button:checked {{ background-color: {p["selection_background"]}; border-color: {p["neutral_accent"]}; }}
    button:disabled {{ color: {p["subtle"]}; border-color: {p["border"]}; background-color: {p["background"]}; }}
    button.primary-action {{ background-color: {p["button_background"]}; color: {p["button_foreground"]}; border-color: {p["button_background"]}; }}
    button.primary-action:hover {{ background-color: {p["equals_hover"]}; color: {p["button_foreground"]}; }}
    button.destructive-action {{ border-color: {p["fault"]}; color: {p["fault"]}; }}
    button.destructive-action:hover {{ background-color: {p["surface_hover"]}; border-color: {p["fault"]}; }}
    progressbar trough {{ background-color: {p["surface"]}; border: 1px solid {p["border"]}; }}
    progressbar progress {{ background-color: {p["neutral_accent"]}; }}
    textview, textview text, treeview, viewport, scrolledwindow {{
        background-color: {p["panel"]}; color: {p["text"]}; border-color: {p["border"]};
    }}
    textview.log-view, textview.log-view text {{ background-color: {p["panel"]}; color: {p["text"]}; }}
    entry selection, textview text selection, treeview.view:selected {{
        background-color: {p["selection_background"]}; color: {p["selection_foreground"]};
    }}
    .status-strip {{ background-color: {p["connection"]}; border-top: 1px solid {p["status_border"]}; }}
    scrollbar slider {{ background-color: {p["neutral_accent"]}; }}
    scrollbar slider:hover {{ background-color: {p["accent_hover"]}; }}
    tooltip {{ background-color: {p["card"]}; color: {p["note"]}; border: 1px solid {p["status_border"]}; }}
    .link-about-dialog {{ background-color: {p["background"]}; color: {p["text"]}; }}
    .link-about-dialog label {{ color: {p["text"]}; }}
    """


def apply_theme(mode: ThemeMode | str | None = None) -> ThemeMode:
    """Apply Common Day/Night; System resolves the host preference to one of them."""
    global _provider

    resolved = load_theme_mode() if mode is None else ThemeMode(mode)
    screen = Gdk.Screen.get_default()
    if screen is None:
        return resolved

    _ensure_system_theme_watch()
    effective = (
        ThemeMode.NIGHT if resolved is ThemeMode.SYSTEM and _system_prefers_dark()
        else ThemeMode.DAY if resolved is ThemeMode.SYSTEM
        else resolved
    )

    css = _base_css()
    css += _night_css() if effective is ThemeMode.NIGHT else _day_css()

    if _provider is None:
        _provider = Gtk.CssProvider()
        Gtk.StyleContext.add_provider_for_screen(
            screen,
            _provider,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION + 50,
        )
    _provider.load_from_data(css.encode("utf-8"))
    return resolved
