#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Functional tests for composed GUI services without constructing GTK."""

from __future__ import annotations

import sys
import tempfile
import threading
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUI = ROOT / "gui"
if str(GUI) not in sys.path:
    sys.path.insert(0, str(GUI))

from ui.backend_catalog import BackendCatalog
from ui.command_runner import (
    CommandCompletion,
    CommandRequest,
    CommandRunner,
    RunnerEvent,
)
from ui.devices import Volume
from ui.live_controller import LiveEventController
from ui.operation_presenter import OperationPresenter
from ui.operation_planner import (
    OperationValidationError,
    build_analysis_arguments,
    control_state,
    prepare_mutation,
)
from ui.privilege_session import PrivilegeSession
from ui.volume_coordinator import VolumeCoordinator
from ui.volume_store import VolumeStore


def _catalog() -> BackendCatalog:
    return BackendCatalog.from_manifest(
        {
            "backends": [
                {
                    "id": "ext4",
                    "aliases": ["ext2", "ext3"],
                    "capabilities": 0,
                    "operations": [
                        {
                            "name": "defrag",
                            "label": "Defragment",
                            "description": "Build a packed layout.",
                        },
                        {
                            "name": "growth-defrag",
                            "label": "Growth Defrag",
                            "description": "Build a growth layout.",
                        },
                        {
                            "name": "recover",
                            "label": "Recover",
                            "description": "Resume a transaction.",
                        },
                    ],
                }
            ]
        }
    )


def _volume(
    path: str,
    *,
    mounted: bool = False,
    image: bool = True,
) -> Volume:
    return Volume(
        catalog=_catalog(),
        path=path,
        name=Path(path).name,
        fstype="ext3",
        label="test",
        size=64 * 1024 * 1024,
        mountpoints=["/mnt/test"] if mounted else [],
        removable=False,
        readonly=False,
        model="test",
        transport="file" if image else "usb",
        image=image,
    )


def test_volume_store_owns_selection_images_and_cache() -> None:
    first = _volume("/dev/first", image=False)
    second = _volume("/dev/second", image=False)
    image = _volume("/tmp/image.ext4")
    store = VolumeStore()
    assert store.refresh([first, second]) == 0
    assert store.current is first
    store.add_image(image)
    store.remember_map({"filesystem": "ext4", "cells": []})
    assert store.current is image
    assert store.cached_map() is not None

    selected = store.refresh([second, first], preserve_path=image.path)
    assert selected == 2
    assert store.current is image
    store.invalidate(image.path)
    assert store.cached_map() is None


def test_volume_coordinator_owns_discovery_images_and_journals() -> None:
    catalog = _catalog()
    physical = _volume("/dev/test", image=False)

    with tempfile.TemporaryDirectory() as directory:
        image_path = Path(directory) / "disk.ext4"
        image_path.write_bytes(b"\0" * 4096)
        coordinator = VolumeCoordinator(
            catalog,
            discover=lambda _catalog: [physical],
            detect_image=lambda _path, _catalog: "ext4",
        )
        discovered = coordinator.discover()
        assert discovered == [physical]
        assert coordinator.apply_discovery(discovered) == 0
        assert coordinator.current is physical
        image = coordinator.open_image(str(image_path))
        assert image.image and image.fstype == "ext4"
        assert coordinator.current is image
        coordinator.remember_map({"filesystem": "ext4", "cells": []})
        assert coordinator.select(1).cached_map is not None
        assert coordinator.journal_path(Path(directory)).endswith(
            "disk.ext4.journal"
        )
        assert coordinator.refresh(preserve_path=image.path) == 1
        assert coordinator.current is image


