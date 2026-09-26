#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise native supervision with live children, without touching a disk."""
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time

helper = sys.argv[1]
child = Path(__file__).with_name("helper_child.py")


def wait_for(predicate):
    deadline = time.monotonic() + 5
    while not predicate():
        assert time.monotonic() < deadline, "timed out waiting for child state"
        time.sleep(0.01)


def run_case(mode, program):
    with tempfile.TemporaryDirectory(prefix="defragger-supervisor-") as temporary:
        marker = Path(temporary) / "child"
        process = subprocess.Popen(
            [helper], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True,
            env={**os.environ, "LD_HELPER_TEST_CHILD": str(child),
                 "LD_HELPER_TEST_MARKER": str(marker)},
        )
        events = queue.Queue()
        close_output = threading.Event()

        def read_events():
            for line in process.stdout:
                event = json.loads(line)
                events.put(event)
                if close_output.is_set():
                    process.stdout.close()
                    return

        reader = threading.Thread(target=read_events, daemon=True)
        reader.start()

        def send(message):
            process.stdin.write(json.dumps(message) + "\n")
            process.stdin.flush()

        def event_of(kind, request_id=None):
            deadline = time.monotonic() + 5
            while True:
                event = events.get(timeout=max(0.01, deadline - time.monotonic()))
                if event["type"] == kind and (
                    request_id is None or event.get("id") == request_id
                ):
                    return event
                assert event["type"] != "error", event
                assert time.monotonic() < deadline, (kind, event)

        pid = None
        try:
            event_of("ready")
            # A single malformed client frame must be bounded and rejected
            # without terminating or desynchronising the persistent root helper.
            send({"action": "ping", "id": 90, "padding": "x" * 70000})
            oversized = events.get(timeout=5)
            assert oversized["type"] == "error", oversized
            send({"action": "ping", "id": 91})
            assert event_of("pong", 91)["id"] == 91

            arguments = ["/dev/test", "--fstype", "fat12"]
            if program == "operation-engine":
                arguments = ["defrag", "/dev/test", "--filesystem", "fat12",
                             "--journal", "/var/lib/linux-defragger/state/1000/test.journal"]
            send({"action": "run", "id": 1, "program": program, "argv": arguments})
            if mode == "immediate-stop":
                send({"action": "stop", "id": 2})
            started = event_of("started", 1)
            pid = started["pid"]
            assert started["pgid"] == pid == os.getpgid(pid)
            if mode != "immediate-stop":
                wait_for(lambda: marker.with_suffix(".ready").exists())
            if mode in ("stop", "queued-stop"):
                if mode == "stop" and program == "operation-engine":
                    event_of("output", 1)
                send({"action": "stop", "id": 2})
            elif mode == "control-eof":
                event_of("output", 1)
                process.stdin.close()
            elif mode == "output-closed":
                event_of("output", 1)
                close_output.set()
                reader.join(timeout=5)
                assert not reader.is_alive(), "protocol output was not closed"

            if mode in ("stop", "queued-stop", "immediate-stop"):
                # Silent analysis must stop well before its ten-second deadline.
                finished = event_of("finished", 1)
                assert finished["returncode"] == 130, finished
                # A follow-on operation is allowed as soon as finished arrives.
                send({"action": "run", "id": 3, "program": program, "argv": arguments})
                second = event_of("started", 3)
                send({"action": "stop", "id": 4})
                assert event_of("finished", 3)["returncode"] == 130
                assert not Path(f"/proc/{second['pid']}").exists(), "child was not reaped"
                send({"action": "quit"})
            elif mode == "output-closed":
                # Even with the control pipe still open, output loss stops/reaps
                # the writer. Then close control so the helper itself can exit.
                wait_for(lambda: marker.with_suffix(".stopped").exists())
                wait_for(lambda: not Path(f"/proc/{pid}").exists())
                process.stdin.close()

            process.wait(timeout=5)
            expected = 1 if mode == "output-closed" else 0
            assert process.returncode == expected, process.stderr.read()
            assert not Path(f"/proc/{pid}").exists(), "child was not reaped"
            assert marker.with_suffix(".stopped").read_text() == "safe boundary reached"
        finally:
            if process.poll() is None:
                process.stdin.close()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    if pid is not None:
                        try:
                            os.killpg(pid, 9)
                        except ProcessLookupError:
                            pass
                    process.kill()
                    process.wait()
            reader.join(timeout=1)


for mode in ("stop", "queued-stop", "immediate-stop", "control-eof", "output-closed"):
    run_case(mode, "operation-engine")
run_case("stop", "mapper")
print("native helper child Stop, queued Stop, follow-on, EOF and broken-output tests passed")
