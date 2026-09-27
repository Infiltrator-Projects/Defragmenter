# Native GUI migration status

The shipped `linux-defragger` launcher continues to use the Python GTK desktop. The `linux-defragger-desktop` C++ GTK build target is a development port; it is **not installed**. The filesystem engines, mapper, operation dispatcher and privileged helper remain native.

## Implemented in the native port

- GTK window with volume selection, image opening, analysis, unmount, defragment, growth defrag, recovery, safe Stop, log, progress, allocation view, Test Media launcher and About.
- Native device discovery through `lsblk`, first-party image probe, identity validation before mutations, root-owned journal name compatible with the existing desktop, and operation availability from the authoritative C++ registry.
- Read-only analysis without administrator privileges for readable targets; reusable `pkexec` session and JSON helper protocol for privileged analysis and mutations. Stop uses the helper's queued cooperative signal behavior.
- C++ policy regression covering journal name compatibility, FAT identity, journal gating, mounted volume refusal and operation command construction.

## Required before switching the launcher

1. Port the existing window design, artwork, theme preferences, map tooltips, full metrics, map caching and responsive geometry. The current GTK window is a functional prototype and does not preserve presentation parity.
2. Complete live event parity. `@@LIVE_RESET` currently lacks the source allocation reconstruction, and live range handling does not preserve all overlay accounting. Add bounded full-map validation and a coherent redraw strategy for high cell counts.
3. Match the current device discovery behavior for raw filesystem candidates, root-only Test Media slots, the native probe fallback, disk ordering and preservation of verified identity on refresh. Avoid synchronous `lsblk` and image probes on the GTK thread.
4. Exercise the entire administrator session lifecycle, including login cancellation, helper exit during local analysis, unmount continuation, Stop during startup, pending close, and post-write analysis. Existing `linux-defragger-privileged-helper-lifecycle` currently fails its child-reaping assertion in this workspace.
5. Verify the GTK window on a display, supported filesystem images, and a disposable block device. The current workspace cannot open an X server socket, so visual and interaction checks have not run.
6. Only after those gates, switch `packaging/linux-defragger`, remove the Python GUI from the install manifest and Debian runtime dependencies, and update the packaging/release tests to exercise the native entry point.
