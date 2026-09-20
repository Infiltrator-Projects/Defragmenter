# SPDX-License-Identifier: GPL-3.0-or-later
"""GTK widget construction and presentation for one Defragmenter window."""

from __future__ import annotations

from typing import Any, Protocol

from gi.repository import Gdk, Gtk

from .map_presenter import MapPresentation
from .operation_planner import ControlState
from .theme import ThemeMode, apply_theme, load_theme_mode, save_theme_mode, theme_label
from .widgets import DiskMap, SummaryCard


APP_NAME = "Defragmenter"
APP_ICON_NAME = "io.github.linuxdefragger"
PROJECT_URL = "https://github.com/Infiltrator-Projects/Defragmenter"
COPYRIGHT = "Copyright © 1993-2026 Shannon Smith"
ABOUT_COMMENTS = (
    "A C-first Linux filesystem allocation analyser and offline defragmenter "
    "authored by Shannon Smith."
)
ABOUT_LICENSE = (
    "Defragmenter is free software licensed under the GNU General Public "
    "License version 3 or, at your option, any later version "
    "(GPL-3.0-or-later).\n\n"
    "See LICENSE in the source package or COPYING.GPL-3.0 in the installed "
    "documentation for the complete licence text."
)


class WindowActions(Protocol):
    """User-intent interface implemented by the window composition root."""

    def refresh_devices(
        self,
        preserve_path: str | None = None,
        clear_cache: bool = False,
    ) -> None: ...

    def on_device_changed(self, combo: Gtk.ComboBoxText) -> None: ...

    def open_image(self, button: Gtk.Widget) -> None: ...

    def unmount_selected(self, button: Gtk.Button) -> None: ...

    def on_map_size_allocate(
        self,
        widget: Gtk.Widget,
        allocation: Gdk.Rectangle,
    ) -> None: ...

    def analyze(
        self,
        clear_log: bool = True,
        target_cells: int | None = None,
        quiet: bool = False,
    ) -> None: ...

    def start_mutation(self, operation: str) -> None: ...

    def request_stop(self, button: Gtk.Button) -> None: ...


