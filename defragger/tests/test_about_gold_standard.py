#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Source-contract regression for the shared LINK-style About surface."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ABOUT = (ROOT / "gui" / "ui" / "about.py").read_text()
ICON_ASSETS = (ROOT / "gui" / "ui" / "icon_assets.py").read_text()
APPLICATION = (ROOT / "gui" / "ui" / "application.py").read_text()
WINDOW = (ROOT / "gui" / "ui" / "window.py").read_text()
DESKTOP = (ROOT / "packaging" / "io.github.linuxdefragger.desktop").read_text()

for required in (
    "class AboutInfo:",
    "class LinkStandardWindowView(WindowView):",
    "Gtk.AboutDialog(",
    'add_class("link-about-dialog")',
    "dialog.set_default_size(560, 520)",
    "dialog.set_size_request(520, 480)",
    "dialog.set_authors(list(info.authors))",
    "dialog.set_license(info.license_text)",
    "dialog.set_wrap_license(True)",
    'website_label="Project website"',
    "load_app_icon_pixbuf(96)",
    "dialog.set_logo_icon_name(None)",
    "apply_window_icon(dialog)",
    'subtitle="DEFRAGMENTER · NATIVE FILESYSTEM OPTIMISATION"',
    '"Shannon Smith — Author and project maintainer"',
):
    assert required in ABOUT, required

# GTK3's named-logo property overrides the pixbuf logo. Clear it before
# installing the project-owned pixbuf and retain the canonical name fallback.
assert ABOUT.index("dialog.set_logo_icon_name(None)") < ABOUT.index("dialog.set_logo(logo)")
assert "dialog.set_logo_icon_name(APP_ICON_NAME)" in ABOUT

for required in (
    '"/usr/lib/linux-defragger/defragmenter-icon.png"',
    '"/usr/share/icons/hicolor/256x256/apps/io.github.linuxdefragger.png"',
    '"packaging" / "io.github.linuxdefragger.png"',
    "GdkPixbuf.Pixbuf.new_from_file_at_scale",
    "GLib.set_prgname(APP_ICON_NAME)",
    "GLib.set_application_name(APP_NAME)",
    "Gdk.set_program_class(APP_ICON_NAME)",
    "Gtk.Window.set_default_icon(icon)",
    "window.set_icon(icon)",
    "Gtk.Window.set_default_icon_name(APP_ICON_NAME)",
    "window.set_icon_name(APP_ICON_NAME)",
):
    assert required in ICON_ASSETS, required

assert "apply_default_window_icon()" in APPLICATION
assert "configure_desktop_identity()" in APPLICATION
assert "StartupWMClass=io.github.linuxdefragger" in DESKTOP
assert "from .about import LinkStandardWindowView" in WINDOW
assert "apply_window_icon(self)" in WINDOW
assert "self.view = LinkStandardWindowView(" in WINDOW
assert "self.view = WindowView(" not in WINDOW

print("LINK-standard About presentation contract passed")