def test_operation_planner_is_pure_and_complete() -> None:
    volume = _volume("/tmp/test.ext4")
    analysis = build_analysis_arguments(
        "/mapper",
        volume,
        999_999,
        minimum_cells=1_024,
        maximum_cells=500_000,
    )
    assert analysis[-1] == "500000"
    with tempfile.TemporaryDirectory() as directory:
        journal = str(Path(directory) / "test.journal")
        plan = prepare_mutation(
            "defrag",
            volume,
            volume.catalog,
            operation_engine="/engine",
            journal_path=journal,
            live_cells=4_096,
            minimum_cells=1_024,
            maximum_cells=500_000,
        )
        assert plan.arguments[:3] == (
            "/engine",
            "defrag",
            "/tmp/test.ext4",
        )
        assert plan.arguments[-2:] == ("--live-map-cells", "4096")
        assert plan.operation_name == "Defragment"

        Path(journal).touch()
        try:
            prepare_mutation(
                "defrag",
                volume,
                volume.catalog,
                operation_engine="/engine",
                journal_path=journal,
                live_cells=4_096,
                minimum_cells=1_024,
                maximum_cells=500_000,
            )
        except OperationValidationError as exc:
            assert exc.title == "Recovery is required"
        else:
            raise AssertionError("unfinished journal did not block Defragment")

    state = control_state(
        volume,
        busy=False,
        stop_requested=False,
        journal_exists=False,
    )
    assert state.analyse and state.defrag and state.growth_defrag
    assert not state.recover and not state.stop

    mounted = _volume("/dev/mounted", mounted=True, image=False)
    blocked = control_state(
        mounted,
        busy=False,
        stop_requested=False,
        journal_exists=False,
    )
    # Mounted physical media keeps the operation affordance clickable.
    # MainWindow owns the safe unmount-and-continue interaction; the native
    # planner still refuses any mounted write that bypasses that UI sequence.
    assert blocked.unmount and blocked.defrag and blocked.growth_defrag


def test_live_event_controller_returns_view_model_updates() -> None:
    controller = LiveEventController()
    result = controller.consume(
        '@@RESULT {"operation":"defrag","status":"completed","message":""}',
        None,
        purpose="defrag",
    )
    assert result.operation_result is not None
    assert result.operation_result.status == "completed"

    phase = controller.consume(
        '@@PHASE {"message":"Staging payloads"}',
        None,
        purpose="defrag",
    )
    assert phase.status == "Staging payloads"
    assert phase.log_messages == ("Staging payloads",)

    map_data = {
        "unit_size": 4096,
        "total_units": 8,
        "cells": [
            {
                "start": 0,
                "end": 3,
                "free": 4,
                "used": 0,
                "fragmented": 0,
                "directory": 0,
                "unknown": 0,
                "outside": 0,
                "bad": 0,
            },
            {
                "start": 4,
                "end": 7,
                "free": 4,
                "used": 0,
                "fragmented": 0,
                "directory": 0,
                "unknown": 0,
                "outside": 0,
                "bad": 0,
            },
        ],
    }
    moved = controller.consume(
        '@@LIVE_RANGE {"source_start_byte":0,'
        '"destination_start_byte":16384,"length_bytes":4096,"sequence":1}',
        map_data,
        purpose="defrag",
    )
    assert moved.map_changed and moved.draw_immediately
    assert map_data["cells"][1]["used"] == 1


def test_command_runner_owns_process_and_emits_typed_events() -> None:
    events = []
    completions = []
    finished = threading.Event()

    def schedule(callback, *args):
        return callback(*args)

    def completed(completion) -> None:
        completions.append(completion)
        finished.set()

    runner = CommandRunner(
        mapper="/mapper",
        operation_engine="/engine",
        privileged_helper="/helper",
        on_event=events.append,
        scheduler=schedule,
    )
    request = CommandRequest(
        arguments=(
            sys.executable,
            "-c",
            "print('ordinary output'); "
            "print('@@RESULT {\"operation\":\"defrag\","
            "\"status\":\"completed\",\"message\":\"\"}')",
        ),
        purpose="defrag",
        privileged=False,
        stream_output=True,
        on_complete=completed,
    )
    assert runner.run(request)
    assert finished.wait(5)
    assert not runner.busy
    assert completions[0].returncode == 0
    assert completions[0].output == "ordinary output\n"
    assert sum(event.kind == "started" for event in events) == 1
    assert any(event.kind == "output" for event in events)
    assert any(event.kind == "engine" for event in events)


