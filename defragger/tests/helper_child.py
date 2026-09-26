#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Controlled child for the test-only native supervisor (never installed)."""
import os
from pathlib import Path
import signal
import sys
import time

marker = Path(os.environ["LD_HELPER_TEST_MARKER"])
stopped = False


def stop(_signal, _frame):
    global stopped
    stopped = True


signal.signal(signal.SIGINT, stop)
# A supervised worker must not inherit and consume the GUI control channel.
assert os.read(0, 1) == b""
marker.with_suffix(".ready").write_text(str(os.getpid()))
writer = sys.argv[1] == "defrag"
if writer:
    # Exercise a queued Stop while the handler is installed but before the
    # worker explicitly declares that cooperative Stop delivery is safe.
    time.sleep(0.25)
    print("@@STOP_READY", flush=True)
    print("worker initialised", flush=True)
deadline = time.monotonic() + 10
while not stopped and time.monotonic() < deadline:
    if writer:
        print("worker checkpoint", flush=True)
    time.sleep(0.02)
if not stopped:
    raise SystemExit(3)
# Model a cooperative writer finishing its safe boundary after receiving SIGINT.
time.sleep(0.15)
marker.with_suffix(".stopped").write_text("safe boundary reached")
raise SystemExit(130)
