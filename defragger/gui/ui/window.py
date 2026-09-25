#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Defragmenter
# Author: Shannon Smith
# Purpose: Compose the GTK view, volume model, command runner and worker events.

"""Top-level GTK application composition.

``MainWindow`` coordinates typed services.  Widget construction, volume
collection policy, operation validation, subprocess ownership, privilege IPC,
and live-map interpretation are implemented by dedicated modules.
"""

from __future__ import annotations

import sys
from pathlib import Path

try:
    import gi

    gi.require_version("Gtk", "3.0")
    gi.require_version("Gdk", "3.0")
    from gi.repository import Gdk, Gio, GLib, Gtk
except (ImportError, ValueError) as exc:
    print(
        "Defragmenter requires GTK 3 Python bindings.\n"
        "Install them on Linux Mint with:\n"
        "  sudo apt install python3-gi python3-cairo gir1.2-gtk-3.0",
        file=sys.stderr,
    )
    raise SystemExit(1) from exc

from version import BUILD_LABEL, VERSION

from .about import SuiteStandardWindowView
from .command_runner import CommandRunner
from .devices import Volume
from .engine_client import (
    load_backend_catalog,
    query_engine_version,
)
from .icon_assets import apply_window_icon
from .live_controller import LiveEventController
from .operation_coordinator import OperationCoordinator
from .operation_planner import (
    control_state,
    operation_tooltips,
)
from .operation_presenter import OperationPresenter
from .support import (
    find_mapper,
    find_operation_engine,
    find_privileged_helper,
    state_dir,
)
from .volume_coordinator import VolumeCoordinator
from .widgets import MAX_MAP_CELLS, MIN_MAP_CELLS
from .window_view import APP_NAME


MAP_RESIZE_DEBOUNCE_MS = 180