def test_privilege_session_owns_allowlist_and_protocol_state() -> None:
    events = []
    completions = []
    session = PrivilegeSession(
        mapper="/mapper",
        operation_engine="/engine",
        privileged_helper="/helper",
        on_event=events.append,
        on_complete=completions.append,
        scheduler=lambda callback, *args: callback(*args),
    )
    assert session._program_and_args(("/mapper", "--map", "/dev/test")) == (
        "mapper",
        ("--map", "/dev/test"),
    )
    assert session._program_and_args(("udisksctl", "unmount")) == (
        "udisksctl",
        ("unmount",),
    )
    try:
        session._program_and_args(("/bin/sh", "-c", "true"))
    except RuntimeError:
        pass
    else:
        raise AssertionError("privileged allowlist accepted /bin/sh")

    request = CommandRequest(
        arguments=("/engine", "defrag", "/dev/test"),
        purpose="defrag",
        privileged=True,
        stream_output=True,
        on_complete=lambda _completion: None,
    )
    session._active_request = request
    session._active_id = 7
    session._handle_message({"type": "started", "id": 7})
    session._handle_message({"type": "progress", "id": 7, "percent": 25})
    session._handle_message({"type": "output", "id": 7, "line": "working"})
    session._handle_message(
        {
            "type": "output",
            "id": 7,
            "line": (
                '@@RESULT {"operation":"defrag","status":"completed",'
                '"message":""}'
            ),
        }
    )
    session._handle_message({"type": "finished", "id": 7, "returncode": 0})
    assert completions[0].output == "working\n"
    assert not any(event.kind == "started" for event in events)
    assert any(event.kind == "progress" and event.percent == 25 for event in events)
    assert any(event.kind == "engine" for event in events)


def test_privilege_session_preserves_helper_fifo_across_gui_scheduler() -> None:
    scheduled = []
    completions = []

    def defer(callback, *args):
        scheduled.append((callback, args))
        return len(scheduled)

    session = PrivilegeSession(
        mapper="/mapper",
        operation_engine="/engine",
        privileged_helper="/helper",
        on_event=lambda _event: None,
        on_complete=completions.append,
        scheduler=defer,
    )
    request = CommandRequest(
        arguments=("/mapper", "/dev/nvme0n1p2", "--fstype", "ntfs"),
        purpose="analysis",
        privileged=True,
        stream_output=False,
        on_complete=lambda _completion: None,
    )
    session._active_request = request
    session._active_id = 19

    payload = (
        '{"filesystem":"ntfs","unit_size":4096,"total_units":8,'
        '"cell_count":1,"cells":[{"start":0,"end":7}]}'
    )
    session._queue_message(
        {"type": "output", "id": 19, "line": payload}
    )
    session._queue_message(
        {"type": "finished", "id": 19, "returncode": 0}
    )

    # The transport must schedule one FIFO drain, not independent callbacks
    # that the GTK main loop could observe in completion-before-output order.
    assert len(scheduled) == 1
    callback, args = scheduled.pop()
    callback(*args)

    assert len(completions) == 1
    assert completions[0].returncode == 0
    assert completions[0].purpose == "analysis"
    assert completions[0].output == payload + "\n"



class _FakeValue:
    def __init__(self) -> None:
        self.text = ""

    def set_text(self, text: str) -> None:
        self.text = text

    def set_value(self, value: str) -> None:
        self.text = value


class _FakeProgress(_FakeValue):
    def __init__(self) -> None:
        super().__init__()
        self.fraction = 0.0
        self.pulses = 0

    def set_fraction(self, fraction: float) -> None:
        self.fraction = fraction

    def pulse(self) -> None:
        self.pulses += 1


class _FakeMap:
    def __init__(self) -> None:
        self.cells = []
        self.draws = 0

    def queue_draw(self) -> None:
        self.draws += 1


class _FakeOperationView:
    def __init__(self) -> None:
        self.progress = _FakeProgress()
        self.status_label = _FakeValue()
        self.disk_map = _FakeMap()
        self.fragmented_card = _FakeValue()
        self.free_card = _FakeValue()
        self.logs = []
        self.errors = []
        self.activity = ("", "")

    def set_activity(self, primary: str, secondary: str) -> None:
        self.activity = (primary, secondary)

    def append_log(self, text: str) -> None:
        self.logs.append(text)

    def show_error(self, title: str, message: str) -> None:
        self.errors.append((title, message))


class _FakeRunner:
    def __init__(self) -> None:
        self.busy = True
        self.stop_requested = False
        self.stop_calls = 0

    def request_stop(self) -> bool:
        self.stop_calls += 1
        self.stop_requested = True
        return True


