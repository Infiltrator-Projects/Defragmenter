# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure GUI operation policy and standard command construction."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Mapping

from core.operations import build_standard_arguments
from .backend_catalog import BackendCatalog
from .devices import Volume


KNOWN_MUTATIONS = frozenset({"defrag", "growth-defrag", "recover"})


class OperationValidationError(RuntimeError):
    """A user-facing validation failure with a stable dialog title."""

    def __init__(self, title: str, message: str) -> None:
        super().__init__(message)
        self.title = title
        self.message = message


@dataclass(frozen=True, slots=True)
class MutationPlan:
    operation: str
    operation_name: str
    arguments: tuple[str, ...]
    confirmation_title: str
    confirmation_message: str


@dataclass(frozen=True, slots=True)
class ControlState:
    refresh: bool
    select_device: bool
    analyse: bool
    unmount: bool
    defrag: bool
    growth_defrag: bool
    recover: bool
    stop: bool


def build_analysis_arguments(
    mapper: str,
    volume: Volume,
    cells: int,
    *,
    minimum_cells: int,
    maximum_cells: int,
) -> tuple[str, ...]:
    bounded = max(minimum_cells, min(maximum_cells, int(cells)))
    return (
        mapper,
        volume.path,
        "--fstype",
        volume.normalized_fstype,
        "--cells",
        str(bounded),
    )


def prepare_mutation(
    operation: str,
    volume: Volume,
    catalog: BackendCatalog,
    *,
    operation_engine: str,
    journal_path: str,
    live_cells: int,
    minimum_cells: int,
    maximum_cells: int,
) -> MutationPlan:
    """Validate one mutation and return its complete standard command."""

    if operation not in KNOWN_MUTATIONS:
        raise OperationValidationError(
            "Unknown operation", f"Defragmenter does not recognise {operation!r}."
        )
    operation_manifest = catalog.operations_for(volume.normalized_fstype).get(operation)
    if operation_manifest is None:
        raise OperationValidationError(
            "Operation unavailable",
            f"The {volume.normalized_fstype.upper()} native backend manifest does not "
            f"advertise {operation}.",
        )
    if volume.readonly:
        raise OperationValidationError(
            "Read-only volume", f"{volume.path} is marked read-only."
        )
    if volume.mounted:
        raise OperationValidationError(
            "The volume is mounted",
            "Unmount it first. Filesystem mutation engines intentionally refuse "
            "mounted volumes.",
        )

    journal_exists = Path(journal_path).exists()
    if operation != "recover" and journal_exists:
        raise OperationValidationError(
            "Recovery is required",
            f"An unfinished journal exists at:\n{journal_path}\n\n"
            "Run Recover before any other operation.",
        )
    if operation == "recover" and not journal_exists:
        raise OperationValidationError(
            "No recovery journal",
            "There is no unfinished transaction for this volume.",
        )

    description = str(operation_manifest.get("description") or operation)
    warning = str(operation_manifest.get("warning") or "").strip()
    operation_name = str(
        operation_manifest.get("label") or operation.replace("-", " ").title()
    )
    confirmation_message = description
    if warning:
        confirmation_message += f"\n\n{warning}"
    confirmation_message += (
        "\n\nThe volume must remain connected and unmounted. A clean Stop request "
        "finishes the active journalled transaction before exiting."
    )

    bounded_cells = max(minimum_cells, min(maximum_cells, int(live_cells)))
    arguments = [
        operation_engine,
        operation,
        volume.path,
        "--filesystem",
        volume.normalized_fstype,
        "--write",
        "--confirm",
        volume.path,
        "--journal",
        journal_path,
    ]
    arguments.extend(build_standard_arguments(operation, bounded_cells))
    return MutationPlan(
        operation=operation,
        operation_name=operation_name,
        arguments=tuple(arguments),
        confirmation_title=f"{operation_name} {volume.path}?",
        confirmation_message=confirmation_message,
    )


def control_state(
    volume: Volume | None,
    *,
    busy: bool,
    stop_requested: bool,
    journal_exists: bool,
) -> ControlState:
    enabled = volume is not None and not busy
    mounted = bool(volume and volume.mounted)
    operations = volume.operations if volume else {}
    mutation_backend = bool(operations)
    can_offer_write = (
        enabled
        and mutation_backend
        and not bool(volume and volume.readonly)
    )
    return ControlState(
        refresh=not busy,
        select_device=not busy,
        analyse=enabled,
        unmount=enabled and mounted and not bool(volume and volume.image),
        defrag=can_offer_write and "defrag" in operations and not journal_exists,
        growth_defrag=(
            can_offer_write
            and "growth-defrag" in operations
            and not journal_exists
        ),
        recover=can_offer_write and "recover" in operations and journal_exists,
        stop=busy and not stop_requested,
    )


def operation_tooltips(
    volume: Volume | None,
    catalog: BackendCatalog,
) -> Mapping[str, str]:
    if volume is None:
        return {
            "defrag": (
                "Select a volume to see whether its filesystem plugin provides "
                "Defragment."
            ),
            "growth-defrag": (
                "Select a volume to see whether its filesystem plugin provides "
                "Growth Defrag."
            ),
            "recover": (
                "Recover is available only when the selected plugin provides a "
                "raw journal recovery worker."
            ),
        }

    filesystem = volume.normalized_fstype.upper()
    manifests = catalog.operations_for(volume.normalized_fstype)
    result: dict[str, str] = {}
    for operation, label in (
        ("defrag", "Defragment"),
        ("growth-defrag", "Growth Defrag"),
        ("recover", "Recover"),
    ):
        manifest = manifests.get(operation)
        if manifest is None:
            result[operation] = (
                f"{filesystem} analysis is available, but this build has no verified "
                f"direct raw-offline {label} writer for that filesystem."
            )
            continue
        description = str(manifest.get("description") or label)
        warning = str(manifest.get("warning") or "").strip()
        text = description + (f"\n\n{warning}" if warning else "")
        if volume.mounted and not volume.image:
            text += (
                "\n\nThis volume is mounted. Defragmenter will ask to "
                "unmount it safely before starting the write operation."
            )
        result[operation] = text
    return result
