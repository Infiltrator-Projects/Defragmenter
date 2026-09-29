# Native GUI migration status

The migration is complete for the installed product. `/usr/bin/linux-defragger` launches the installed `linux-defragger-desktop` C++ GTK application, and Debian/local packaging does not install the retired Python GUI runtime. The native desktop is required to preserve the pre-migration polished navigation/page hierarchy and dense physical pixel map; a reduced prototype shell is not considered migration-complete.

## Current native contract

- C++ GTK owns the production window, volume selection, image opening, analysis, unmount, Defragment, Growth Defrag, Recover, safe Stop, activity output, allocation view, Test Media launch and About.
- Native device discovery and first-party probing establish filesystem identity before mutation is offered.
- Read-only analysis runs without administrator privileges when the target is readable; mutation uses the reusable `pkexec` privileged-helper protocol.
- The helper accepts only the documented mutation argument schema, confines journals to the root-owned per-user state directory and launches privileged children with a minimal fixed environment.
- The installed launcher and packaging tests explicitly reject a Python GTK runtime dependency.
- Retained files under `gui/` are source/test compatibility material and are not installed as the production application.

## Remaining presentation/qualification work

Presentation polish, additional live-event richness, map caching and broader display/device qualification may continue without reopening the runtime migration. These are normal product improvements; they are not prerequisites for selecting the already-shipped native desktop.
