# Native GUI migration status

The shipped `linux-defragger` launcher continues to use the Python GTK desktop. The `linux-defragger-desktop` C++ GTK build target is a development port; it is **not installed**. The filesystem engines, mapper, operation dispatcher and privileged helper remain native.

## Implemented in the native port

- GTK window with volume selection, image opening, analysis, unmount, defragment, growth defrag, recovery, safe Stop, log, progress, allocation view, Test Media launcher and About.
- Native device discovery through `lsblk`, first-party image probe, identity validation before mutations, root-owned journal name compatible with the existing desktop, and operation availability from the authoritative C++ registry.
- Read-only analysis without administrator privileges for readable targets; reusable `pkexec` session and JSON helper protocol for privileged analysis and mutations. Stop uses the helper's queued cooperative signal behavior.
- C++ policy regression covering journal name compatibility, FAT identity, journal gating, mounted volume refusal and operation command construction.
- Native live allocation reset, range and cell updates with bounded inputs and regression coverage for legacy NTFS ranges and rejected deltas.
- Stop requested during administrator startup cancels the pending operation before it is submitted. Volume and image selection are disabled during an active operation.

## Required before switching the launcher

1. Port the existing window design, artwork, theme preferences, map tooltips, full metrics, map caching and responsive geometry. The current GTK window is a functional prototype and does not preserve presentation parity.
2. Complete live event parity. The reset now reconstructs source allocation and the range updater preserves fragmented and directory proportions. Add status and metrics parity, full-map schema validation, and a coherent redraw strategy for high cell counts.
3. Match the current device discovery behavior for raw filesystem candidates, root-only Test Media slots, the native probe fallback, disk ordering and preservation of verified identity on refresh. Avoid synchronous `lsblk` and image probes on the GTK thread.
4. Exercise the entire administrator session lifecycle, including login cancellation, helper exit during local analysis, unmount continuation, pending close, and post-write analysis. The helper lifecycle test has intermittently failed its child-reaping assertion in this workspace; its process identity check now accounts for PID reuse, but all interaction paths still need display and device verification.
5. Verify the GTK window on a display, supported filesystem images, and a disposable block device. The current workspace cannot open an X server socket, so visual and interaction checks have not run.
6. Only after those gates, switch `packaging/linux-defragger`, remove the Python GUI from the install manifest and Debian runtime dependencies, and update the packaging/release tests to exercise the native entry point.
