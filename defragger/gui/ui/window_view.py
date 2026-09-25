# SPDX-License-Identifier: GPL-3.0-or-later
"""GTK widget construction and presentation for one Defragmenter window."""

from __future__ import annotations

from typing import Any, Protocol

from gi.repository import Gdk, Gtk

from .map_presenter import MapPresentation
from .operation_planner import ControlState
from .theme import ThemeMode, apply_theme, load_theme_mode, save_theme_mode, theme_label
from .widgets import CheckerBall, DiskMap, GaugeCard, HeroArtwork, SummaryCard


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

    def launch_test_media(self) -> None: ...


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

    @staticmethod
    def _icon(name: str, size: int = 24) -> Gtk.Image:
        image = Gtk.Image.new_from_icon_name(name, Gtk.IconSize.DIALOG)
        image.set_pixel_size(size)
        return image

    def _nav_button(
        self,
        icon_name: str,
        title: str,
        subtitle: str,
        callback: Any,
        *,
        selected: bool = False,
        css_class: str = "",
    ) -> Gtk.Button:
        button = Gtk.Button()
        button.set_relief(Gtk.ReliefStyle.NONE)
        button.set_hexpand(True)
        button.get_style_context().add_class("nav-button")
        if css_class:
            button.get_style_context().add_class(css_class)
        if selected:
            button.get_style_context().add_class("nav-selected")
        if not hasattr(self, "_nav_buttons"):
            self._nav_buttons = []
        self._nav_buttons.append(button)
        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        row.set_border_width(6)
        row.pack_start(self._icon(icon_name, 28), False, False, 0)
        labels = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=1)
        primary = Gtk.Label(label=title)
        primary.set_xalign(0)
        primary.get_style_context().add_class("nav-title")
        secondary = Gtk.Label(label=subtitle)
        secondary.set_xalign(0)
        secondary.get_style_context().add_class("nav-subtitle")
        labels.pack_start(primary, False, False, 0)
        labels.pack_start(secondary, False, False, 0)
        row.pack_start(labels, True, True, 0)
        button.add(row)

        def activate(widget: Gtk.Button) -> None:
            for candidate in getattr(self, "_nav_buttons", []):
                candidate.get_style_context().remove_class("nav-selected")
            widget.get_style_context().add_class("nav-selected")
            callback(widget)

        button.connect("clicked", activate)
        return button

    def _action_button(
        self,
        icon_name: str,
        title: str,
        subtitle: str,
        css_class: str,
        callback: Any,
    ) -> Gtk.Button:
        button = Gtk.Button()
        button.set_hexpand(True)
        button.set_vexpand(True)
        button.get_style_context().add_class("action-card")
        button.get_style_context().add_class(css_class)
        content = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        content.set_border_width(8)
        content.pack_start(self._icon(icon_name, 32), False, False, 0)
        labels = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        title_label = Gtk.Label(label=title)
        title_label.set_xalign(0)
        title_label.get_style_context().add_class("action-card-title")
        subtitle_label = Gtk.Label(label=subtitle)
        subtitle_label.set_xalign(0)
        subtitle_label.set_line_wrap(True)
        subtitle_label.get_style_context().add_class("action-card-subtitle")
        labels.pack_start(title_label, False, False, 0)
        labels.pack_start(subtitle_label, False, False, 0)
        content.pack_start(labels, True, True, 0)
        arrow = Gtk.Label(label="›")
        arrow.get_style_context().add_class("action-arrow")
        content.pack_end(arrow, False, False, 0)
        button.add(content)
        button.connect("clicked", callback)
        return button

    def _build_sidebar(self) -> Gtk.Widget:
        sidebar = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        sidebar.set_size_request(238, -1)
        sidebar.set_border_width(12)
        sidebar.get_style_context().add_class("sidebar")

        brand = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        brand.pack_start(self._icon(APP_ICON_NAME, 52), False, False, 0)
        brand_labels = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        brand_title = Gtk.Label(label="Defragmenter")
        brand_title.set_xalign(0)
        brand_title.get_style_context().add_class("sidebar-brand")
        brand_subtitle = Gtk.Label(label="Visual disk optimisation")
        brand_subtitle.set_xalign(0)
        brand_subtitle.get_style_context().add_class("sidebar-brand-subtitle")
        brand_labels.pack_start(brand_title, False, False, 0)
        brand_labels.pack_start(brand_subtitle, False, False, 0)
        brand.pack_start(brand_labels, True, True, 0)
        sidebar.pack_start(brand, False, False, 6)

        overview = self._nav_button(
            "go-home-symbolic",
            "Overview",
            "Drive at a glance",
            lambda _button: self._show_overview(),
            selected=True,
            css_class="nav-overview",
        )
        sidebar.pack_start(overview, False, False, 0)

        self.sidebar_analyze_button = self._nav_button(
            "system-search-symbolic",
            "Analyse",
            "Scan and visualise",
            lambda _button: self.controller.analyze(),
            css_class="nav-analyse",
        )
        self.sidebar_defrag_button = self._nav_button(
            "view-grid-symbolic",
            "Defragment",
            "Optimise file layout",
            lambda _button: self.controller.start_mutation("defrag"),
            css_class="nav-defrag",
        )
        self.sidebar_growth_button = self._nav_button(
            "go-up-symbolic",
            "Growth Defrag",
            "Keep free space contiguous",
            lambda _button: self.controller.start_mutation("growth-defrag"),
            css_class="nav-growth",
        )
        self.sidebar_recover_button = self._nav_button(
            "edit-undo-symbolic",
            "Recover",
            "Resume safe recovery",
            lambda _button: self.controller.start_mutation("recover"),
            css_class="nav-recover",
        )
        for button in (
            self.sidebar_analyze_button,
            self.sidebar_defrag_button,
            self.sidebar_growth_button,
            self.sidebar_recover_button,
        ):
            sidebar.pack_start(button, False, False, 0)

        sidebar.pack_start(Gtk.Separator(), False, False, 6)
        self.sidebar_test_media_button = self._nav_button(
            "drive-removable-media-symbolic",
            "Test Media",
            "Check and diagnose",
            lambda _button: self.controller.launch_test_media(),
            css_class="nav-test-media",
        )
        self.sidebar_settings_button = self._nav_button(
            "preferences-system-symbolic",
            "Settings",
            "Appearance and preferences",
            lambda _button: self._show_settings_dialog(),
            css_class="nav-settings",
        )
        sidebar.pack_start(self.sidebar_test_media_button, False, False, 0)
        sidebar.pack_start(self.sidebar_settings_button, False, False, 0)

        footer_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        checker = CheckerBall()
        checker.set_halign(Gtk.Align.CENTER)
        footer_box.pack_start(checker, False, False, 0)
        footer = Gtk.Label(label="Fast disks\nHappy computing.")
        footer.set_xalign(0)
        footer.get_style_context().add_class("sidebar-footer")
        footer_box.pack_start(footer, False, False, 0)
        sidebar.pack_end(footer_box, False, False, 4)
        return sidebar

    def _build(self) -> None:
        outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        outer.get_style_context().add_class("app-shell")
        self.window.add(outer)
        outer.pack_start(self._build_menu_bar(), False, False, 0)

        body = Gtk.Paned(orientation=Gtk.Orientation.HORIZONTAL)
        body.set_wide_handle(False)
        body.set_position(238)
        body.set_hexpand(True)
        body.set_vexpand(True)
        body.get_style_context().add_class("app-body")
        outer.pack_start(body, True, True, 0)

        # Build the content before the sidebar so the selected Overview button
        # can safely focus the already-created map widget.
        body_scroll = Gtk.ScrolledWindow()
        self.body_scroll = body_scroll
        body_scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        body_scroll.set_shadow_type(Gtk.ShadowType.NONE)
        body_scroll.get_style_context().add_class("body-scroll")

        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=14)
        root.set_border_width(16)
        body_scroll.add(root)

        hero = Gtk.Frame()
        hero.set_shadow_type(Gtk.ShadowType.NONE)
        hero.get_style_context().add_class("hero-panel")

        hero_overlay = Gtk.Overlay()
        hero_overlay.add(HeroArtwork())
        hero_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        hero_box.set_border_width(16)

        hero_heading = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        drive_badge = Gtk.Frame()
        drive_badge.set_shadow_type(Gtk.ShadowType.NONE)
        drive_badge.get_style_context().add_class("hero-drive-badge")
        drive_badge.add(self._icon("drive-harddisk-symbolic", 48))
        hero_heading.pack_start(drive_badge, False, False, 0)

        hero_text = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        selected_label = Gtk.Label(label="SELECTED VOLUME")
        selected_label.set_xalign(0)
        selected_label.get_style_context().add_class("hero-kicker")
        self.volume_title = Gtk.Label(label="Choose a disk")
        self.volume_title.set_xalign(0)
        self.volume_title.set_ellipsize(3)
        self.volume_title.get_style_context().add_class("hero-volume-title")
        self.volume_detail = Gtk.Label(label="Select a filesystem to visualise")
        self.volume_detail.set_xalign(0)
        self.volume_detail.set_ellipsize(3)
        self.volume_detail.get_style_context().add_class("hero-hint")
        hero_text.pack_start(selected_label, False, False, 0)
        hero_text.pack_start(self.volume_title, False, False, 0)
        hero_text.pack_start(self.volume_detail, False, False, 0)
        hero_heading.pack_start(hero_text, True, True, 0)

        version_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=1)
        version_box.get_style_context().add_class("version-badge")
        version_title = Gtk.Label(label=f"Engine {self.engine_version}")
        version_title.set_xalign(1)
        version_title.get_style_context().add_class("version-primary")
        version_gui = Gtk.Label(label=f"GUI {self.gui_version}")
        version_gui.set_xalign(1)
        version_gui.get_style_context().add_class("version-secondary")
        version_box.pack_start(version_title, False, False, 0)
        version_box.pack_start(version_gui, False, False, 0)
        hero_heading.pack_end(version_box, False, False, 4)

        self.hero_status = Gtk.Label(label="Ready")
        self.hero_status.get_style_context().add_class("hero-status")
        hero_heading.pack_end(self.hero_status, False, False, 0)
        hero_box.pack_start(hero_heading, True, True, 0)

        device_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.device_combo = Gtk.ComboBoxText()
        self.device_combo.set_hexpand(True)
        self.device_combo.connect("changed", self.controller.on_device_changed)
        device_row.pack_start(self.device_combo, True, True, 0)
        self.refresh_button = Gtk.Button.new_with_label("Refresh")
        self.refresh_button.connect(
            "clicked",
            lambda _button: self.controller.refresh_devices(clear_cache=True),
        )
        device_row.pack_start(self.refresh_button, False, False, 0)
        self.image_button = Gtk.Button.new_with_label("Open image…")
        self.image_button.connect("clicked", self.controller.open_image)
        device_row.pack_start(self.image_button, False, False, 0)
        self.unmount_button = Gtk.Button.new_with_label("Unmount")
        self.unmount_button.connect("clicked", self.controller.unmount_selected)
        device_row.pack_start(self.unmount_button, False, False, 0)
        hero_box.pack_end(device_row, False, False, 0)

        hero_overlay.add_overlay(hero_box)
        hero.add(hero_overlay)
        root.pack_start(hero, False, False, 0)

        cards = Gtk.Grid(column_spacing=10, row_spacing=10)
        cards.set_column_homogeneous(True)
        self.fragmented_card = GaugeCard(
            "Fragmentation",
            DiskMap.COLORS["fragmented"],
            "summary-fragmented",
        )
        self.free_card = GaugeCard(
            "Free space",
            (0.08, 0.62, 1.0),
            "summary-free",
        )
        self.capacity_card = GaugeCard(
            "Allocated",
            (1.0, 0.66, 0.10),
            "summary-capacity",
        )
        self.files_card = SummaryCard("Files", "summary-files")
        cards.attach(self.fragmented_card, 0, 0, 1, 1)
        cards.attach(self.free_card, 1, 0, 1, 1)
        cards.attach(self.capacity_card, 2, 0, 1, 1)
        cards.attach(self.files_card, 3, 0, 1, 1)
        root.pack_start(cards, False, False, 0)

        map_frame = Gtk.Frame()
        map_frame.set_shadow_type(Gtk.ShadowType.NONE)
        map_frame.get_style_context().add_class("map-panel")
        map_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        map_box.set_border_width(12)

        map_header = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        map_title = self._section_label("Disk map")
        map_header.pack_start(map_title, False, False, 0)
        map_hint = Gtk.Label(label="physical-position pixel view")
        map_hint.set_xalign(0)
        map_hint.get_style_context().add_class("map-hint")
        map_header.pack_start(map_hint, False, False, 0)
        map_box.pack_start(map_header, False, False, 0)

        self.disk_map = DiskMap()
        self.disk_map.set_can_focus(True)
        self.disk_map.connect("size-allocate", self.controller.on_map_size_allocate)
        self.disk_map.get_style_context().add_class("pixel-map")
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
            ("Outside filesystem", DiskMap.COLORS["outside"]),
            ("Used", DiskMap.COLORS["used"]),
            ("Fragmented", DiskMap.COLORS["fragmented"]),
            ("Directory", DiskMap.COLORS["directory"]),
            ("Unknown", DiskMap.COLORS["unknown"]),
            ("Metadata / reserved", DiskMap.COLORS["bad"]),
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
            label="Physical pixel map · left-to-right, top-to-bottom follows real on-disk allocation order."
        )
        self.map_caption.set_xalign(0)
        self.map_caption.set_line_wrap(True)
        self.map_caption.get_style_context().add_class("map-caption")
        map_box.pack_start(self.map_caption, False, False, 0)
        map_frame.add(map_box)
        root.pack_start(map_frame, True, True, 0)

        actions = Gtk.Grid(column_spacing=10, row_spacing=10)
        actions.set_column_homogeneous(True)
        self.analyze_button = self._action_button(
            "system-search-symbolic",
            "Analyse",
            "Scan and visualise",
            "action-analyse",
            lambda _button: self.controller.analyze(),
        )
        self.defrag_button = self._action_button(
            "view-grid-symbolic",
            "Defragment",
            "Optimise file layout",
            "action-defrag",
            lambda _button: self.controller.start_mutation("defrag"),
        )
        self.growth_button = self._action_button(
            "go-up-symbolic",
            "Growth Defrag",
            "Keep free space contiguous",
            "action-growth",
            lambda _button: self.controller.start_mutation("growth-defrag"),
        )
        self.recover_button = self._action_button(
            "edit-undo-symbolic",
            "Recover",
            "Resume a safe recovery",
            "action-recover",
            lambda _button: self.controller.start_mutation("recover"),
        )
        actions.attach(self.analyze_button, 0, 0, 1, 1)
        actions.attach(self.defrag_button, 1, 0, 1, 1)
        actions.attach(self.growth_button, 2, 0, 1, 1)
        actions.attach(self.recover_button, 3, 0, 1, 1)
        root.pack_start(actions, False, False, 0)

        lower = Gtk.Grid(column_spacing=12, row_spacing=0)
        lower.set_column_homogeneous(True)

        preview = Gtk.Frame()
        preview.set_shadow_type(Gtk.ShadowType.NONE)
        preview.get_style_context().add_class("preview-panel")
        preview_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        preview_box.set_border_width(12)
        preview_title = self._section_label("Current allocation")
        preview_box.pack_start(preview_title, False, False, 0)
        self.preview_map = DiskMap()
        self.preview_map.set_size_request(-1, 116)
        self.preview_map.set_sensitive(False)
        preview_box.pack_start(self.preview_map, True, True, 0)
        preview_note = Gtk.Label(
            label="Compact live mirror of the authoritative disk map."
        )
        preview_note.set_xalign(0)
        preview_note.get_style_context().add_class("preview-note")
        preview_box.pack_start(preview_note, False, False, 0)
        preview.add(preview_box)
        lower.attach(preview, 0, 0, 1, 1)

        activity = Gtk.Frame()
        activity.set_shadow_type(Gtk.ShadowType.NONE)
        activity.get_style_context().add_class("activity-panel")
        activity_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        activity_box.set_border_width(12)
        activity_header = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        activity_header.pack_start(self._icon("media-playback-start-symbolic", 20), False, False, 0)
        activity_title = Gtk.Label(label="Activity")
        activity_title.set_xalign(0)
        activity_title.get_style_context().add_class("section-title")
        activity_header.pack_start(activity_title, True, True, 0)
        self.stop_button = Gtk.Button.new_with_label("Stop safely")
        self.stop_button.connect("clicked", self.controller.request_stop)
        self.stop_button.set_sensitive(False)
        self.stop_button.get_style_context().add_class("destructive-action")
        activity_header.pack_end(self.stop_button, False, False, 0)
        activity_box.pack_start(activity_header, False, False, 0)

        self.activity_primary = Gtk.Label(label="Ready for analysis")
        self.activity_primary.set_xalign(0)
        self.activity_primary.get_style_context().add_class("activity-primary")
        activity_box.pack_start(self.activity_primary, False, False, 0)
        self.activity_secondary = Gtk.Label(
            label="Choose a volume; analysis and safe operations appear here."
        )
        self.activity_secondary.set_xalign(0)
        self.activity_secondary.set_line_wrap(True)
        self.activity_secondary.get_style_context().add_class("activity-secondary")
        activity_box.pack_start(self.activity_secondary, False, False, 0)

        self.progress = Gtk.ProgressBar()
        self.progress.set_hexpand(True)
        self.progress.set_show_text(True)
        self.progress.set_text("Ready")
        self.progress.get_style_context().add_class("operation-progress")
        activity_box.pack_start(self.progress, False, False, 0)

        expander = Gtk.Expander(label="Technical activity log")
        expander.set_expanded(False)
        expander.get_style_context().add_class("log-expander")
        scroll = Gtk.ScrolledWindow()
        scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        scroll.set_min_content_height(100)
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
        activity_box.pack_start(expander, False, True, 0)
        activity.add(activity_box)
        lower.attach(activity, 1, 0, 1, 1)

        root.pack_start(lower, False, False, 0)

        status_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        status_box.set_border_width(8)
        status_box.get_style_context().add_class("status-strip")
        ready_dot = Gtk.Label(label="●")
        ready_dot.get_style_context().add_class("ready-dot")
        status_box.pack_start(ready_dot, False, False, 0)
        self.footer_volume = Gtk.Label(label="No volume selected")
        self.footer_volume.set_xalign(0)
        self.footer_volume.get_style_context().add_class("footer-volume")
        status_box.pack_start(self.footer_volume, False, False, 0)
        status_box.pack_start(
            Gtk.Separator(orientation=Gtk.Orientation.VERTICAL),
            False,
            False,
            8,
        )
        self.status_label = Gtk.Label(label="Ready")
        self.status_label.set_xalign(0)
        self.status_label.get_style_context().add_class("status-text")
        status_box.pack_start(self.status_label, True, True, 0)
        root.pack_start(status_box, False, False, 0)

        body.pack1(self._build_sidebar(), resize=False, shrink=False)
        body.pack2(body_scroll, resize=True, shrink=True)

    def _show_overview(self) -> None:
        adjustment = self.body_scroll.get_vadjustment()
        adjustment.set_value(adjustment.get_lower())
        self.disk_map.grab_focus()

    def _show_settings_dialog(self) -> None:
        dialog = Gtk.Dialog(
            title="Defragmenter Settings",
            transient_for=self.window,
            modal=True,
        )
        dialog.add_button("Close", Gtk.ResponseType.CLOSE)
        content = dialog.get_content_area()
        content.set_border_width(18)
        content.set_spacing(12)

        heading = Gtk.Label(label="Appearance")
        heading.set_xalign(0)
        heading.get_style_context().add_class("section-title")
        content.pack_start(heading, False, False, 0)

        note = Gtk.Label(
            label="Choose how Defragmenter follows the desktop appearance."
        )
        note.set_xalign(0)
        content.pack_start(note, False, False, 0)

        combo = Gtk.ComboBoxText()
        modes = (ThemeMode.SYSTEM, ThemeMode.DAY, ThemeMode.NIGHT)
        for mode in modes:
            combo.append_text(theme_label(mode))
        current = load_theme_mode()
        combo.set_active(modes.index(current))

        def apply_selection(widget: Gtk.ComboBoxText) -> None:
            index = widget.get_active()
            if index < 0:
                return
            mode = modes[index]
            save_theme_mode(mode)
            apply_theme(mode)
            self.sync_theme_menu(mode)

        combo.connect("changed", apply_selection)
        content.pack_start(combo, False, False, 0)
        dialog.show_all()
        dialog.run()
        dialog.destroy()

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
        .app-title { font-size: 24pt; font-weight: bold; }
        .app-subtitle { font-size: 10.5pt; }
        .sidebar-brand { font-size: 15pt; font-weight: bold; }
        .sidebar-brand-subtitle { font-size: 8.5pt; }
        .nav-title { font-size: 10pt; font-weight: bold; }
        .nav-subtitle { font-size: 8.25pt; }
        .sidebar-footer { font-size: 8.5pt; }
        .hero-kicker { font-size: 8.5pt; font-weight: bold; letter-spacing: 1px; }
        .hero-hint { font-size: 10pt; }
        .version-primary { font-size: 9.5pt; font-weight: bold; }
        .version-secondary { font-size: 9pt; }
        .section-title { font-size: 10.5pt; font-weight: bold; }
        .summary-title { font-size: 9.25pt; }
        .summary-value { font-size: 18pt; font-weight: bold; }
        .legend-item label, .map-caption, .map-hint { font-size: 8.75pt; }
        .action-card-title { font-size: 11pt; font-weight: bold; }
        .action-card-subtitle { font-size: 8.75pt; }
        .action-arrow { font-size: 22pt; font-weight: bold; }
        .hero-volume-title { font-size: 17pt; font-weight: bold; }
        .hero-status { font-size: 9pt; font-weight: bold; }
        .activity-primary { font-size: 10.5pt; font-weight: bold; }
        .activity-secondary, .preview-note { font-size: 8.75pt; }
        .footer-volume { font-size: 9pt; font-weight: bold; }
        .log-expander { font-weight: bold; }
        textview.log-view { font-size: 9.25pt; }
        .status-text { font-size: 9pt; }
        .ready-dot { font-size: 10pt; }
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
        if 0 <= active_index < len(names):
            selected = names[active_index]
            parts = [part.strip() for part in selected.split("—")]
            self.volume_title.set_text(parts[0] if parts else selected)
            detail = "  •  ".join(parts[1:]) if len(parts) > 1 else selected
            self.volume_detail.set_text(detail)
            self.footer_volume.set_text(parts[0] if parts else selected)
            lowered = selected.lower()
            mounted = "mounted" in lowered and "unmounted" not in lowered
            self.hero_status.set_text("● Mounted" if mounted else "● Offline")
        else:
            self.volume_title.set_text("Choose a disk")
            self.volume_detail.set_text("Select a filesystem to visualise")
            self.footer_volume.set_text("No volume selected")
            self.hero_status.set_text("Ready")

    def set_activity(self, primary: str, secondary: str) -> None:
        self.activity_primary.set_text(primary)
        self.activity_secondary.set_text(secondary)

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

    def show_about(self) -> None:
        """Display the product About surface through the selected presenter."""
        raise NotImplementedError(
            "WindowView requires a concrete About presentation implementation"
        )

    def reset_summary(self) -> None:
        self.preview_map.set_cells([])
        self.activity_primary.set_text("Ready for analysis")
        self.activity_secondary.set_text(
            "Choose a volume; analysis and safe operations appear here."
        )
        self.capacity_card.set_title("Allocated")
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
        for card in (
            self.capacity_card,
            self.free_card,
            self.fragmented_card,
        ):
            card.set_fraction(0.0)
            card.set_detail("")
        self.map_caption.set_text(
            "Physical pixel map · detail increases with the available drawing area without reordering disk positions."
        )

    def apply_map_presentation(self, presentation: MapPresentation) -> None:
        self.disk_map.set_cells(presentation.cells)
        self.preview_map.set_cells(presentation.cells)
        self.preview_map.set_unit_label(presentation.unit_label)
        total_units = sum(
            int(cell["end"]) - int(cell["start"]) + 1
            for cell in presentation.cells
        )
        free_units = sum(int(cell.get("free", 0)) for cell in presentation.cells)
        used_units = sum(int(cell.get("used", 0)) for cell in presentation.cells)
        fragmented_units = sum(
            int(cell.get("fragmented", 0)) for cell in presentation.cells
        )

        self.capacity_card.set_title("Allocated")
        self.capacity_card.set_value(presentation.capacity_value)
        self.capacity_card.set_fraction(
            used_units / max(1, free_units + used_units)
        )
        self.capacity_card.set_detail("filesystem usage")

        self.free_card.set_title(presentation.free_title)
        self.free_card.set_value(presentation.free_value)
        self.free_card.set_fraction(
            free_units / max(1, free_units + used_units)
        )
        self.free_card.set_detail("available allocation space")

        self.files_card.set_title(presentation.files_title)
        self.files_card.set_value(presentation.files_value)

        self.fragmented_card.set_title(presentation.fragmentation_title)
        self.fragmented_card.set_value(presentation.fragmentation_value)
        self.fragmented_card.set_fraction(
            fragmented_units / max(1, used_units)
        )
        self.fragmented_card.set_detail("fragmented allocation")

        self.disk_map.set_unit_label(presentation.unit_label)
        self.map_caption.set_text(presentation.caption)
        self.status_label.set_text(presentation.status)
        self.activity_primary.set_text("Scan completed")
        self.activity_secondary.set_text(presentation.status)

    def set_control_state(self, state: ControlState) -> None:
        self.refresh_button.set_sensitive(state.refresh)
        self.image_button.set_sensitive(state.refresh)
        self.device_combo.set_sensitive(state.select_device)
        idle = state.refresh
        self.analyze_button.set_sensitive(idle)
        self.unmount_button.set_sensitive(state.unmount)
        self.defrag_button.set_sensitive(idle)
        self.growth_button.set_sensitive(idle)
        self.recover_button.set_sensitive(idle)
        self.stop_button.set_sensitive(state.stop)
        self.sidebar_analyze_button.set_sensitive(idle)
        self.sidebar_defrag_button.set_sensitive(idle)
        self.sidebar_growth_button.set_sensitive(idle)
        self.sidebar_recover_button.set_sensitive(idle)

    def set_operation_tooltips(self, tooltips: dict[str, str]) -> None:
        self.defrag_button.set_tooltip_text(tooltips["defrag"])
        self.growth_button.set_tooltip_text(tooltips["growth-defrag"])
        self.recover_button.set_tooltip_text(tooltips["recover"])
        self.sidebar_defrag_button.set_tooltip_text(tooltips["defrag"])
        self.sidebar_growth_button.set_tooltip_text(tooltips["growth-defrag"])
        self.sidebar_recover_button.set_tooltip_text(tooltips["recover"])
