#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression test for the GTK-neutral GUI operation coordinator."""

from __future__ import annotations

import sys
import json
import tempfile
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
GUI = ROOT / "gui"
if str(GUI) not in sys.path:
    sys.path.insert(0, str(GUI))

from ui.backend_catalog import BackendCatalog
from ui.command_models import CommandCompletion
from ui.live_controller import LiveEventController
from ui.operation_coordinator import OperationCoordinator
from ui.operation_presenter import OperationPresenter


class _DiskMap:
    def __init__(self) -> None:
        self.cells = []
        self.draws = 0

    def desired_cell_count(
        self,
        width: int | None = None,
        height: int | None = None,
    ) -> int:
        if width is not None and height is not None:
            return max(1, width) * max(1, height)
        return 369_720

    def set_cells(self, cells) -> None:
        self.cells = cells

    def queue_draw(self) -> None:
        self.draws += 1


class _Value:
    def __init__(self) -> None:
        self.text = ""
        self.fraction = 0.0

    def set_fraction(self, value: float) -> None:
        self.fraction = value

    def set_text(self, value: str) -> None:
        self.text = value

    def set_value(self, value: str) -> None:
        self.text = value

    def pulse(self) -> None:
        pass

    def get_text(self) -> str:
        return self.text


class _View:
    def __init__(self) -> None:
        self.disk_map = _DiskMap()
        self.progress = _Value()
        self.status_label = _Value()
        self.logs: list[str] = []
        self.errors: list[tuple[str, str]] = []
        self.fragmented_card = _Value()
        self.free_card = _Value()
        self.confirm_result = True
        self.presentations = []

    def append_log(self, text: str) -> None:
        self.logs.append(text)

    def clear_log(self) -> None:
        self.logs.clear()

    def show_error(self, title: str, message: str) -> None:
        self.errors.append((title, message))

    def confirm(self, _title: str, _message: str) -> bool:
        return self.confirm_result

    def apply_map_presentation(self, presentation) -> None:
        self.presentations.append(presentation)
        self.disk_map.cells = presentation.cells


class _Runner:
    def __init__(self) -> None:
        self.busy = False
        self.stop_requested = False
        self.requests = []

    def run(self, request) -> bool:
        if self.busy:
            return False
        self.requests.append(request)
        self.busy = True
        return True

    def complete(self, returncode: int, output: str) -> None:
        request = self.requests[-1]
        self.busy = False
        request.on_complete(CommandCompletion(returncode, output, request.purpose))

    def request_stop(self) -> bool:
        self.stop_requested = True
        return True


class _Volumes:
    def __init__(self, path: str = "/dev/test", *, image: bool = False) -> None:
        self.current = SimpleNamespace(
            path=path,
            normalized_fstype="ext4",
            display_fstype="ext4",
            mounted=False,
            image=image,
            readonly=False,
            operations={"defrag": {}, "growth-defrag": {}, "recover": {}},
        )
        self.invalidated: list[str] = []
        self.remembered: list[dict[str, object]] = []

    def journal_path(self, directory: Path) -> str:
        return str(directory / "test.journal")

    def remember_map(self, data) -> None:
        self.remembered.append(data)

    def invalidate(self, path: str) -> None:
        self.invalidated.append(path)


class _Presenter:
    def command_finished(self, *_args, **_kwargs) -> None:
        pass

    def apply_post_analysis_status(self) -> None:
        pass


def _catalog() -> BackendCatalog:
    return BackendCatalog.from_manifest(
        {
            "backends": [
                {
                    "id": "ext4",
                    "aliases": ["ext2", "ext3"],
                    "capabilities": 0,
                    "operations": [
                        {"name": "defrag", "label": "Defragment", "description": "Pack."},
                        {"name": "growth-defrag", "label": "Growth Defrag", "description": "Reserve."},
                        {"name": "recover", "label": "Recover", "description": "Recover."},
                    ],
                }
            ]
        }
    )


def _map_payload() -> str:
    return json.dumps(
        {
            "backend": "read-only-domain",
            "filesystem": "ext4",
            "unit_size": 4096,
            "total_units": 4,
            "total_bytes": 16384,
            "filesystem_bytes": 16384,
            "free_bytes": 8192,
            "used_bytes": 8192,
            "regular_files": 1,
            "directories": 1,
            "fragmented_files": 0,
            "fragmented_directories": 0,
            "cell_count": 1,
            "cells": [{"start": 0, "end": 3, "free": 2, "used": 2}],
        }
    )