class WindowView:
    """Own every GTK widget; delegate user intent to the window controller."""

    def __init__(
        self,
        window: Gtk.ApplicationWindow,
        controller: WindowActions,
        *,
        gui_version: str,
        engine_version: str,
        build_label: str,
    ) -> None:
        self.window = window
        self.controller = controller
        self.gui_version = gui_version
        self.engine_version = engine_version
        self.build_label = build_label
        self._build()
        self._load_css()

    @staticmethod
    def _section_label(text: str) -> Gtk.Label:
        label = Gtk.Label(label=text)
        label.get_style_context().add_class("section-title")
        return label

    def _build(self) -> None:
        outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        outer.get_style_context().add_class("app-shell")
        self.window.add(outer)
        outer.pack_start(self._build_menu_bar(), False, False, 0)

        body_scroll = Gtk.ScrolledWindow()
        body_scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        body_scroll.set_shadow_type(Gtk.ShadowType.NONE)
        body_scroll.get_style_context().add_class("body-scroll")
        outer.pack_start(body_scroll, True, True, 0)

        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        root.set_border_width(14)
        body_scroll.add(root)

        title_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=16)
        title_row.set_border_width(2)
        title_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=3)

        title = Gtk.Label(label="Defragmenter")
        title.set_xalign(0)
        title.get_style_context().add_class("app-title")
        title_box.pack_start(title, False, False, 0)

        subtitle = Gtk.Label(
            label=(
                "Analyse allocation, fully pack and defragment supported "
                "filesystems, or create 10% growth-space layouts"
            )
        )
        subtitle.set_xalign(0)
        subtitle.set_line_wrap(True)
        subtitle.get_style_context().add_class("app-subtitle")
        title_box.pack_start(subtitle, False, False, 0)
        title_row.pack_start(title_box, True, True, 0)

        version_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=1)
        version_box.set_border_width(8)
        version_box.get_style_context().add_class("version-badge")
        version_title = Gtk.Label(label=f"Engine {self.engine_version}")
        version_title.set_xalign(1)
        version_title.get_style_context().add_class("version-primary")
        version_gui = Gtk.Label(label=f"GUI {self.gui_version}")
        version_gui.set_xalign(1)
        version_gui.get_style_context().add_class("version-secondary")
        version_box.pack_start(version_title, False, False, 0)
        version_box.pack_start(version_gui, False, False, 0)
        title_row.pack_end(version_box, False, False, 0)
        root.pack_start(title_row, False, False, 0)

        device_frame = Gtk.Frame()
        device_frame.set_label_widget(self._section_label("Volume"))
        device_frame.get_style_context().add_class("section-panel")
        device_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        device_box.set_border_width(10)
        self.device_combo = Gtk.ComboBoxText()
        self.device_combo.set_hexpand(True)
        self.device_combo.connect("changed", self.controller.on_device_changed)
        device_box.pack_start(self.device_combo, True, True, 0)
        self.refresh_button = Gtk.Button.new_with_label("Refresh")
        self.refresh_button.connect(
            "clicked",
            lambda _button: self.controller.refresh_devices(clear_cache=True),
        )
        device_box.pack_start(self.refresh_button, False, False, 0)
        self.image_button = Gtk.Button.new_with_label("Open image…")
        self.image_button.connect("clicked", self.controller.open_image)
        device_box.pack_start(self.image_button, False, False, 0)
        self.unmount_button = Gtk.Button.new_with_label("Unmount")
        self.unmount_button.connect("clicked", self.controller.unmount_selected)
        device_box.pack_start(self.unmount_button, False, False, 0)
        device_frame.add(device_box)
        root.pack_start(device_frame, False, False, 0)

        cards = Gtk.Grid(column_spacing=10, row_spacing=10)
        cards.set_column_homogeneous(True)
        self.capacity_card = SummaryCard("Capacity")
        self.free_card = SummaryCard("Free space")
        self.files_card = SummaryCard("Files")
        self.fragmented_card = SummaryCard("Fragmentation")
        cards.attach(self.capacity_card, 0, 0, 1, 1)
        cards.attach(self.free_card, 1, 0, 1, 1)
        cards.attach(self.files_card, 2, 0, 1, 1)
        cards.attach(self.fragmented_card, 3, 0, 1, 1)
        root.pack_start(cards, False, False, 0)

        map_frame = Gtk.Frame()
        map_frame.set_label_widget(self._section_label("Allocation map"))
        map_frame.get_style_context().add_class("map-panel")
        map_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        map_box.set_border_width(10)

        self.disk_map = DiskMap()
        self.disk_map.connect(
            "size-allocate", self.controller.on_map_size_allocate
        )
        map_box.pack_start(self.disk_map, True, True, 0)

        legend = Gtk.FlowBox()
        legend.set_selection_mode(Gtk.SelectionMode.NONE)
        legend.set_homogeneous(False)
        legend.set_row_spacing(6)
        legend.set_column_spacing(14)
        legend.set_min_children_per_line(2)
        legend.set_max_children_per_line(7)
        legend.get_style_context().add_class("legend-strip")
        for label, colour in (
            ("Free", DiskMap.COLORS["free"]),
            ("Outside active filesystem", DiskMap.COLORS["outside"]),
            ("Used", DiskMap.COLORS["used"]),
            ("Fragmented", DiskMap.COLORS["fragmented"]),
            ("Directory", DiskMap.COLORS["directory"]),
            ("Unknown", DiskMap.COLORS["unknown"]),
            ("Filesystem metadata/reserved", DiskMap.COLORS["bad"]),
        ):
            item = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=5)
            item.get_style_context().add_class("legend-item")
            swatch = Gtk.DrawingArea()
            swatch.set_size_request(14, 14)
            swatch.connect("draw", self._draw_swatch, colour)
            item.pack_start(swatch, False, False, 0)
            item.pack_start(Gtk.Label(label=label), False, False, 0)
            legend.insert(item, -1)
        map_box.pack_start(legend, False, False, 0)

        self.map_caption = Gtk.Label(
            label="Each square represents a range of filesystem allocation units."
        )
        self.map_caption.set_xalign(0)
        self.map_caption.set_line_wrap(True)
        self.map_caption.get_style_context().add_class("map-caption")
        map_box.pack_start(self.map_caption, False, False, 0)

        map_frame.add(map_box)
        root.pack_start(map_frame, True, True, 0)

        action_frame = Gtk.Frame()
        action_frame.set_label_widget(self._section_label("Operations"))
        action_frame.get_style_context().add_class("action-panel")
        action_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        action_box.set_border_width(10)

        action_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.analyze_button = Gtk.Button.new_with_label("Analyse")
        self.analyze_button.connect(
            "clicked", lambda _button: self.controller.analyze()
        )
        self.analyze_button.get_style_context().add_class("primary-action")
        action_row.pack_start(self.analyze_button, False, False, 0)

        self.defrag_button = Gtk.Button.new_with_label("Defragment")
        self.defrag_button.connect(
            "clicked",
            lambda _button: self.controller.start_mutation("defrag"),
        )
        self.defrag_button.get_style_context().add_class("operation-action")
        action_row.pack_start(self.defrag_button, False, False, 0)

        self.growth_button = Gtk.Button.new_with_label("Growth Defrag")
        self.growth_button.connect(
            "clicked",
            lambda _button: self.controller.start_mutation("growth-defrag"),
        )
        self.growth_button.get_style_context().add_class("operation-action")
        action_row.pack_start(self.growth_button, False, False, 0)

        self.recover_button = Gtk.Button.new_with_label("Recover")
        self.recover_button.connect(
            "clicked",
            lambda _button: self.controller.start_mutation("recover"),
        )
        action_row.pack_start(self.recover_button, False, False, 0)

        separator = Gtk.Separator(orientation=Gtk.Orientation.VERTICAL)
        action_row.pack_start(separator, False, False, 4)

        self.stop_button = Gtk.Button.new_with_label("Stop safely")
        self.stop_button.connect("clicked", self.controller.request_stop)
        self.stop_button.set_sensitive(False)
        self.stop_button.get_style_context().add_class("destructive-action")
        action_row.pack_start(self.stop_button, False, False, 0)
        action_box.pack_start(action_row, False, False, 0)

        self.progress = Gtk.ProgressBar()
        self.progress.set_hexpand(True)
        self.progress.set_show_text(True)
        self.progress.set_text("Ready")
        self.progress.get_style_context().add_class("operation-progress")
        action_box.pack_start(self.progress, False, False, 0)
        action_frame.add(action_box)
        root.pack_start(action_frame, False, False, 0)

        expander = Gtk.Expander(label="Operation log")
        expander.set_expanded(True)
        expander.get_style_context().add_class("log-expander")
        scroll = Gtk.ScrolledWindow()
        scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        # Leave enough vertical flexibility for the top-level frame to resize
        # below the desktop work area.  The allocation map receives surplus
        # height first, so the log stays useful without fixing window geometry.
        scroll.set_min_content_height(110)
        scroll.get_style_context().add_class("log-scroll")
        self.log_view = Gtk.TextView()
        self.log_view.set_editable(False)
        self.log_view.set_cursor_visible(False)
        self.log_view.set_left_margin(10)
        self.log_view.set_right_margin(10)
        self.log_view.set_top_margin(8)
        self.log_view.set_bottom_margin(8)
        self.log_view.get_style_context().add_class("log-view")
        self.log_buffer = self.log_view.get_buffer()
        scroll.add(self.log_view)
        expander.add(scroll)
        root.pack_start(expander, False, True, 0)

        status_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        status_box.set_border_width(6)
        status_box.get_style_context().add_class("status-strip")
        status_prefix = Gtk.Label(label="STATUS")
        status_prefix.get_style_context().add_class("status-prefix")
        status_box.pack_start(status_prefix, False, False, 0)
        self.status_label = Gtk.Label(label="Ready")
        self.status_label.set_xalign(0)
        self.status_label.get_style_context().add_class("status-text")
        status_box.pack_start(self.status_label, True, True, 0)
        root.pack_start(status_box, False, False, 0)

    def _build_menu_bar(self) -> Gtk.MenuBar:
        menu_bar = Gtk.MenuBar()
        menu_bar.get_style_context().add_class("app-menubar")

        file_item = Gtk.MenuItem.new_with_mnemonic("_File")
        file_menu = Gtk.Menu()
        file_item.set_submenu(file_menu)

        new_window_item = Gtk.MenuItem.new_with_mnemonic("_New window")
        new_window_item.connect(
            "activate",
            lambda _item: self.window.get_application().new_window(),
        )
        file_menu.append(new_window_item)
        open_item = Gtk.MenuItem.new_with_mnemonic("_Open image…")
        open_item.connect("activate", self.controller.open_image)
        file_menu.append(open_item)
        refresh_item = Gtk.MenuItem.new_with_mnemonic("_Refresh volumes")
        refresh_item.connect(
            "activate",
            lambda _item: self.controller.refresh_devices(clear_cache=True),
        )
        file_menu.append(refresh_item)
        file_menu.append(Gtk.SeparatorMenuItem())
        quit_item = Gtk.MenuItem.new_with_mnemonic("_Quit")
        quit_item.connect("activate", lambda _item: self.window.close())
        file_menu.append(quit_item)

        view_item = Gtk.MenuItem.new_with_mnemonic("_View")
        view_menu = Gtk.Menu()
        view_item.set_submenu(view_menu)

        theme_item = Gtk.MenuItem.new_with_label("Theme")
        theme_menu = Gtk.Menu()
        theme_item.set_submenu(theme_menu)
        self._theme_items: dict[ThemeMode, Gtk.RadioMenuItem] = {}
        group: Gtk.RadioMenuItem | None = None
        active_mode = load_theme_mode()
        for mode in (ThemeMode.SYSTEM, ThemeMode.DAY, ThemeMode.NIGHT):
            item = (
                Gtk.RadioMenuItem.new_with_label_from_widget(group, theme_label(mode))
                if group is not None
                else Gtk.RadioMenuItem.new_with_label(None, theme_label(mode))
            )
            if group is None:
                group = item
            item.set_active(mode is active_mode)
            item.connect("toggled", self._on_theme_toggled, mode)
            self._theme_items[mode] = item
            theme_menu.append(item)
        view_menu.append(theme_item)

        about_item = Gtk.MenuItem.new_with_mnemonic("_About")
        about_menu = Gtk.Menu()
        about_item.set_submenu(about_menu)
        about_dialog_item = Gtk.MenuItem.new_with_label("About Defragmenter")
        about_dialog_item.connect("activate", lambda _item: self.show_about())
        about_menu.append(about_dialog_item)

        menu_bar.append(file_item)
        menu_bar.append(view_item)
        menu_bar.append(about_item)
        return menu_bar

    def _on_theme_toggled(
        self,
        item: Gtk.RadioMenuItem,
        mode: ThemeMode,
    ) -> None:
        if not item.get_active():
            return
        save_theme_mode(mode)
        apply_theme(mode)
        application = self.window.get_application()
        if application is None:
            return
        for window in application.get_windows():
            view = getattr(window, "view", None)
            if view is not None and hasattr(view, "sync_theme_menu"):
                view.sync_theme_menu(mode)

    def sync_theme_menu(self, mode: ThemeMode) -> None:
        item = getattr(self, "_theme_items", {}).get(mode)
        if item is not None and not item.get_active():
            item.set_active(True)

    @staticmethod
    def _draw_swatch(
        _widget: Gtk.Widget,
        cr: Any,
        colour: tuple[float, float, float],
    ) -> bool:
        cr.set_source_rgb(*colour)
        cr.rectangle(0, 0, 14, 14)
        cr.fill()
        return False

    def _load_css(self) -> None:
        """Install layout/typography rules; colour policy lives in theme.py."""
        css = b"""
        .app-title { font-size: 21pt; font-weight: bold; }
        .app-subtitle { font-size: 10pt; }
        .version-primary { font-size: 9.5pt; font-weight: bold; }
        .version-secondary { font-size: 9pt; }
        .section-title { font-size: 10pt; font-weight: bold; }
        .summary-title { font-size: 9.5pt; }
        .summary-value { font-size: 16pt; font-weight: bold; }
        .legend-item label, .map-caption { font-size: 9pt; }
        button.primary-action, button.operation-action { font-weight: bold; }
        .log-expander { font-weight: bold; }
        textview.log-view { font-size: 9.5pt; }
        .status-prefix { font-size: 8.5pt; font-weight: bold; }
        .status-text { font-size: 9pt; }
        .about-title { font-size: 19pt; font-weight: bold; }
        .about-version, .about-copy, .about-meta-key, .about-meta-value { font-size: 9.5pt; }
        """
        provider = Gtk.CssProvider()
        provider.load_from_data(css)
        screen = Gdk.Screen.get_default()
        if screen is not None:
            Gtk.StyleContext.add_provider_for_screen(
                screen,
                provider,
                Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION,
            )

    def populate_volumes(self, names: list[str], active_index: int) -> None:
        self.device_combo.remove_all()
        for name in names:
            self.device_combo.append_text(name)
        self.device_combo.set_active(active_index)

    def append_log(self, text: str) -> None:
        if not text:
            return
        end = self.log_buffer.get_end_iter()
        self.log_buffer.insert(end, text if text.endswith("\n") else text + "\n")
        mark = self.log_buffer.create_mark(
            None, self.log_buffer.get_end_iter(), False
        )
        self.log_view.scroll_mark_onscreen(mark)
        self.log_buffer.delete_mark(mark)

    def clear_log(self) -> None:
        self.log_buffer.set_text("")

    def show_error(self, title: str, message: str) -> None:
        dialog = Gtk.MessageDialog(
            transient_for=self.window,
            modal=True,
            message_type=Gtk.MessageType.ERROR,
            buttons=Gtk.ButtonsType.CLOSE,
            text=title,
        )
        dialog.format_secondary_text(message)
        dialog.run()
        dialog.destroy()

    def confirm(self, title: str, message: str) -> bool:
        dialog = Gtk.MessageDialog(
            transient_for=self.window,
            modal=True,
            message_type=Gtk.MessageType.WARNING,
            buttons=Gtk.ButtonsType.CANCEL,
            text=title,
        )
        dialog.format_secondary_text(message)
        dialog.add_button("Proceed", Gtk.ResponseType.OK)
        response = dialog.run()
        dialog.destroy()
        return response == Gtk.ResponseType.OK

    def choose_image(self) -> str | None:
        chooser = Gtk.FileChooserDialog(
            title="Open filesystem image",
            transient_for=self.window,
            action=Gtk.FileChooserAction.OPEN,
        )
        chooser.add_buttons(
            Gtk.STOCK_CANCEL,
            Gtk.ResponseType.CANCEL,
            Gtk.STOCK_OPEN,
            Gtk.ResponseType.OK,
        )
        response = chooser.run()
        filename = (
            chooser.get_filename() if response == Gtk.ResponseType.OK else None
        )
        chooser.destroy()
        return filename

    def _show_license(self, parent: Gtk.Window) -> None:
        dialog = Gtk.Dialog(
            title="Defragmenter licence",
            transient_for=parent,
            modal=True,
        )
        dialog.add_button("Close", Gtk.ResponseType.CLOSE)
        dialog.set_default_size(520, 300)
        content = dialog.get_content_area()
        content.set_border_width(18)
        content.set_spacing(10)
        text = Gtk.Label(label=ABOUT_LICENSE)
        text.set_xalign(0)
        text.set_yalign(0)
        text.set_line_wrap(True)
        text.set_selectable(True)
        text.get_style_context().add_class("about-copy")
        content.pack_start(text, True, True, 0)
        dialog.show_all()
        dialog.run()
        dialog.destroy()

    def show_about(self) -> None:
        dialog = Gtk.Dialog(
            title=f"About {APP_NAME}",
            transient_for=self.window,
            modal=True,
        )
        dialog.set_default_size(540, 470)
        dialog.add_button("Licence", 1)
        dialog.add_button("Close", Gtk.ResponseType.CLOSE)

        content = dialog.get_content_area()
        content.set_border_width(24)
        content.set_spacing(16)

        hero = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=18)
        image = Gtk.Image.new_from_icon_name(APP_ICON_NAME, Gtk.IconSize.DIALOG)
        image.set_pixel_size(88)
        hero.pack_start(image, False, False, 0)

        title_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=3)
        title = Gtk.Label(label=APP_NAME)
        title.set_xalign(0)
        title.get_style_context().add_class("about-title")
        title_box.pack_start(title, False, False, 0)
        version = Gtk.Label(label=f"Version {self.gui_version}")
        version.set_xalign(0)
        version.get_style_context().add_class("about-version")
        title_box.pack_start(version, False, False, 0)
        hero.pack_start(title_box, True, True, 0)
        content.pack_start(hero, False, False, 0)

        description = Gtk.Label(label=ABOUT_COMMENTS)
        description.set_xalign(0)
        description.set_line_wrap(True)
        description.get_style_context().add_class("about-copy")
        content.pack_start(description, False, False, 0)

        details = Gtk.Grid(column_spacing=14, row_spacing=8)
        details.set_hexpand(True)
        for row, (key, value) in enumerate(
            (
                ("Author", "Shannon Smith — Author and project maintainer"),
                ("Build", self.build_label),
                ("Engine", self.engine_version),
                ("Copyright", COPYRIGHT),
            )
        ):
            key_label = Gtk.Label(label=key)
            key_label.set_xalign(0)
            key_label.get_style_context().add_class("about-meta-key")
            value_label = Gtk.Label(label=value)
            value_label.set_xalign(0)
            value_label.set_line_wrap(True)
            value_label.get_style_context().add_class("about-meta-value")
            details.attach(key_label, 0, row, 1, 1)
            details.attach(value_label, 1, row, 1, 1)
        content.pack_start(details, False, False, 0)

        website = Gtk.LinkButton.new_with_label(PROJECT_URL, "Project website")
        website.set_halign(Gtk.Align.START)
        content.pack_start(website, False, False, 0)

        dialog.show_all()
        while True:
            response = dialog.run()
            if response == 1:
                self._show_license(dialog)
                continue
            break
        dialog.destroy()

    def reset_summary(self) -> None:
        self.capacity_card.set_title("Capacity")
        self.free_card.set_title("Free space")
        self.files_card.set_title("Files")
        self.fragmented_card.set_title("Fragmentation")
        for card in (
            self.capacity_card,
            self.free_card,
            self.files_card,
            self.fragmented_card,
        ):
            card.set_value("—")
        self.map_caption.set_text(
            "Allocation grid · detail increases with the available drawing area."
        )

    def apply_map_presentation(self, presentation: MapPresentation) -> None:
        self.disk_map.set_cells(presentation.cells)
        self.capacity_card.set_title(presentation.capacity_title)
        self.capacity_card.set_value(presentation.capacity_value)
        self.free_card.set_title(presentation.free_title)
        self.free_card.set_value(presentation.free_value)
        self.files_card.set_title(presentation.files_title)
        self.files_card.set_value(presentation.files_value)
        self.fragmented_card.set_title(presentation.fragmentation_title)
        self.fragmented_card.set_value(presentation.fragmentation_value)
        self.disk_map.set_unit_label(presentation.unit_label)
        self.map_caption.set_text(presentation.caption)
        self.status_label.set_text(presentation.status)

    def set_control_state(self, state: ControlState) -> None:
        self.refresh_button.set_sensitive(state.refresh)
        self.image_button.set_sensitive(state.refresh)
        self.device_combo.set_sensitive(state.select_device)
        self.analyze_button.set_sensitive(state.analyse)
        self.unmount_button.set_sensitive(state.unmount)
        self.defrag_button.set_sensitive(state.defrag)
        self.growth_button.set_sensitive(state.growth_defrag)
        self.recover_button.set_sensitive(state.recover)
        self.stop_button.set_sensitive(state.stop)

    def set_operation_tooltips(self, tooltips: dict[str, str]) -> None:
        self.defrag_button.set_tooltip_text(tooltips["defrag"])
        self.growth_button.set_tooltip_text(tooltips["growth-defrag"])
        self.recover_button.set_tooltip_text(tooltips["recover"])
