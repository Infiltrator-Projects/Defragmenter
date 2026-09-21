# SPDX-License-Identifier: GPL-3.0-or-later
"""Suite-standard native About presentation for Defragmenter."""

from __future__ import annotations

from dataclasses import dataclass
from gi.repository import Gtk

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
    version: str
    description: str
    build: str = ""
    release_date: str = ""
    authors: tuple[str, ...] = ()
    copyright: str = ""
    website: str = ""
    license_name: str = ""
    license_text: str = ""
    credits: str = ""


def _about_logo():
    """Load the approved Defragmenter artwork at the shared About size."""
    return load_app_icon_pixbuf(96)


def show_common_about(parent: Gtk.Window, info: AboutInfo) -> None:
    """Render the suite-standard native GTK3 About contract."""
    comments = [info.description] if info.description else []
    if info.build:
        comments.append(f"Build: {info.build}")
    if info.release_date:
        comments.append(f"Release date: {info.release_date}")

    dialog = Gtk.AboutDialog(
        transient_for=parent,
        modal=True,
        program_name=info.product_name,
        version=info.version,
        comments="\n\n".join(comments),
        website=info.website or None,
        website_label="Website" if info.website else None,
        copyright=info.copyright or None,
    )
    dialog.set_title(f"About {info.product_name}")
    dialog.set_destroy_with_parent(True)
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


class SuiteStandardWindowView(WindowView):
    """Window view whose About surface follows the suite-wide System Monitor standard."""

    def show_about(self) -> None:
        show_common_about(
            self.window,
            AboutInfo(
                product_name=APP_NAME,
                version=self.gui_version,
                description=ABOUT_COMMENTS,
                build=self.build_label,
                authors=("Shannon Smith — Author and project maintainer",),
                copyright=COPYRIGHT,
                website=PROJECT_URL,
                license_name="GPL-3.0-or-later",
                license_text=ABOUT_LICENSE,
            ),
        )