def _composed(path: str, state: Path, *, image: bool = True):
    view = _View()
    runner = _Runner()
    volumes = _Volumes(path, image=image)
    placeholder = _Presenter()
    coordinator = OperationCoordinator(
        view=view,
        volumes=volumes,
        catalog=_catalog(),
        runner=runner,
        presenter=placeholder,
        mapper="/mapper",
        operation_engine="/engine",
        state_directory=lambda: state,
        minimum_cells=1_024,
        maximum_cells=500_000,
    )
    presenter = OperationPresenter(
        view=view,
        runner=runner,
        live_events=LiveEventController(),
        map_provider=lambda: coordinator.map_data,
        scheduler=lambda _delay, _callback: 1,
        cancel_scheduled=lambda _token: None,
        controls_changed=lambda: None,
    )
    coordinator.presenter = presenter
    return coordinator, view, runner, volumes


def test_gui_analysis_dispatches_mapper() -> None:
    view = _View()
    runner = _Runner()
    coordinator = OperationCoordinator(
        view=view,
        volumes=_Volumes(),
        catalog=SimpleNamespace(),
        runner=runner,
        presenter=_Presenter(),
        mapper="/mapper",
        operation_engine="/engine",
        state_directory=lambda: Path("/tmp"),
        minimum_cells=1_024,
        maximum_cells=500_000,
    )
    coordinator.analyze()
    assert len(runner.requests) == 1
    request = runner.requests[0]
    assert request.arguments == (
        "/mapper",
        "/dev/test",
        "--fstype",
        "ext4",
        "--cells",
        "369720",
    )
    assert request.purpose == "analysis"
    assert request.privileged
    assert view.logs == ["Analysing EXT4 volume /dev/test…"]
    assert coordinator.desired_map_cells(640, 320) == 204_800
    assert not coordinator.map_resolution_needs_refresh(204_800)

    runner.busy = False
    coordinator.analyze(target_cells=2_000_000)
    assert runner.requests[-1].arguments[-1] == "500000"


def test_complete_gui_operation_lifecycle() -> None:
    with tempfile.TemporaryDirectory(prefix="linux-defragger-gui-") as directory:
        state = Path(directory)
        image = state / "volume.ext4"
        image.write_bytes(b"test")
        coordinator, view, runner, volumes = _composed(str(image), state)

        coordinator.analyze(target_cells=250_000)
        assert not runner.requests[-1].privileged
        runner.complete(0, _map_payload())
        assert coordinator.map_data is not None
        assert len(view.presentations) == 1
        assert len(volumes.remembered) == 1
        # The backend may legitimately return fewer cells than requested when
        # the filesystem itself has fewer units. Remember the requested pixel
        # density so resizing does not loop forever trying the same resolution.
        assert coordinator.last_map_cell_target == 250_000
        assert not coordinator.map_resolution_needs_refresh(250_000)
        assert coordinator.map_resolution_needs_refresh(200_000)

        coordinator.analyze()
        runner.complete(0, "not-json")
        assert view.errors[-1][0] == "The analyser did not return a valid allocation map"

        request_count = len(runner.requests)
        view.confirm_result = False
        coordinator.start_mutation("defrag")
        assert len(runner.requests) == request_count

        view.confirm_result = True
        coordinator.start_mutation("defrag")
        mutation = runner.requests[-1]
        assert mutation.purpose == "defrag" and mutation.privileged
        assert volumes.invalidated == [str(image)]
        runner.complete(0, "")
        assert runner.requests[-1].purpose == "analysis"
        runner.complete(0, _map_payload())

        coordinator.start_mutation("growth-defrag")
        assert runner.requests[-1].privileged
        runner.complete(130, "")
        assert runner.requests[-1].purpose == "analysis"
        runner.complete(0, _map_payload())
        assert view.progress.text == "Stopped safely"
        assert "stopped safely" in view.status_label.text.lower()


def test_recovery_journal_controls_and_privilege_selection() -> None:
    with tempfile.TemporaryDirectory(prefix="linux-defragger-gui-recovery-") as directory:
        state = Path(directory)
        image = state / "volume.ext4"
        image.write_bytes(b"test")
        coordinator, view, runner, _volumes = _composed(str(image), state)

        coordinator.start_mutation("recover")
        assert not runner.requests
        assert view.errors[-1][0] == "No recovery journal"

        Path(coordinator.journal_path).touch()
        coordinator.start_mutation("defrag")
        assert not runner.requests
        assert view.errors[-1][0] == "Recovery is required"

        coordinator.start_mutation("recover")
        assert runner.requests[-1].purpose == "recover"
        assert runner.requests[-1].privileged
        assert "--journal" in runner.requests[-1].arguments

        physical, _view, physical_runner, _volumes = _composed(
            "/dev/test", state, image=False
        )
        physical.analyze()
        assert physical_runner.requests[-1].privileged


if __name__ == "__main__":
    test_gui_analysis_dispatches_mapper()
    test_complete_gui_operation_lifecycle()
    test_recovery_journal_controls_and_privilege_selection()
    print("complete GUI operation lifecycle tests passed")
