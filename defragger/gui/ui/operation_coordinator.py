# SPDX-License-Identifier: GPL-3.0-or-later
"""GTK-neutral analysis and mutation lifecycle coordination.

The coordinator owns the complete path from a selected volume to a validated
command and back to a refreshed allocation map.  ``MainWindow`` supplies GTK
callbacks but no longer contains operation policy, JSON parsing, permission
selection or command-continuation logic.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, Callable, Protocol

from .backend_catalog import BackendCatalog
from .command_models import CommandCompletion, CommandRequest
from .command_runner import CommandRunner
from .map_presenter import (
    AllocationMapError,
    MapPresentation,
    present_allocation_map,
)
from .operation_planner import (
    OperationValidationError,
    build_analysis_arguments,
    prepare_mutation,
)
from .operation_presenter import OperationPresenter
from .volume_coordinator import VolumeCoordinator


class _DiskMap(Protocol):
    def desired_cell_count(self) -> int: ...

    def set_cells(self, cells: list[dict[str, Any]]) -> None: ...


class _Progress(Protocol):
    def set_fraction(self, fraction: float) -> None: ...

    def set_text(self, text: str) -> None: ...


class _TextValue(Protocol):
    def get_text(self) -> str: ...

    def set_text(self, text: str) -> None: ...


class OperationCoordinatorView(Protocol):
    disk_map: _DiskMap
    progress: _Progress
    status_label: _TextValue

    def append_log(self, text: str) -> None: ...

    def clear_log(self) -> None: ...

    def show_error(self, title: str, message: str) -> None: ...

    def confirm(self, title: str, message: str) -> bool: ...

    def apply_map_presentation(self, presentation: MapPresentation) -> None: ...


StateDirectory = Callable[[], Path]


class OperationCoordinator:
    """Own analysis, mutation and post-operation refresh for one window."""

    def __init__(
        self,
        *,
        view: OperationCoordinatorView,
        volumes: VolumeCoordinator,
        catalog: BackendCatalog,
        runner: CommandRunner,
        presenter: OperationPresenter,
        mapper: str,
        operation_engine: str,
        state_directory: StateDirectory,
        minimum_cells: int,
        maximum_cells: int,
    ) -> None:
        self.view = view
        self.volumes = volumes
        self.catalog = catalog
        self.runner = runner
        self.presenter = presenter
        self.mapper = mapper
        self.operation_engine = operation_engine
        self.state_directory = state_directory
        self.minimum_cells = minimum_cells
        self.maximum_cells = maximum_cells
        self.map_data: dict[str, Any] | None = None
        self.last_map_cell_target = 0

    @property
    def journal_path(self) -> str:
        return self.volumes.journal_path(self.state_directory())

    def reset_map(self) -> None:
        self.map_data = None
        self.last_map_cell_target = 0
        self.view.disk_map.set_cells([])

    def apply_map(self, data: dict[str, Any]) -> MapPresentation:
        volume = self.volumes.current
        operations = volume.operations if volume else ()
        presentation = present_allocation_map(data, operations)
        data["cells"] = presentation.cells
        self.map_data = data
        self.volumes.remember_map(data)
        self.last_map_cell_target = presentation.cell_count
        self.view.apply_map_presentation(presentation)
        return presentation

    def use_cached_map(self, data: dict[str, Any]) -> MapPresentation:
        return self.apply_map(data)

    def desired_map_cells(self) -> int:
        return self.view.disk_map.desired_cell_count()

    def analyze(
        self,
        *,
        clear_log: bool = True,
        target_cells: int | None = None,
        quiet: bool = False,
    ) -> None:
        volume = self.volumes.current
        if not volume or self.runner.busy:
            return
        if not quiet:
            if clear_log:
                self.view.clear_log()
            else:
                self.view.append_log("\nRefreshing the allocation map…")
            self.view.append_log(
                f"Analysing {volume.display_fstype.upper()} volume "
                f"{volume.path}…"
            )
            if volume.mounted:
                self.view.append_log(
                    "The volume is mounted. Analysis is read-only; this is a live "
                    "snapshot and the map may change while the filesystem is active."
                )
        try:
            arguments = build_analysis_arguments(
                self.mapper,
                volume,
                (
                    target_cells
                    if target_cells is not None
                    else self.desired_map_cells()
                ),
                minimum_cells=self.minimum_cells,
                maximum_cells=self.maximum_cells,
            )
        except Exception as exc:
            self.view.progress.set_fraction(0.0)
            self.view.progress.set_text("Failed")
            self.view.status_label.set_text("Analysis could not start")
            self.view.show_error("Unable to start analysis", str(exc))
            return

        def parsed(output: str) -> None:
            try:
                payload = json.loads(output)
                if not isinstance(payload, dict):
                    raise AllocationMapError("allocation map root is not an object")
                presentation = self.apply_map(payload)
            except (json.JSONDecodeError, AllocationMapError) as exc:
                self.view.show_error(
                    "The analyser did not return a valid allocation map",
                    f"{exc}\n\n{output[-2000:]}",
                )
                return
            if volume.mounted:
                self.view.status_label.set_text(
                    self.view.status_label.get_text()
                    + " · live mounted snapshot"
                )
            if not quiet:
                self.view.append_log(presentation.analysis_log)
            self.presenter.apply_post_analysis_status()

        self._run_engine(arguments, "analysis", parsed)

    def start_mutation(self, operation: str) -> None:
        volume = self.volumes.current
        if not volume or self.runner.busy:
            return
        live_cells = (
            len(self.map_data.get("cells", []))
            if self.map_data
            else self.desired_map_cells()
        )
        try:
            plan = prepare_mutation(
                operation,
                volume,
                self.catalog,
                operation_engine=self.operation_engine,
                journal_path=self.journal_path,
                live_cells=live_cells,
                minimum_cells=self.minimum_cells,
                maximum_cells=self.maximum_cells,
            )
        except OperationValidationError as exc:
            self.view.show_error(exc.title, exc.message)
            return
        if not self.view.confirm(
            plan.confirmation_title,
            plan.confirmation_message,
        ):
            return

        self.volumes.invalidate(volume.path)
        self.view.clear_log()
        self.view.append_log(
            f"Starting {plan.operation_name} on {volume.path}…"
        )
        self.run_command(
            list(plan.arguments),
            # Every mutation uses the protected, root-owned journal namespace,
            # including writes and recovery for user-owned image files.
            privileged=True,
            purpose=operation,
            on_success=lambda _output: self.analyze(clear_log=False),
        )

    def _run_engine(
        self,
        arguments: tuple[str, ...] | list[str],
        purpose: str,
        on_success: Callable[[str], None],
    ) -> None:
        volume = self.volumes.current
        if not volume:
            return
        self.run_command(
            list(arguments),
            privileged=self._requires_privilege(volume.path, volume.image),
            purpose=purpose,
            on_success=on_success,
            stream_output=False,
        )

    @staticmethod
    def _requires_privilege(path: str, image: bool) -> bool:
        return not image or not os.access(path, os.R_OK | os.W_OK)

    def run_command(
        self,
        arguments: list[str],
        *,
        privileged: bool,
        purpose: str,
        on_success: Callable[[str], None] | None = None,
        raw_completion: Callable[[int, str], None] | None = None,
        stream_output: bool = True,
    ) -> None:
        request = CommandRequest(
            arguments=tuple(arguments),
            purpose=purpose,
            privileged=privileged,
            stream_output=stream_output,
            on_complete=lambda completion: self._finished(
                completion,
                on_success=on_success,
                raw_completion=raw_completion,
            ),
        )
        self.runner.run(request)

    def _finished(
        self,
        completion: CommandCompletion,
        *,
        on_success: Callable[[str], None] | None,
        raw_completion: Callable[[int, str], None] | None,
    ) -> None:
        self.presenter.command_finished(
            completion,
            on_success=on_success,
            raw_completion=raw_completion,
        )


__all__ = ["OperationCoordinator", "OperationCoordinatorView"]