def test_operation_presenter_owns_transient_window_lifecycle() -> None:
    view = _FakeOperationView()
    runner = _FakeRunner()
    scheduled = {}
    cancelled = []
    control_changes = []
    map_data = {
        "unit_size": 4096,
        "total_units": 4,
        "cells": [
            {
                "start": 0,
                "end": 3,
                "free": 4,
                "used": 0,
                "fragmented": 0,
                "directory": 0,
                "unknown": 0,
                "outside": 0,
                "bad": 0,
            }
        ],
    }

    def schedule(delay, callback):
        token = len(scheduled) + 1
        scheduled[token] = (delay, callback)
        return token

    aest = timezone(timedelta(hours=10), "AEST")
    wall_times = iter(
        (
            datetime(2026, 9, 12, 21, 30, 0, tzinfo=aest),
            datetime(2026, 9, 12, 21, 30, 2, tzinfo=aest),
        )
    )
    monotonic_times = iter((100.0, 102.347))

    presenter = OperationPresenter(
        view=view,
        runner=runner,
        live_events=LiveEventController(),
        map_provider=lambda: map_data,
        scheduler=schedule,
        cancel_scheduled=cancelled.append,
        controls_changed=lambda: control_changes.append(True),
        wall_clock=lambda: next(wall_times),
        monotonic_clock=lambda: next(monotonic_times),
    )
    presenter.on_runner_event(RunnerEvent("started", "growth-defrag"))
    presenter.on_runner_event(RunnerEvent("started", "growth-defrag"))
    assert view.progress.text == "Growth Defrag in progress…"
    assert scheduled[1][0] == 120

    presenter.on_runner_event(
        RunnerEvent("progress", "growth-defrag", percent=25.0)
    )
    assert view.progress.fraction == 0.25
    assert "25.00%" in view.status_label.text

    presenter.on_runner_event(
        RunnerEvent(
            "engine",
            "growth-defrag",
            '@@RESULT {"operation":"growth-defrag",'
            '"status":"not-needed","message":""}',
        )
    )
    completed = []
    presenter.command_finished(
        CommandCompletion(0, "", "growth-defrag"),
        on_success=completed.append,
        raw_completion=None,
    )
    assert completed == [""]
    assert view.logs.count(
        "Growth Defrag request started: 2026-09-12 21:30:00 AEST"
    ) == 1
    assert "Growth Defrag request finished: 2026-09-12 21:30:02 AEST" in view.logs
    assert "Growth Defrag end-to-end elapsed: 00:00:02.347" in view.logs
    assert view.progress.text == "Not needed"
    assert presenter.post_analysis_status is not None
    presenter.apply_post_analysis_status()
    assert "existing 10% growth-space layout" in view.status_label.text
    assert presenter.post_analysis_status is None
    assert cancelled == [1]

    noisy_failure = "\n".join(
        [f"XFS placement: {index} of 57344 blocks." for index in range(80)]
        + [
            "linux-defragger-xfs-worker: XFS AG 0 needs 40 allocation-tree "
            "blocks but has only 13 existing tree/AGFL reserve blocks",
            "XFS placement: 57344 of 57344 blocks.",
        ]
    )
    presenter.command_finished(
        CommandCompletion(1, noisy_failure, "defrag"),
        on_success=None,
        raw_completion=None,
    )
    assert view.errors[-1] == (
        "Defragment failed",
        "linux-defragger-xfs-worker: XFS AG 0 needs 40 allocation-tree "
        "blocks but has only 13 existing tree/AGFL reserve blocks",
    )
    assert len(view.errors[-1][1]) < 420

    assert presenter.defer_close_while_busy()
    assert runner.stop_calls == 1
    assert "Close postponed" in view.status_label.text
    assert len(control_changes) >= 3


def main() -> None:
    test_volume_store_owns_selection_images_and_cache()
    test_volume_coordinator_owns_discovery_images_and_journals()
    test_operation_planner_is_pure_and_complete()
    test_live_event_controller_returns_view_model_updates()
    test_command_runner_owns_process_and_emits_typed_events()
    test_privilege_session_owns_allowlist_and_protocol_state()
    test_privilege_session_preserves_helper_fifo_across_gui_scheduler()
    test_operation_presenter_owns_transient_window_lifecycle()
    print("composed GUI service tests passed")


if __name__ == "__main__":
    main()
