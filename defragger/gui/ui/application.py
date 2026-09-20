# SPDX-License-Identifier: GPL-3.0-or-later
"""GTK application lifecycle separated from the per-window controller."""

from __future__ import annotations

import sys

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk

from .icon_assets import apply_default_window_icon, configure_desktop_identity
from .theme import apply_theme, load_theme_mode
from .window import MainWindow


APP_ID = "io.github.linuxdefragger"


class LinuxDefraggerApplication(Gtk.Application):
    def __init__(self) -> None:
        super().__init__(application_id=APP_ID, flags=0)
        apply_default_window_icon()
        self.windows: list[MainWindow] = []

    def new_window(self) -> None:
        apply_theme(load_theme_mode())
        try:
            window = MainWindow(self)
        except Exception as exc:
            dialog = Gtk.MessageDialog(
                transient_for=None,
                modal=True,
                message_type=Gtk.MessageType.ERROR,
                buttons=Gtk.ButtonsType.CLOSE,
                text="Unable to start Defragmenter",
            )
            dialog.format_secondary_text(str(exc))
            dialog.run()
            dialog.destroy()
            return
        self.windows.append(window)
        window.connect("destroy", self._window_destroyed)
        window.show_all()
        window.present()

    def _window_destroyed(self, window: MainWindow) -> None:
        if window in self.windows:
            self.windows.remove(window)

    def do_activate(self) -> None:
        if not self.windows:
            self.new_window()
        else:
            self.windows[-1].present()


def main(argv: list[str] | None = None) -> int:
    configure_desktop_identity()
    app = LinuxDefraggerApplication()
    return app.run(argv or sys.argv)
