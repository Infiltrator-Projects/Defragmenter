# SPDX-License-Identifier: GPL-3.0-or-later
"""Present command lifecycle and live engine events without owning GTK widgets.

``OperationPresenter`` is the boundary between the presentation-neutral
``CommandRunner`` and a window view.  It owns transient operation state,
progress timing, safe-stop completion, live-map redraw coalescing and typed
worker results.  The window remains the composition root and supplies only
small callbacks for control-state refresh and post-operation analysis.
"""

from __future__ import annotations

from datetime import datetime
import time
from typing import Any, Callable, Protocol

from core.protocol import OperationResult

from .command_models import CommandCompletion, RunnerEvent
from .command_runner import CommandRunner
from .live_controller import LiveEventController
from .operation_status import operation_display_name, successful_completion


class _TextValue(Protocol):
    def set_text(self, text: str) -> None: ...


class _Progress(_TextValue, Protocol):
    def set_fraction(self, fraction: float) -> None: ...

    def pulse(self) -> None: ...


class _DiskMap(Protocol):
    cells: list[dict[str, Any]]

    def queue_draw(self) -> None: ...


class _SummaryValue(Protocol):
    def set_value(self, value: str) -> None: ...


class OperationView(Protocol):
    """The narrow view interface required during an operation."""

    progress: _Progress
    status_label: _TextValue
    disk_map: _DiskMap
    fragmented_card: _SummaryValue
    free_card: _SummaryValue

    def append_log(self, text: str) -> None: ...

    def show_error(self, title: str, message: str) -> None: ...

    def set_activity(self, primary: str, secondary: str) -> None: ...


Schedule = Callable[..., Any]
CancelScheduled = Callable[[Any], None]
MapProvider = Callable[[], dict[str, Any] | None]
VoidCallback = Callable[[], None]
AnalysisCallback = Callable[[], None]
SuccessCallback = Callable[[str], None]
RawCompletionCallback = Callable[[int, str], None]
WallClock = Callable[[], datetime]
MonotonicClock = Callable[[], float]

_TIMED_MUTATIONS = {"defrag", "growth-defrag", "recover"}
_FAILURE_MARKERS = (
    "failed",
    "error",
    "cannot ",
    "can't ",
    "refus",
    "too small",
    "device or resource busy",
    " needs ",
)


def _format_timestamp(value: datetime) -> str:
    localized = value if value.tzinfo is not None else value.astimezone()
    return localized.strftime("%Y-%m-%d %H:%M:%S %Z")


def _format_elapsed(seconds: float) -> str:
    safe_seconds = max(0.0, seconds)
    hours, remainder = divmod(safe_seconds, 3600.0)
    minutes, remaining_seconds = divmod(remainder, 60.0)
    return f"{int(hours):02d}:{int(minutes):02d}:{remaining_seconds:06.3f}"


def _failure_summary(output: str, returncode: int) -> str:
    """Return one bounded diagnostic; the complete transcript stays in the log."""

    lines = [line.strip() for line in output.splitlines() if line.strip()]
    candidates = [
        line
        for line in lines
        if not line.startswith("@@")
        and any(marker in line.lower() for marker in _FAILURE_MARKERS)
    ]
    summary = candidates[-1] if candidates else (lines[-1] if lines else "")
    if not summary:
        return f"Exit status {returncode}"
    limit = 420
    if len(summary) > limit:
        summary = summary[: limit - 1].rstrip() + "…"
    return summary


