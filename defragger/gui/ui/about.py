# SPDX-License-Identifier: GPL-3.0-or-later
"""LINK-standard About presentation for Defragmenter."""

from __future__ import annotations

from dataclasses import dataclass
from gi.repository import Gdk, Gtk

from .icon_assets import apply_window_icon, load_app_icon_pixbuf
from .window_view import (
    ABOUT_COMMENTS,
    ABOUT_LICENSE,
    APP_ICON_NAME,
    APP_NAME,
    COPYRIGHT,
    PROJECT_URL,
    WindowView,
)


@dataclass(frozen=True)
class AboutInfo:
    """Python GTK3 mirror of LINK's LinkAboutInfo presentation contract."""

    product_name: str
    subtitle: str
    version: str
    description: str
    release_date: str = ""
    authors: tuple[str, ...] = ()
    copyright: str = ""
    website: str = ""
    license_name: str = ""
    license_text: str = ""
    credits: str = ""


_about_provider: Gtk.CssProvider | None = None


def _apply_about_style() -> None:
    """Apply the same LINK/InfiltratorFS About chrome at app priority."""
    global _about_provider
    if _about_provider is not None:
        return
    screen = Gdk.Screen.get_default()
    if screen is None:
        return
    css = b"""
    .link-about-dialog image {
        margin-top: 12px;
        margin-bottom: 8px;
    }
    .link-about-dialog stackswitcher {
        margin: 6px 12px 8px 12px;
    }
    .link-about-dialog scrolledwindow {
        min-width: 500px;
        min-height: 300px;
    }
    """
    provider = Gtk.CssProvider()
    provider.load_from_data(css)
    Gtk.StyleContext.add_provider_for_screen(
        screen,
        provider,
        Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION + 120,
    )
    _about_provider = provider


def _about_logo():
    """Load the approved Defragmenter artwork at the shared About size."""
    return load_app_icon_pixbuf(96)


def show_common_about(parent: Gtk.Window, info: AboutInfo) -> None:
    """Render the shared LINK About contract with the native GTK3 shell."""
    _apply_about_style()
    comments = [part for part in (info.subtitle, info.description) if part]
    if info.release_date:
        comments.append(f"Released: {info.release_date}")
    if info.credits:
        comments.append(f"Credits: {info.credits}")

    dialog = Gtk.AboutDialog(
        transient_for=parent,
        modal=True,
        program_name=info.product_name,
        version=info.version,
        comments="\n\n".join(comments),
        website=info.website or None,
        website_label="Project website" if info.website else None,
        copyright=info.copyright or None,
    )
    dialog.set_title(f"About {info.product_name}")
    dialog.set_destroy_with_parent(True)
    dialog.set_resizable(True)
    dialog.set_default_size(560, 520)
    dialog.set_size_request(520, 480)
    dialog.get_style_context().add_class("link-about-dialog")
    apply_window_icon(dialog)
    if info.authors:
        dialog.set_authors(list(info.authors))
    if info.license_text:
        dialog.set_license(info.license_text)
        dialog.set_wrap_license(True)
    elif info.license_name:
        dialog.set_license(info.license_name)
    logo = _about_logo()
    dialog.set_logo_icon_name(None)
    if logo is not None:
        dialog.set_logo(logo)
    else:
        # Installed packages always ship the matching hicolor name. Keep this
        # as a final theme fallback for source-tree or damaged-install runs.
        dialog.set_logo_icon_name(APP_ICON_NAME)
    dialog.run()
    dialog.destroy()


class LinkStandardWindowView(WindowView):
    """Window view whose About surface follows the shared LINK product standard."""

    def show_about(self) -> None:
        show_common_about(
            self.window,
            AboutInfo(
                product_name=APP_NAME,
                subtitle="DEFRAGMENTER · NATIVE FILESYSTEM OPTIMISATION",
                version=self.gui_version,
                description=ABOUT_COMMENTS,
                authors=("Shannon Smith — Author and project maintainer",),
                copyright=COPYRIGHT,
                website=PROJECT_URL,
                license_name="GPL-3.0-or-later",
                license_text=ABOUT_LICENSE,
            ),
        )