class MainWindow(Gtk.ApplicationWindow):
    """Compose independent GUI, storage, runner, and protocol components."""

    def __init__(self, application: Gtk.Application) -> None:
        super().__init__(application=application, title=f"{APP_NAME} {VERSION}")
        # Behave like a normal desktop window.  The startup geometry is only a
        # preference; once realised it is capped to the actual monitor work area
        # so panels, RDP decorations and smaller desktops cannot leave controls
        # outside the visible screen.
        self.set_decorated(True)
        self.set_resizable(True)
        apply_window_icon(self)
        self.set_type_hint(Gdk.WindowTypeHint.NORMAL)
        self.set_skip_taskbar_hint(False)
        self.set_skip_pager_hint(False)
        self.set_default_size(1480, 900)
        self.set_position(Gtk.WindowPosition.CENTER)
        self.connect("realize", self._configure_native_window)
        self._map_resize_source: int | None = None
        self._map_resize_target = 0

        self.mapper = find_mapper()
        self.operation_engine = find_operation_engine()
        self.privileged_helper = find_privileged_helper()
        self.backend_catalog = load_backend_catalog(self.mapper)
        self.engine_version = query_engine_version(self.operation_engine)

        self.volumes = VolumeCoordinator(self.backend_catalog)

        self.view = SuiteStandardWindowView(
            self,
            self,
            gui_version=VERSION,
            engine_version=self.engine_version,
            build_label=BUILD_LABEL,
        )
        self.live_events = LiveEventController()
        self.runner = CommandRunner(
            mapper=self.mapper,
            operation_engine=self.operation_engine,
            privileged_helper=self.privileged_helper,
            on_event=lambda event: self.operations.on_runner_event(event),
            scheduler=GLib.idle_add,
        )
        self.operations = OperationPresenter(
            view=self.view,
            runner=self.runner,
            live_events=self.live_events,
            map_provider=lambda: self.coordinator.map_data,
            scheduler=GLib.timeout_add,
            cancel_scheduled=GLib.source_remove,
            controls_changed=self.update_controls,
        )
        self.coordinator = OperationCoordinator(
            view=self.view,
            volumes=self.volumes,
            catalog=self.backend_catalog,
            runner=self.runner,
            presenter=self.operations,
            mapper=self.mapper,
            operation_engine=self.operation_engine,
            state_directory=state_dir,
            minimum_cells=MIN_MAP_CELLS,
            maximum_cells=MAX_MAP_CELLS,
        )

        self.connect(
            "delete-event",
            lambda *_args: self.operations.defer_close_while_busy(),
        )
        self.connect("destroy", self._shutdown)
        self.refresh_devices()
        GLib.timeout_add(150, self._authenticate_on_launch)

    def _configure_native_window(self, _window: Gtk.Widget) -> None:
        """Publish normal WM controls and fit startup geometry to the work area."""

        native_window = self.get_window()
        if native_window is None:
            return

        # Do not rely on WMFunction.ALL semantics: explicitly advertise every
        # normal desktop operation so Cinnamon/Muffin and RDP window managers
        # expose resize/minimise/maximise/close consistently.
        functions = (
            Gdk.WMFunction.RESIZE
            | Gdk.WMFunction.MOVE
            | Gdk.WMFunction.MINIMIZE
            | Gdk.WMFunction.MAXIMIZE
            | Gdk.WMFunction.CLOSE
        )
        native_window.set_functions(functions)

        display = Gdk.Display.get_default()
        monitor = (
            display.get_monitor_at_window(native_window)
            if display is not None
            else None
        )
        if monitor is None:
            return

        workarea = monitor.get_workarea()
        # Client geometry needs headroom for server-side title bars/borders.
        # Never request more than the usable monitor rectangle minus this
        # decoration margin.
        width = min(1480, max(1, workarea.width - 48))
        height = min(900, max(1, workarea.height - 64))
        self.resize(width, height)

    @property
    def current_volume(self) -> Volume | None:
        return self.volumes.current

    @property
    def busy(self) -> bool:
        return self.runner.busy

    def append_log(self, text: str) -> None:
        self.view.append_log(text)

    def clear_log(self) -> None:
        self.view.clear_log()

    def show_error(self, title: str, message: str) -> None:
        self.view.show_error(title, message)

    def confirm(self, title: str, message: str) -> bool:
        return self.view.confirm(title, message)

    def _authenticate_on_launch(self) -> bool:
        self.runner.authenticate()
        return False

    def refresh_devices(
        self,
        preserve_path: str | None = None,
        clear_cache: bool = False,
    ) -> None:
        if self.busy:
            return
        try:
            active = self.volumes.refresh(
                preserve_path=preserve_path,
                clear_cache=clear_cache,
            )
        except Exception as exc:
            self.show_error("Unable to enumerate storage devices", str(exc))
            return
        self.view.populate_volumes(
            [volume.display_name for volume in self.volumes.volumes],
            active,
        )
        if not self.volumes.volumes:
            self.view.status_label.set_text(
                "No supported filesystems detected. Open an image or attach a "
                "supported volume."
            )
        self.update_controls()

    def on_device_changed(self, combo: Gtk.ComboBoxText) -> None:
        selection = self.volumes.select(combo.get_active())
        self.view.reset_summary()
        volume = selection.volume
        if volume is None:
            self.coordinator.reset_map()
            self.update_controls()
            return
        cached = selection.cached_map
        if cached is not None:
            self.coordinator.use_cached_map(cached)
            self.view.status_label.set_text(
                self.view.status_label.get_text() + " · cached analysis"
            )
        else:
            self.coordinator.reset_map()
            self.view.status_label.set_text(
                volume.display_name + " · analysing…"
            )
            GLib.idle_add(self._auto_analyse_selected, volume.path)
        self.update_controls()

    def _auto_analyse_selected(self, selected_path: str) -> bool:
        if (
            self.current_volume is not None
            and self.current_volume.path == selected_path
            and self.volumes.cached_map() is None
            and not self.busy
        ):
            self.analyze(clear_log=True)
        return False

    def open_image(self, _button: Gtk.Widget) -> None:
        filename = self.view.choose_image()
        if not filename:
            return
        try:
            volume = self.volumes.open_image(filename)
        except Exception as exc:
            self.show_error("Unable to open filesystem image", str(exc))
            return
        self.refresh_devices(volume.path)

    def unmount_selected(self, _button: Gtk.Button) -> None:
        volume = self.current_volume
        if not volume or volume.image or not volume.mounted:
            return
        self.clear_log()
        self.append_log(f"Unmounting {volume.path} through udisksctl…")
        self.coordinator.run_command(
            ["udisksctl", "unmount", "-b", volume.path],
            privileged=True,
            purpose="unmount",
            on_success=lambda _output: self.refresh_devices(volume.path),
        )

    def journal_path(self) -> str:
        return self.coordinator.journal_path

    def _cancel_map_resize_refresh(self) -> None:
        if self._map_resize_source is not None:
            GLib.source_remove(self._map_resize_source)
            self._map_resize_source = None

    def _schedule_map_resize_refresh(self) -> None:
        self._cancel_map_resize_refresh()
        self._map_resize_source = GLib.timeout_add(
            MAP_RESIZE_DEBOUNCE_MS,
            self._refresh_map_after_resize,
        )

    def _refresh_map_after_resize(self) -> bool:
        self._map_resize_source = None
        target = self._map_resize_target
        if target <= 0 or self.coordinator.map_data is None:
            self._map_resize_target = 0
            return False
        if not self.coordinator.map_resolution_needs_refresh(target):
            self._map_resize_target = 0
            return False
        if self.runner.busy:
            # Keep the newest pixel target pending. update_controls() schedules
            # it once the active analysis/mutation has actually completed.
            return False
        self._map_resize_target = 0
        self.analyze(
            clear_log=False,
            target_cells=target,
            quiet=True,
        )
        return False

    def on_map_size_allocate(
        self,
        _widget: Gtk.Widget,
        _allocation: Gdk.Rectangle,
    ) -> None:
        if self.coordinator.map_data is None:
            return

        # Drawing geometry already follows the current GTK allocation. The
        # native mapper must follow it too: one requested map cell per drawable
        # pixel (within the bounded GUI limit) keeps clusters-per-cell coupled
        # to the visible map resolution instead of freezing at the first size.
        self.view.disk_map.queue_draw()
        target = self.coordinator.desired_map_cells(
            _allocation.width,
            _allocation.height,
        )
        if not self.coordinator.map_resolution_needs_refresh(target):
            self._map_resize_target = 0
            self._cancel_map_resize_refresh()
            return
        self._map_resize_target = target
        self._schedule_map_resize_refresh()

    def analyze(
        self,
        clear_log: bool = True,
        target_cells: int | None = None,
        quiet: bool = False,
    ) -> None:
        self.coordinator.analyze(
            clear_log=clear_log,
            target_cells=target_cells,
            quiet=quiet,
        )

    def start_mutation(self, operation: str) -> None:
        volume = self.current_volume
        if volume is None:
            self.show_error(
                "Select a volume",
                "Choose a supported volume before starting an optimisation operation.",
            )
            return
        if self.busy:
            return
        if volume.mounted and not volume.image:
            operation_name = operation.replace("-", " ").title()
            if not self.confirm(
                "Unmount required",
                f"{operation_name} requires {volume.path} to be unmounted.\n\n"
                "Unmount it safely and continue?",
            ):
                return
            self.clear_log()
            self.append_log(
                f"Unmounting {volume.path} safely before {operation_name}…"
            )
            selected_path = volume.path

            def continue_after_unmount(_output: str) -> None:
                self.refresh_devices(selected_path)
                refreshed = self.current_volume
                if refreshed is None or refreshed.path != selected_path:
                    self.show_error(
                        "Volume changed",
                        "The selected volume changed while unmounting. "
                        "Choose it again before continuing.",
                    )
                    return
                if refreshed.mounted:
                    self.show_error(
                        "Unable to continue",
                        f"{selected_path} is still mounted.",
                    )
                    return
                GLib.idle_add(self._start_mutation_after_unmount, operation)

            self.coordinator.run_command(
                ["udisksctl", "unmount", "-b", volume.path],
                privileged=True,
                purpose="unmount-for-mutation",
                on_success=continue_after_unmount,
            )
            return
        self.coordinator.start_mutation(operation)

    def _start_mutation_after_unmount(self, operation: str) -> bool:
        self.coordinator.start_mutation(operation)
        return False

    def launch_test_media(self) -> None:
        executable = GLib.find_program_in_path("linux-defragger-test-media")
        if not executable:
            self.show_error(
                "Test Media is unavailable",
                "The Defragmenter Test Media companion is not installed.",
            )
            return
        try:
            Gio.Subprocess.new(
                [executable],
                Gio.SubprocessFlags.NONE,
            )
        except GLib.Error as exc:
            self.show_error("Unable to launch Test Media", str(exc))

    def request_stop(self, _button: Gtk.Button) -> None:
        self.operations.request_stop()

    def update_controls(self) -> None:
        volume = self.current_volume
        mutation_backend = bool(volume and volume.operations)
        journal_exists = bool(
            mutation_backend
            and volume
            and Path(self.coordinator.journal_path).exists()
        )
        state = control_state(
            volume,
            busy=self.busy,
            stop_requested=self.runner.stop_requested,
            journal_exists=journal_exists,
        )
        self.view.set_control_state(state)
        self.view.set_operation_tooltips(
            dict(operation_tooltips(volume, self.backend_catalog))
        )
        if (
            not self.runner.busy
            and self._map_resize_target > 0
            and self.coordinator.map_resolution_needs_refresh(
                self._map_resize_target
            )
            and self._map_resize_source is None
        ):
            self._schedule_map_resize_refresh()

    def _shutdown(self, *_args: object) -> None:
        self._cancel_map_resize_refresh()
        self.runner.shutdown()
