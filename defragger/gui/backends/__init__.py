# SPDX-License-Identifier: GPL-3.0-or-later
"""Minimal installed GUI backend contract surface.

Production backend discovery, mapping and mutation are owned by the native C++
registry and filesystem workers.  The legacy Python Registry remains available
lazily in the source tree for migration/parity tests without pulling the old
plugin graph into the installed GTK process.
"""

from .contracts import BackendError, FilesystemBackend

__all__ = ["BackendError", "FilesystemBackend", "Registry"]


def __getattr__(name: str):
    if name != "Registry":
        raise AttributeError(name)
    from .registry import Registry
    return Registry
