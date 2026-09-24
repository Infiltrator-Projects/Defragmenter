<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Defragmenter

**Project copyright:** © 1993-2026 Shannon Smith

[![Project quality gate](https://github.com/Infiltrator-Projects/Defragmenter/actions/workflows/quality-gate.yml/badge.svg)](https://github.com/Infiltrator-Projects/Defragmenter/actions/workflows/quality-gate.yml)

Defragmenter is a C-first offline filesystem allocation analyser and defragmenter for Linux. Native C owns the raw filesystem engines and storage-safety core; C++17 owns selected filesystem-neutral application services where RAII, stronger value types and explicit process/protocol ownership improve the implementation. Write-capable engines operate directly on unmounted block devices or filesystem images and do not delegate production mutations to mounted kernel filesystem drivers or external repair/defragmentation tools.

**Current version:** 1.8.0-196

**Platform:** Linux

**Licence:** GPL-3.0-or-later

> **Safety status:** The version 1.8.0-196 filesystem-safety audit is complete. Defragment, Growth Defrag and Recover are enabled behind exact target confirmation, mounted-target refusal, durable filesystem-specific recovery and final verification. The separate Test Media utility is deliberately destructive and must be used only on sacrificial targets. See `docs/AUDIT_STATUS.md`.

## Engineering ethos

What happens when filesystem analysis and defragmentation are built from first principles, from the on-disk structures upward, rather than delegated to whatever repair tool happens to be installed? Defragmenter is where allocation maps, placement policy, recovery and verified mutation become first-party code.

Filesystem specifications, mature implementations and external tools are evidence to study, not runtime authorities for project behaviour. The operating system may provide raw I/O and Common may provide proven neutral primitives, but filesystem parsing, safety policy, placement planning, staging, recovery and final verification remain owned here. Unsupported or uncertain layouts fail closed instead of being guessed.

The project prefers the strongest justified method, not automatically the newest one. A replacement earns its place by improving safety, correctness, recoverability, performance or clarity, and write support is not considered complete until interruption and recovery paths are testable.

## Appearance

The main GTK application supports **Follow system**, **Day** and **Night** appearance modes. Follow system detects the host GTK/Mint light/dark preference and resolves it to the exact Common Day or Night palette; it does not inherit an unrelated toolkit palette. Common 1.19.24 supplies the complete layered Linux MBLINK reference face for Night — including titlebar, connection, heading, summary, detail, note, state-border and hover-accent roles — while Day supplies the matching white semantic palette. Defragmenter maps those neutral roles into its GTK selectors without redefining a private palette. The selected mode is persisted per user and synchronised across open windows.

Typography is deliberately closed to the three packaged MB Corpo faces: MB Corpo A Condensed for primary titles, MB Corpo S Regular for normal interface text and MB Corpo S Bold for emphasis. Defragmenter and Test Media do not request generic system or monospace fallback families; the Debian and local installers ship and register the same verified font bundle used by MBLINK.

The desktop, window and About surfaces use one approved 96×96 Defragmenter artwork asset in the same non-automotive Infiltrator desktop family as System Monitor: a dark graphite tile, #00ADEF cyan linework and a simple defragmentation glyph showing scattered blocks compacting into an ordered group. Packaging installs the same bytes into the desktop icon theme, Mint app-install icon path and the application's private runtime path; the GTK application resolves those installed copies deterministically before consulting the icon theme, so a missing cache entry cannot silently turn the About/window artwork into a placeholder. The process also publishes the same `io.github.linuxdefragger` identity as its GTK application ID, X11 program class and desktop `StartupWMClass`, so Cinnamon can associate the running Python-hosted window with the packaged desktop icon instead of falling back to a generic taskbar glyph.

## Capabilities

The project contains native analysis support across FAT12/16/32, exFAT, NTFS, ext2/3/4, XFS v5, Amiga OFS/FFS/SFS/PFS3 variants, HFS/HFS+, Btrfs, APFS, Minix, UFS, ZFS/OpenZFS members and Linux swap, with write support implemented only where the filesystem-specific engine has an explicit contract.

Production operations are:

- **Analyse / Map** — read-only allocation and fragmentation analysis.
- **Defragment** — canonical placement of supported movable allocations.
- **Growth Defrag** — canonical placement with an exact 10% post-file reserve.
- **Recover** — resume/repair of supported interrupted persistent transactions.

Unsupported layouts fail closed rather than being guessed. Write-capable engines perform verified staging/recovery and a final read-only rescan before reporting success.

## Architecture

The canonical implementation lives under `defragger/`.

Filesystem implementations are organised below `defragger/gui/filesystems/<format>/native/`. Filesystem-neutral device safety, raw I/O and Stop handling live under `defragger/src/core/`. The C++17 application-service layer in `defragger/native/` is the sole registry, allocation-map translation, operation-dispatch and privileged-helper control plane; GTK remains Python presentation/glue and consumes the native manifest directly.

The operating system supplies raw block I/O, but filesystem parsing, placement planning, staging and metadata updates are owned by the project. Architecture and regression tests reject known external filesystem mutation/repair orchestration and duplicate implementation paths.

Shared first-party primitives are consumed from the exact pinned Common 1.19.24 dependency; filesystem-specific rules remain in Defragmenter. Common supplies the generic numeric, arithmetic, byte-order, exact-I/O, durable-file, design/typography and font-provenance contracts used here, while Defragmenter keeps filesystem interpretation, target safety, placement and recovery semantics local. The forensic Common pass also removed an unused Python transaction/journal implementation and unused Python raw-write path so mutation has one authoritative native durability/I/O stack instead of a second compatibility implementation.

## Build and test

From the canonical project directory:

```bash
cd defragger
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLD_ENABLE_WERROR=ON
cmake --build build -j"$(nproc)"
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

The `.deb` keeps the human-facing `Defragmenter-...` asset filename while its Debian/APT package identity is `infiltrator-defragmenter`; existing `linux-defragger` installations migrate through the repository transition package.

GitHub automatically provides its standard `Source code (zip)` and `Source code (tar.gz)` links for every release tag. Defragmenter does not upload a duplicate custom source archive.

## Repository and release policy

This repository uses `main` as its working branch. Development changes are made directly on `main`; the normal project workflow does not depend on PR, feature or release branches.

Every push to `main` runs the project quality gate. Ordinary commits do not publish. A commit becomes release-eligible only when its subject begins `Release <version>` and the complete quality gate succeeds.

The release workflow requires a successful Project quality gate for the exact current `main` commit, verifies permanent history protection against deletion, force pushes and nonlinear history, and creates a new immutable version tag and release only from that tested commit.

Existing version tags and published releases are immutable and are never moved, replaced or edited in place. Manually runnable quality-gate helpers are diagnostic tools only and are not release-approval mechanisms.

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