class OperationPresenter:
    """Own transient operation presentation state for one window."""

    def __init__(
        self,
        *,
        view: OperationView,
        runner: CommandRunner,
        live_events: LiveEventController,
        map_provider: MapProvider,
        scheduler: Schedule,
        cancel_scheduled: CancelScheduled,
        controls_changed: VoidCallback,
        wall_clock: WallClock | None = None,
        monotonic_clock: MonotonicClock | None = None,
    ) -> None:
        self._view = view
        self._runner = runner
        self._live_events = live_events
        self._map_provider = map_provider
        self._scheduler = scheduler
        self._cancel_scheduled = cancel_scheduled
        self._controls_changed = controls_changed
        self._wall_clock = wall_clock or (lambda: datetime.now().astimezone())
        self._monotonic_clock = monotonic_clock or time.monotonic
        self.operation_result: OperationResult | None = None
        self.post_analysis_status: str | None = None
        self.post_analysis_progress_text: str | None = None
        self._pulse_id: Any | None = None
        self._determinate_progress = False
        self._live_redraw_id: Any | None = None
        self._timed_purpose: str | None = None
        self._operation_started_wall: datetime | None = None
        self._operation_started_monotonic: float | None = None

    def on_runner_event(self, event: RunnerEvent) -> None:
        """Apply one typed command event to the view."""

        if event.kind == "started":
            self._set_operation_started(event.purpose)
            display_name = operation_display_name(event.purpose)
            self._view.set_activity(
                f"{display_name} in progress",
                "The operation is running through the verified native engine.",
            )
        elif event.kind == "helper-starting":
            self._view.append_log(event.message)
            self._view.status_label.set_text(
                "Waiting for administrator authentication…"
            )
        elif event.kind == "helper-ready":
            self._view.append_log(event.message)
            if not self._runner.busy:
                self._view.status_label.set_text(
                    "Ready · Administrator session active"
                )
        elif event.kind == "helper-closed":
            self._view.append_log(event.message)
        elif event.kind == "output":
            self._view.append_log(event.message)
        elif event.kind == "engine":
            self.consume_engine_line(event.message, event.purpose)
        elif event.kind == "progress":
            percent = event.percent or 0.0
            display_name = operation_display_name(event.purpose)
            self._determinate_progress = True
            self._view.progress.set_fraction(percent / 100.0)
            self._view.progress.set_text(
                f"{display_name}: {percent:.2f}%"
            )
            self._view.status_label.set_text(
                f"{display_name} in progress · {percent:.2f}%"
            )
            self._view.set_activity(
                f"{display_name} · {percent:.0f}%",
                "Working through the current verified transaction.",
            )
        elif event.kind == "stop-requested":
            self._view.append_log(event.message)
            self._view.progress.set_text(
                "Stopping after current transaction…"
            )
        elif event.kind in {"stop-delivered", "stop-failed"}:
            self._view.append_log(event.message)
        elif event.kind == "error":
            self._view.show_error(
                "Administrator access failed", event.message
            )
        self._controls_changed()

    def _set_operation_started(self, purpose: str) -> None:
        if (
            purpose in _TIMED_MUTATIONS
            and purpose == self._timed_purpose
            and self._operation_started_wall is not None
            and self._operation_started_monotonic is not None
        ):
            # Treat repeated transport acknowledgements as idempotent.  The
            # request acceptance event owns the wall-clock start boundary.
            return
        self.operation_result = None
        self._determinate_progress = False
        self._view.progress.set_fraction(0.0)
        display_name = operation_display_name(purpose)
        self._view.progress.set_text(f"{display_name} in progress…")
        if purpose in _TIMED_MUTATIONS:
            self._timed_purpose = purpose
            self._operation_started_wall = self._wall_clock()
            self._operation_started_monotonic = self._monotonic_clock()
            self._view.append_log(
                f"{display_name} request started: "
                f"{_format_timestamp(self._operation_started_wall)}"
            )
        if self._pulse_id is None:
            self._pulse_id = self._scheduler(120, self._pulse_progress)

    def _pulse_progress(self) -> bool:
        if not self._runner.busy:
            self._pulse_id = None
            return False
        if not self._determinate_progress:
            self._view.progress.pulse()
        return True

    def command_finished(
        self,
        completion: CommandCompletion,
        *,
        on_success: SuccessCallback | None,
        raw_completion: RawCompletionCallback | None,
    ) -> None:
        """Present one final completion and invoke its continuation once."""

        returncode = completion.returncode
        output = completion.output
        purpose = completion.purpose
        self._determinate_progress = False
        self._cancel_live_redraw()
        self._cancel_pulse()
        self._append_operation_timing(purpose)

        stopped_safely = returncode == 130
        self._view.progress.set_fraction(
            1.0 if returncode == 0 or stopped_safely else 0.0
        )
        self._view.progress.set_text(
            "Stopped safely"
            if stopped_safely
            else ("Complete" if returncode == 0 else "Failed")
        )
        self._controls_changed()
        if raw_completion is not None:
            raw_completion(returncode, output)
            return
        if stopped_safely:
            display_name = operation_display_name(purpose)
            self._view.set_activity(
                f"{display_name} stopped safely",
                "The active journalled transaction reached a recoverable boundary.",
            )
            self._view.append_log(
                f"{display_name} stopped safely. The active journalled "
                "transaction completed before exit."
            )
            self._view.status_label.set_text(
                f"{display_name} stopped safely."
            )
            self.post_analysis_status = (
                f"{display_name} stopped safely · allocation map refreshed"
            )
            self.post_analysis_progress_text = "Stopped safely"
            if on_success is not None:
                on_success(output)
            return
        if returncode == 0:
            self._view.set_activity(
                "Operation complete",
                "The operation completed and the allocation map is being refreshed.",
            )
            presentation = successful_completion(
                purpose, self.operation_result
            )
            self._view.progress.set_text(presentation.progress_text)
            self._view.status_label.set_text(presentation.status_text)
            if presentation.post_analysis_status is not None:
                self.post_analysis_status = (
                    presentation.post_analysis_status
                )
                self.post_analysis_progress_text = (
                    presentation.post_analysis_progress_text
                )
            if on_success is not None:
                on_success(output)
            return
        display_name = operation_display_name(purpose)
        self._view.set_activity(
            f"{display_name} failed",
            "Open the technical activity log for the complete diagnostic.",
        )
        self._view.show_error(
            f"{display_name} failed",
            _failure_summary(output, returncode),
        )

    def _append_operation_timing(self, purpose: str) -> None:
        """Record wall-clock bounds and end-to-end elapsed time for mutations."""

        if (
            purpose != self._timed_purpose
            or self._operation_started_wall is None
            or self._operation_started_monotonic is None
        ):
            return
        finished_wall = self._wall_clock()
        elapsed = self._monotonic_clock() - self._operation_started_monotonic
        display_name = operation_display_name(purpose)
        self._view.append_log(
            f"{display_name} request finished: {_format_timestamp(finished_wall)}"
        )
        self._view.append_log(
            f"{display_name} end-to-end elapsed: {_format_elapsed(elapsed)}"
        )
        self._timed_purpose = None
        self._operation_started_wall = None
        self._operation_started_monotonic = None

    def apply_post_analysis_status(self) -> None:
        """Apply and clear status retained across a map refresh."""

        if self.post_analysis_status is None:
            return
        self._view.status_label.set_text(self.post_analysis_status)
        self._view.progress.set_fraction(1.0)
        self._view.progress.set_text(
            self.post_analysis_progress_text or "Complete"
        )
        self.post_analysis_status = None
        self.post_analysis_progress_text = None

    def request_stop(self) -> None:
        self._runner.request_stop()
        self._controls_changed()

    def defer_close_while_busy(self) -> bool:
        """Return true while close must wait for a safe transaction boundary."""

        if not self._runner.busy:
            return False
        if not self._runner.stop_requested:
            self._runner.request_stop()
        self._view.status_label.set_text(
            "Close postponed until the active journalled transaction stops safely."
        )
        self._controls_changed()
        return True

    def consume_engine_line(self, line: str, purpose: str) -> bool:
        """Apply one typed worker line and schedule the minimum redraw work."""

        map_data = self._map_provider()
        outcome = self._live_events.consume(
            line,
            map_data,
            purpose=purpose,
        )
        if not outcome.consumed:
            return False
        if outcome.operation_result is not None:
            self.operation_result = outcome.operation_result
        for message in outcome.log_messages:
            self._view.append_log(message)
        if outcome.status is not None:
            self._view.status_label.set_text(outcome.status)
        if outcome.map_changed and map_data is not None:
            self._view.disk_map.cells = map_data["cells"]
            if outcome.draw_immediately:
                self._view.disk_map.queue_draw()
            self._schedule_live_redraw()
        if outcome.fragmented_value is not None:
            self._view.fragmented_card.set_value(outcome.fragmented_value)
        if outcome.free_value is not None:
            self._view.free_card.set_value(outcome.free_value)
        return True

    def _schedule_live_redraw(self) -> None:
        if self._live_redraw_id is None:
            self._live_redraw_id = self._scheduler(
                100, self._flush_live_redraw
            )

    def _flush_live_redraw(self) -> bool:
        self._live_redraw_id = None
        self._view.disk_map.queue_draw()
        return False

    def _cancel_live_redraw(self) -> None:
        if self._live_redraw_id is None:
            return
        self._cancel_scheduled(self._live_redraw_id)
        self._live_redraw_id = None
        self._view.disk_map.queue_draw()

    def _cancel_pulse(self) -> None:
        if self._pulse_id is None:
            return
        self._cancel_scheduled(self._pulse_id)
        self._pulse_id = None
