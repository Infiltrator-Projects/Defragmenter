#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression coverage for NTFS live allocation-map reset events."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUI = ROOT / "gui"
if str(GUI) not in sys.path:
    sys.path.insert(0, str(GUI))

from ui.live_map import LiveMapUpdater


def _map() -> dict:
    return {
        "filesystem": "ntfs",
        "unit_size": 4096,
        "total_units": 16,
        "filesystem_units": 16,
        "cells": [
            {
                "start": 0,
                "end": 7,
                "free": 0,
                "used": 8,
                "unknown": 0,
                "outside": 0,
                "fragmented": 4,
                "directory": 1,
                "bad": 0,
            },
            {
                "start": 8,
                "end": 15,
                "free": 0,
                "used": 8,
                "unknown": 0,
                "outside": 0,
                "fragmented": 2,
                "directory": 0,
                "bad": 0,
            },
        ],
    }


def test_standard_live_reset() -> None:
    data = _map()
    LiveMapUpdater(data).reset(
        {
            "filesystem": "ntfs",
            "unit_size": 4096,
            "filesystem_units": 16,
            "used_ranges": [[0, 4 * 4096], [8 * 4096, 4 * 4096]],
        }
    )
    assert [(cell["used"], cell["free"]) for cell in data["cells"]] == [
        (4, 4),
        (4, 4),
    ]
    assert all(cell["fragmented"] == 0 for cell in data["cells"])



def test_reason_only_live_reset_is_noop() -> None:
    data = _map()
    before = [dict(cell) for cell in data["cells"]]
    LiveMapUpdater(data).reset({"reason": "authoritative post-recovery map"})
    assert data["cells"] == before


def test_live_reset_rejects_missing_used_ranges() -> None:
    data = _map()
    try:
        LiveMapUpdater(data).reset(
            {"unit_size": 4096, "filesystem_units": 16}
        )
    except ValueError as error:
        assert "used-range" in str(error)
    else:
        raise AssertionError("missing used_ranges was accepted")

def main() -> None:
    test_standard_live_reset()
    test_reason_only_live_reset_is_noop()
    test_live_reset_rejects_missing_used_ranges()
    print("NTFS live-map reset regression tests passed")


if __name__ == "__main__":
    main()
