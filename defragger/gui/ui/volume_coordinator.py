# SPDX-License-Identifier: GPL-3.0-or-later
"""GTK-neutral volume discovery, image opening, selection and map caching."""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

from .backend_catalog import BackendCatalog
from .devices import Volume, discover_volumes
from .engine_client import detect_image_fstype
from .support import safe_journal_name
from .volume_store import VolumeStore


DiscoverVolumes = Callable[[BackendCatalog], list[Volume]]
DetectImageFilesystem = Callable[[str, BackendCatalog], str]


@dataclass(frozen=True, slots=True)
class VolumeSelection:
    """Selection result consumed by the window presentation layer."""

    volume: Volume | None
    cached_map: dict[str, Any] | None

    @property
    def needs_analysis(self) -> bool:
        return self.volume is not None and self.cached_map is None


class VolumeCoordinator:
    """Own the complete per-window volume lifecycle without importing GTK."""

    def __init__(
        self,
        catalog: BackendCatalog,
        *,
        discover: DiscoverVolumes = discover_volumes,
        detect_image: DetectImageFilesystem = detect_image_fstype,
    ) -> None:
        self.catalog = catalog
        self.store = VolumeStore()
        self._discover = discover
        self._detect_image = detect_image

    @property
    def current(self) -> Volume | None:
        return self.store.current

    @property
    def volumes(self) -> tuple[Volume, ...]:
        return tuple(self.store.volumes)

    def discover(self) -> list[Volume]:
        """Return a fresh physical-volume snapshot without mutating GUI state."""

        return self._discover(self.catalog)

    def apply_discovery(
        self,
        discovered: list[Volume],
        *,
        preserve_path: str | None = None,
        clear_cache: bool = False,
    ) -> int:
        """Apply a completed discovery snapshot on the owning GUI thread."""

        return self.store.refresh(
            discovered,
            preserve_path=preserve_path,
            clear_cache=clear_cache,
        )

    def refresh(
        self,
        *,
        preserve_path: str | None = None,
        clear_cache: bool = False,
    ) -> int:
        """Synchronously refresh physical devices for non-GTK callers/tests."""

        return self.apply_discovery(
            self.discover(),
            preserve_path=preserve_path,
            clear_cache=clear_cache,
        )

    def select(self, index: int) -> VolumeSelection:
        """Select from the current snapshot without performing blocking I/O."""

        self.store.select(index)
        return VolumeSelection(self.current, self.store.cached_map())

    def revalidate_selected(
        self,
        path: str,
        discovered: list[Volume],
    ) -> VolumeSelection:
        """Apply a background identity refresh for the currently selected path."""

        selected = self.current
        if selected is None or selected.path != path or selected.image:
            return VolumeSelection(self.current, self.store.cached_map())

        fresh = next((volume for volume in discovered if volume.path == path), None)
        if fresh is None:
            self.store.invalidate(path)
            selected.identity_verified = False
            return VolumeSelection(selected, None)

        if (
            selected.identity_verified
            and not fresh.identity_verified
            and selected.size == fresh.size
            and (
                (
                    selected.filesystem_uuid
                    and fresh.filesystem_uuid
                    and selected.filesystem_uuid.casefold()
                    == fresh.filesystem_uuid.casefold()
                )
                or (
                    selected.partition_uuid
                    and fresh.partition_uuid
                    and selected.partition_uuid.casefold()
                    == fresh.partition_uuid.casefold()
                )
            )
            and (
                selected.normalized_fstype == fresh.normalized_fstype
                or {
                    selected.fstype.casefold(),
                    fresh.fstype.casefold(),
                } <= {"fat", "vfat", "msdos", "fat12", "fat16", "fat32"}
            )
        ):
            fresh.fstype = selected.fstype
            fresh.fs_version = selected.fs_version
            fresh.identity_verified = True

        self.store.replace(fresh)
        return VolumeSelection(self.current, self.store.cached_map())

    def accept_native_identity(self, filesystem: str) -> Volume:
        """Promote the selected volume only after native analysis proves it."""

        volume = self.current
        actual = str(filesystem or "").strip().lower()
        if volume is None:
            raise ValueError("no selected volume exists for native identity proof")
        if not actual or not self.catalog.supports(actual):
            raise ValueError(
                "the native analyser returned an unsupported filesystem identity"
            )

        candidate = volume.fstype.strip().lower()
        candidate_backend = self.catalog.normalize(candidate)
        actual_backend = self.catalog.normalize(actual)
        generic_fat = {"fat", "vfat", "msdos"}
        fat_variants = {"fat12", "fat16", "fat32"}
        same_family = candidate_backend == actual_backend
        if candidate in generic_fat and actual in fat_variants:
            same_family = True
        if not same_family:
            raise ValueError(
                f"native identity {actual!r} conflicts with discovery candidate "
                f"{candidate!r}"
            )

        self.store.invalidate(volume.path)
        # Preserve a useful shared-backend alias when the mapper reports only
        # the backend id (for example OFS/FFS -> AFFS). Otherwise prefer the
        # more precise native identity.
        if candidate in generic_fat or actual != actual_backend or candidate == actual_backend:
            volume.fstype = actual
            if actual in fat_variants:
                volume.fs_version = actual.upper()
        volume.identity_verified = True
        return volume

    def open_image(self, filename: str) -> Volume:
        """Validate, model and select a filesystem image."""

        path = str(Path(filename).resolve())
        fstype = self._detect_image(path, self.catalog)
        size = Path(path).stat().st_size
        volume = Volume(
            catalog=self.catalog,
            path=path,
            name=Path(path).name,
            fstype=fstype,
            label=Path(path).name,
            size=size,
            mountpoints=[],
            removable=False,
            readonly=not os.access(path, os.W_OK),
            model="filesystem image",
            transport="file",
            image=True,
            identity_verified=True,
        )
        self.store.add_image(volume)
        return volume

    def remember_map(self, data: dict[str, Any]) -> None:
        self.store.remember_map(data)

    def cached_map(self) -> dict[str, Any] | None:
        return self.store.cached_map()

    def invalidate(self, path: str) -> None:
        self.store.invalidate(path)

    def journal_path(self, state_directory: Path) -> str:
        volume = self.current
        if volume is None:
            return ""
        identity = "|".join(
            value
            for value in (volume.filesystem_uuid, volume.partition_uuid)
            if value
        )
        return str(
            state_directory /
            f"{safe_journal_name(volume.path, identity)}.journal"
        )
