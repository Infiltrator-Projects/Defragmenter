# SPDX-License-Identifier: GPL-3.0-or-later
"""Reusable administrator-session transport for Defragmenter.

This module owns ``pkexec``, the privileged-helper process and its JSON
protocol.  It knows nothing about GTK widgets or ordinary subprocesses.
"""

from __future__ import annotations

from collections import deque
import json
import os
import shutil
import subprocess
import threading
from typing import Any

from core.protocol import EngineEventParser

from .command_models import (
    CommandCompletion,
    CommandRequest,
    CompletionCallback,
    EventCallback,
    RunnerEvent,
    Scheduler,
)


class PrivilegeSession:
    """Own one reusable helper process and at most one privileged request."""

    def __init__(
        self,
        *,
        mapper: str,
        operation_engine: str,
        privileged_helper: str,
        on_event: EventCallback,
        on_complete: CompletionCallback,
        scheduler: Scheduler,
    ) -> None:
        self.mapper = os.path.realpath(mapper)
        self.operation_engine = os.path.realpath(operation_engine)
        self.privileged_helper = privileged_helper
        self._on_event = on_event
        self._on_complete = on_complete
        self._scheduler = scheduler

        self.process: subprocess.Popen[str] | None = None
        self.ready = False
        self.starting = False
        self._active_request: CommandRequest | None = None
        self._write_lock = threading.Lock()
        self._message_lock = threading.Lock()
        self._message_queue: deque[dict[str, Any]] = deque()
        self._message_dispatch_scheduled = False
        self._request_id = 0
        self._active_id: int | None = None
        self._output_parts: list[str] = []
        self._stderr_parts: list[str] = []

    @property
    def active_purpose(self) -> str:
        request = self._active_request
        return request.purpose if request is not None else ""

    def authenticate(self, purpose: str = "") -> None:
        """Start the reusable session without submitting a command."""

        if self.ready or self.starting:
            return
        self._start(purpose)

    def run(self, request: CommandRequest) -> None:
        """Queue one privileged request and start or reuse the helper."""

        if not request.privileged:
            raise ValueError("PrivilegeSession only accepts privileged requests")
        if self._active_request is not None:
            raise RuntimeError("the administrator session already has a request")
        self._active_request = request
        if self.ready:
            self._begin_request()
        elif not self.starting:
            self._start(request.purpose)

    def _emit(self, event: RunnerEvent, *, from_worker: bool = False) -> None:
        if from_worker:
            self._scheduler(self._deliver_event, event)
        else:
            self._deliver_event(event)

    def _deliver_event(self, event: RunnerEvent) -> bool:
        self._on_event(event)
        return False

    def _program_and_args(
        self, arguments: tuple[str, ...]
    ) -> tuple[str, tuple[str, ...]]:
        executable = os.path.realpath(arguments[0])
        if executable == self.mapper:
            return "mapper", arguments[1:]
        if executable == self.operation_engine:
            return "operation-engine", arguments[1:]
        if os.path.basename(arguments[0]) == "udisksctl":
            return "udisksctl", arguments[1:]
        raise RuntimeError(
            f"The privileged helper does not permit: {arguments[0]}"
        )

    def _start(self, purpose: str) -> None:
        pkexec = shutil.which("pkexec")
        if not pkexec:
            self._start_failed(
                "pkexec is not installed; administrator authentication is "
                "unavailable."
            )
            return
        self.starting = True
        self._emit(
            RunnerEvent(
                "helper-starting",
                purpose,
                "Requesting administrator access for this application session…",
            )
        )

        def launcher() -> None:
            try:
                process = subprocess.Popen(
                    [pkexec, self.privileged_helper],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    bufsize=1,
                    start_new_session=True,
                    env={**os.environ, "LC_ALL": "C", "LANG": "C"},
                )
                self.process = process
                threading.Thread(
                    target=self._drain_stderr,
                    args=(process,),
                    daemon=True,
                ).start()
                assert process.stdout is not None
                for raw in process.stdout:
                    try:
                        message = json.loads(raw)
                    except json.JSONDecodeError:
                        self._emit(
                            RunnerEvent(
                                "output",
                                self.active_purpose,
                                "Privileged helper returned invalid data: "
                                + raw.rstrip(),
                            ),
                            from_worker=True,
                        )
                        continue
                    self._queue_message(message)
                returncode = process.wait()
                self._scheduler(self._exited, returncode)
            except Exception as exc:
                self._scheduler(self._start_failed, str(exc))

        threading.Thread(target=launcher, daemon=True).start()

    def _queue_message(self, message: dict[str, Any]) -> None:
        """Queue one helper message without exposing scheduler reordering."""

        should_schedule = False
        with self._message_lock:
            self._message_queue.append(message)
            if not self._message_dispatch_scheduled:
                self._message_dispatch_scheduled = True
                should_schedule = True
        if should_schedule:
            self._scheduler(self._drain_messages)

    def _drain_messages(self) -> bool:
        """Deliver helper messages in the exact order read from its pipe."""

        while True:
            with self._message_lock:
                if not self._message_queue:
                    self._message_dispatch_scheduled = False
                    return False
                message = self._message_queue.popleft()
            self._handle_message(message)

    def _drain_stderr(self, process: subprocess.Popen[str]) -> None:
        if process.stderr is None:
            return
        for line in process.stderr:
            self._stderr_parts.append(line)

    def _send(self, message: dict[str, Any]) -> None:
        process = self.process
        if process is None or process.stdin is None or process.poll() is not None:
            raise RuntimeError("the privileged helper is not running")
        encoded = json.dumps(message, separators=(",", ":")) + "\n"
        with self._write_lock:
            process.stdin.write(encoded)
            process.stdin.flush()

    def _begin_request(self) -> None:
        request = self._active_request
        if request is None or not self.ready:
            return
        try:
            program, helper_args = self._program_and_args(request.arguments)
            self._request_id += 1
            self._active_id = self._request_id
            self._output_parts = []
            self._send(
                {
                    "action": "run",
                    "id": self._active_id,
                    "program": program,
                    "argv": list(helper_args),
                }
            )
        except Exception as exc:
            self._complete(
                CommandCompletion(127, str(exc), request.purpose)
            )

    def _handle_message(self, message: dict[str, Any]) -> bool:
        message_type = str(message.get("type", ""))
        request = self._active_request
        purpose = request.purpose if request is not None else ""

        if message_type == "ready":
            self.ready = True
            self.starting = False
            self._emit(
                RunnerEvent(
                    "helper-ready",
                    purpose,
                    "Administrator session unlocked at launch. Further operations "
                    "will reuse it.",
                )
            )
            self._begin_request()
            return False

        message_id = message.get("id")
        if message_type == "started" and message_id == self._active_id:
            # CommandRunner emitted the one request-lifecycle event when it
            # accepted this command.  The helper acknowledgement must not
            # restart presentation timing or duplicate the timestamp.
            return False
        if message_type == "progress" and message_id == self._active_id:
            try:
                percent = max(
                    0.0, min(100.0, float(message.get("percent", 0.0)))
                )
            except (TypeError, ValueError):
                percent = 0.0
            self._emit(RunnerEvent("progress", purpose, percent=percent))
            return False
        if message_type == "output" and message_id == self._active_id:
            line = str(message.get("line", ""))
            if EngineEventParser.is_event_line(line):
                self._emit(RunnerEvent("engine", purpose, line))
            else:
                self._output_parts.append(line + "\n")
                if request is not None and request.stream_output:
                    self._emit(RunnerEvent("output", purpose, line))
            return False
        if message_type == "error":
            text = str(message.get("message", "privileged helper error"))
            if message_id == self._active_id:
                self._output_parts.append(text + "\n")
            else:
                self._emit(RunnerEvent("output", purpose, text))
            return False
        if message_type == "stop-result":
            if bool(message.get("delivered")):
                self._emit(
                    RunnerEvent(
                        "stop-delivered",
                        purpose,
                        "Stop signal delivered; the engine will exit at the next safe "
                        "transaction boundary.",
                    )
                )
            else:
                self._emit(
                    RunnerEvent(
                        "stop-failed",
                        purpose,
                        "Stop signal was not delivered: "
                        + str(message.get("message", "unknown reason")),
                    )
                )
            return False
        if message_type == "finished" and message_id == self._active_id:
            returncode = int(message.get("returncode", 127))
            output = "".join(self._output_parts)
            self._active_id = None
            self._output_parts = []
            self._complete(CommandCompletion(returncode, output, purpose))
        return False

    def _start_failed(self, message: str) -> bool:
        self.starting = False
        self.ready = False
        request = self._active_request
        if request is not None:
            self._complete(
                CommandCompletion(127, message, request.purpose)
            )
        else:
            self._emit(RunnerEvent("error", message=message))
        return False

    def _exited(self, returncode: int) -> bool:
        was_ready = self.ready
        self.ready = False
        self.starting = False
        self.process = None
        stderr = "".join(self._stderr_parts).strip()
        self._stderr_parts = []
        request = self._active_request
        if request is not None:
            self._active_id = None
            self._complete(
                CommandCompletion(
                    returncode or 127,
                    stderr
                    or "The administrator session ended before the operation "
                    "completed.",
                    request.purpose,
                )
            )
        elif was_ready:
            self._emit(
                RunnerEvent(
                    "helper-closed",
                    message="Administrator session closed.",
                )
            )
        return False

    def _complete(self, completion: CommandCompletion) -> None:
        self._active_request = None
        self._on_complete(completion)

    def request_stop(self, purpose: str) -> bool:
        """Ask the helper to signal its active writer process group."""

        if self._active_request is None:
            self._emit(
                RunnerEvent(
                    "stop-failed",
                    purpose,
                    "The administrator session has no active operation.",
                )
            )
            return False
        try:
            stop_id = (
                self._active_id
                if self._active_id is not None
                else self._request_id
            )
            self._send({"action": "stop", "id": stop_id})
            return True
        except Exception as exc:
            self._emit(
                RunnerEvent(
                    "stop-failed",
                    purpose,
                    "Unable to send stop request to the administrator session: "
                    + str(exc),
                )
            )
            return False

    def shutdown(self) -> None:
        """Request helper shutdown without abandoning an active writer."""

        process = self.process
        if process is None or process.poll() is not None:
            return
        try:
            self._send({"action": "quit"})
        except (BrokenPipeError, OSError, RuntimeError):
            pass
