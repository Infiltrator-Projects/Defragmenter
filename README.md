<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Defragmenter

**Unified filesystem architecture:** [one authoritative `.so` per format, shared by Defragmenter, BOOT and the kernel](docs/FILESYSTEM_ENGINE_OWNERSHIP.md). Defragmenter owns generic safety, scheduling and I/O orchestration, while the `.so` owns format-specific allocation, relocation, mutation and recovery. Current internal filesystem engines are transitional until independently proven equivalent to their loaded modules.

**Project copyright:** © 1993-2026 Shannon Smith

[![Project quality gate](https://github.com/Infiltrator-Projects/Defragmenter/actions/workflows/quality-gate.yml/badge.svg)](https://github.com/Infiltrator-Projects/Defragmenter/actions/workflows/quality-gate.yml)

Defragmenter is a C-first offline filesystem allocation analyser and defragmenter for Linux. Native C currently contains transitional filesystem-specific engines and owns the storage-safety core; in the target architecture the matching installed `.so` exclusively owns filesystem semantics and operations; C++17 owns selected filesystem-neutral application services where RAII, stronger value types and explicit process/protocol ownership improve the implementation. Write-capable engines operate directly on unmounted block devices or filesystem images and do not delegate production mutations to mounted kernel filesystem drivers or external repair/defragmentation tools.

**Version authority:** `defragger/VERSION` is the source authority; a published version is additionally bound to its exact qualified `main` commit, version tag and verified release assets.

**Platform:** Linux

**Licence:** GPL-3.0-or-later

> **Safety status:** The completed release safety decision is recorded in `docs/AUDIT_STATUS.md` rather than duplicated here. Defragment, Growth Defrag and Recover remain behind exact target confirmation, mounted-target refusal, durable filesystem-specific recovery and final verification. The separate Test Media utility is deliberately destructive and must be used only on sacrificial targets.

## Engineering ethos

What happens when filesystem analysis and defragmentation are built from first principles, from the on-disk structures upward, rather than delegated to whatever repair tool happens to be installed? Defragmenter is where allocation maps, placement policy, recovery and verified mutation become first-party code.

Filesystem specifications, mature implementations and external tools are evidence to study, not runtime authorities for project behaviour. The operating system may provide raw I/O and Common may provide proven neutral primitives, but filesystem parsing, safety policy, placement planning, staging, recovery and final verification remain owned here. Unsupported or uncertain layouts fail closed instead of being guessed.

The project prefers the strongest justified method, not automatically the newest one. A replacement earns its place by improving safety, correctness, recoverability, performance or clarity, and write support is not considered complete until interruption and recovery paths are testable.

## Appearance

The graphical dashboard direction is captured in [`docs/UI_VISION.md`](docs/UI_VISION.md), including the approved concept image. The left rail is navigation rather than a duplicate command surface: Overview is the dashboard, while Analyse, Defragment, Growth Defrag, Recover, Test Media and Settings open dedicated pages with one relevant primary action. The complete concept screenshot is documentation-only. Runtime presentation uses three dedicated standalone raster assets — SSD identity art, hero landscape and Workbench-inspired sidebar scene — installed beside the GTK UI; the application never crops or displays pieces of the prototype screen. The authoritative allocation map remains a physically positional pixel raster: left-to-right and then top-to-bottom advances monotonically through the analysed on-disk address space, with no Hilbert, Morton/Z-order or other aesthetic remapping. Every painted device pixel represents one or more whole physical storage units. When storage units outnumber pixels, adjacent ranges are combined in physical order; when the canvas exceeds the available storage detail, surplus canvas remains unused. Storage units are never magnified to fill the window. The caption reports the actual integer unit count or range per mapped pixel, and aggregated analyser cells are never subdivided into invented detail. This remains a continuous pixel raster without square grid cells. The Overview no longer carries a duplicate miniature allocation map: the main map is the elastic vertical surface and shrinks first on laptop work areas so the action row, compact activity strip and footer remain visible without needless scrolling.

The main GTK application supports **Follow system**, **Day** and **Night** appearance modes. Follow system detects the host GTK/Mint light/dark preference and resolves it to the exact Common Day or Night palette; it does not inherit an unrelated toolkit palette. Common 1.19.46 supplies the complete layered Linux MBLink reference face for Night — including titlebar, connection, heading, summary, detail, note, state-border and hover-accent roles — while Day supplies the matching white semantic palette. Defragmenter resolves the native Common design API directly and maps those neutral roles into its GTK selectors without redefining a private palette. The selected mode is persisted per user and synchronised across open windows.

Typography is deliberately closed to the three verified shared MB Corpo faces: MB Corpo A Condensed for primary titles, MB Corpo S Regular for normal interface text and MB Corpo S Bold for emphasis. Defragmenter and Test Media do not request generic system or monospace fallback families; the Debian and local installers establish one persistent, checksum-verified native font cache, reusing a local seed when available.

The desktop, window and About surfaces use one approved 96×96 Defragmenter artwork asset in the same non-automotive Infiltrator desktop family as System Monitor: a dark graphite tile, #00ADEF cyan linework and a simple defragmentation glyph showing scattered blocks compacting into an ordered group. Packaging installs the same source asset into the desktop icon theme, Mint app-install icon path and the application's private runtime path; the GTK application resolves those installed copies deterministically before consulting the icon theme, so a missing cache entry cannot silently turn the About/window artwork into a placeholder. The process also publishes the same `io.github.defragmenter` identity as its GTK application ID, X11 program class and desktop `StartupWMClass`, so Cinnamon can associate the running native C++ GTK window with the packaged desktop icon instead of falling back to a generic taskbar glyph.

## Capabilities

The project contains native analysis support across FAT12/16/32, exFAT, NTFS, ext2/3/4, XFS v5, Amiga OFS/FFS/SFS/PFS3 variants, HFS/HFS+, Btrfs, APFS, Minix, UFS, ZFS/OpenZFS members and Linux swap, with write support implemented only where the filesystem-specific engine has an explicit contract.

Production operations are:

- **Analyse / Map** — read-only allocation and fragmentation analysis.
- **Defragment** — canonical placement of supported movable allocations.
- **Growth Defrag** — canonical placement with an exact 10% post-file reserve.
- **Recover** — resume/repair of supported interrupted persistent transactions.

Unsupported layouts fail closed rather than being guessed. Write-capable engines perform verified staging/recovery and a final read-only rescan before reporting success.

Linux filesystem metadata and Test Media GPT labels are routing hints, not write authority. New physical-volume layout operations require a successful first-party native Analyse to identify and qualify the filesystem. Recover instead requires an existing journal and delegates journal schema, target binding, stage integrity and recovery-phase validation to the native worker, because an interrupted source may not be analysable. Root-only Test Media slots may therefore appear briefly as identity-pending candidates; a stale or mismatched label cannot enable a writer.

## Architecture

The canonical implementation lives under `defragger/`.

Most filesystem implementations are organised below `defragger/gui/filesystems/<format>/native/`. FAT12/16/32 analysis, relocation and recovery live in the matching installed Filesystems `.so`, called through a versioned child-process host. Filesystem-neutral device safety, raw I/O and Stop handling live under `defragger/src/core/`. The C++17 application-service layer in `defragger/native/` is the sole registry, allocation-map translation, operation-dispatch, privileged-helper control plane and installed GTK desktop. There is no retained Python application, presentation, capability or privilege-policy layer; Python under `defragger/tests/` is limited to independent fixture construction and black-box test orchestration.

The operating system supplies raw block I/O, but filesystem parsing, placement planning, staging and metadata updates are owned by the project. Architecture and regression tests reject known external filesystem mutation/repair orchestration and duplicate implementation paths.

Shared first-party primitives are consumed from the exact pinned Common 1.19.46 dependency. Common supplies generic numeric, arithmetic, byte-order, exact-I/O, durable-file, design/typography and font-provenance contracts. Defragmenter retains generic target safety and supplies host services to the installed FAT module, which owns FAT interpretation, placement and recovery.

## Build and test

From the canonical project directory:

```bash
cd defragger
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLD_ENABLE_WERROR=ON
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')
[ "$jobs" -gt 1 ] && jobs=$((jobs - 1))
cmake --build build -j"$jobs"
ctest --test-dir build --output-on-failure
```

The permanent GitHub quality gate performs warnings-as-errors C/C++ builds and runs the complete native, filesystem, GUI, architecture, safety and release regression suite, including native-helper shutdown with live child processes, mounted-image hard-link identity, large-map capture, deterministic malformed-media rejection and C++ mapper fixture-parity checks. An opt-in dm-log-writes/replay-log harness adds sacrificial block-layer crash-boundary qualification without pretending that hosted CI is physical-media evidence.

## Release assets

A numbered release publishes:

| File | Purpose |
| --- | --- |
| `Defragmenter-<version>-amd64.deb` | Generic amd64 Debian package. |
| `Defragmenter-<version>-local-folder.run` | Hardware-native local compile/install program. |
| `RELEASE_SHA256SUMS.txt` | SHA-256 checksums for the two project release artifacts. |

The `.deb` keeps the human-facing `Defragmenter-...` asset filename while its Debian/APT package identity is `defragmenter`; updates replace the existing `defragmenter` package through normal APT version ordering. This alpha release provides no legacy package-name migration aliases.

The local `.run` embeds the exact committed Defragmenter source for the qualified revision plus the exact pinned Common commit. Its source inventory comes from Git objects rather than the ambient build directory, so untracked runner logs, temporary files and other workspace debris cannot enter a qualified installer. Python test drivers are not part of the production source payload.

GitHub automatically provides its standard `Source code (zip)` and `Source code (tar.gz)` links for every release tag. Defragmenter does not upload a duplicate custom source archive.

## Repository and release policy

This repository uses `main` as its working branch. Development changes are made directly on `main`; the normal project workflow does not depend on PR, feature or release branches.

Every push to `main` runs the project quality gate. Ordinary commits do not publish. A change to `defragger/VERSION` is the explicit release decision; commit-message wording is not part of the release control plane. The exact versioned `main` commit becomes release-eligible only after both permanent qualification lanes succeed.

The release workflow verifies permanent `main` history protection against deletion, force pushes and nonlinear history, then publishes only from that exact qualified commit. Release artefacts are built once by qualification and handed unchanged to publication; the publication workflow never rebuilds them. The audited source baseline covers every tracked input that can alter what qualification builds, installs or accepts, including tests and the source-install entry point, so weakening a qualification test after audit fails closed in the same way as changing production source.

Version identity is write-once by project policy and enforced by the publisher: an existing tag must already point at the qualified commit, an existing release must target that commit, unexpected or duplicate assets are rejected, and every published asset digest must match the qualified artefact. The project does not rely on GitHub's separate platform-level immutable-release flag for those guarantees. Published tags/releases must not be moved, retargeted or edited in place. Manually runnable quality-gate helpers are diagnostic tools only and are not release-approval mechanisms.

## Documentation

- `docs/ARCHITECTURE.md` — ownership, layers, safety boundaries and system contracts.
- `docs/DESIGN.md` — first-principles goals, non-goals, trade-offs and failure philosophy.
- `docs/DECISIONS.md` — durable architectural choices and consequences.
- `docs/VALIDATION.md` — automated, destructive-path, manual and release evidence boundaries.
- `docs/AUDIT_STATUS.md` — current write-safety case and exact audited baselines.
- `docs/REFERENCES.md` — filesystem/platform technical references.
- `defragger/README.md` — source-tree layout, filesystem-support matrix and local build notes.
- `defragger/VERSION` — current source version.

## Safety

Defragmentation changes filesystem allocation metadata and data placement. Keep verified backups and use sacrificial/test media during development and validation. The **Defragmenter Test Media** utility is intentionally destructive and must never be pointed at a system/boot disk or irreplaceable media.

## Licence

Copyright © 1993-2026 Shannon Smith.

Defragmenter first-party code, scripts, tests, packaging and documentation are licensed under the GNU General Public License version 3 or, at your option, any later version (`GPL-3.0-or-later`). The canonical licence text is `LICENSE`.

Defragmenter Test Media includes first-party native creators and raw verification for all 21 slots. It builds the bounded qualification corpus without host filesystem formatters or target filesystem drivers. After upgrading from schema 2, rebuild an existing test disk before qualification or verification; schema 3 derives every expected payload from the compiled corpus.
