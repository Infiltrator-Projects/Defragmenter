#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Standard operation names and common direct-offline command policy."""

from __future__ import annotations

from enum import Enum


class Operation(str, Enum):
    DEFRAG = "defrag"
    GROWTH_DEFRAG = "growth-defrag"
    RECOVER = "recover"

    @classmethod
    def parse(cls, value: str) -> "Operation":
        try:
            return cls(value)
        except ValueError as exc:
            allowed = ", ".join(item.value for item in cls)
            raise ValueError(f"unknown operation {value!r}; expected one of: {allowed}") from exc


DISPLAY_NAMES = {
    Operation.DEFRAG: "Defragment",
    Operation.GROWTH_DEFRAG: "Growth Defrag",
    Operation.RECOVER: "Recover",
}


def build_standard_arguments(operation: str, live_cells: int) -> list[str]:
    """Return operation-wide tuning flags independent of filesystem choice."""

    selected = Operation.parse(operation)
    # Worker concurrency is filesystem-engine policy. Do not advertise a
    # generic --workers switch when most engines do not consume it.
    common = ["--ram-buffer", "auto"]
    if selected is Operation.DEFRAG:
        return ["--batch-clusters", "4096", *common, "--live-map-cells", str(live_cells)]
    if selected is Operation.GROWTH_DEFRAG:
        return [
            "--growth-percent",
            "10",
            "--batch-clusters",
            "4096",
            *common,
            "--live-map-cells",
            str(live_cells),
        ]
    return common
