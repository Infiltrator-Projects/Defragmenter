<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Defragmenter

**Defragmenter** is a native C/C++ offline filesystem allocation analyser, defragmenter and recovery application for Linux. It analyses filesystem images and selected unmounted devices; write operations are available only for explicitly qualified filesystem subsets. It does not mount targets or invoke external filesystem defragmentation or repair tools.

**Copyright:** © 1993–2026 Shannon Smith · **Licence:** GPL-3.0-or-later · **Version:** [`defragger/VERSION`](defragger/VERSION)

[![Project quality gate](https://github.com/Infiltrator-Projects/Defragmenter/actions/workflows/quality-gate.yml/badge.svg)](https://github.com/Infiltrator-Projects/Defragmenter/actions/workflows/quality-gate.yml)

## Operations and limits

- **Analyse / Map:** inspect allocation and physical fragmentation without writing.
- **Defragment:** relocate supported movable allocations into a verified canonical layout.
- **Growth Defrag:** as above, with an exact 10% post-file free-space reserve, where admitted.
- **Recover:** validate and resume/restore a supported interrupted durable transaction.

The qualified support matrix, including format-specific exclusions, is in [the implementation README](defragger/README.md). ZFS/OpenZFS and Linux swap are **analysis-only by design**. Unsupported features, inconsistent geometry, mounted targets, mismatched identity or unverified recovery state are rejected rather than guessed.

A new write operation requires native analysis, target and mounted-overlap checks, a supported plan, durable recovery material and independent post-operation verification. Recover instead requires a valid existing journal; an interrupted filesystem may not be analysable. A desktop label or filesystem-discovery hint never authorises a write.

## Architecture

The intended authority is **one installed filesystem `.so` per on-disk format**, shared across consumers. Defragmenter owns the GUI, filesystem-neutral device safety, raw-I/O host services, privilege/Stop handling, scheduling and independent verification; the authoritative module owns its format's parser, allocation rules, relocation and recovery where integrated. No missing or incompatible required module may trigger a private filesystem fallback. Integration is format-by-format, not inferred from a working GUI.

- `defragger/native/`: C++17 GTK desktop, single registry, mapper, operation dispatcher and privileged-helper control plane.
- `defragger/src/core/`: C filesystem-neutral target, mounted-overlap, identity and raw-I/O safety.
- `defragger/gui/filesystems/`: native format workers and module hosts, including explicitly bounded transitional engines.
- `defragger/test_media/`: separate native C sacrificial-media construction and raw verification.
- `defragger/tests/`: native regressions and independent Python fixture/black-box drivers. Python is not installed application code.
- `defragger/packaging/`: Debian package and locally compiled `.run` release installers.

The shared [Libraries/Common dependency](https://github.com/Infiltrator-Projects/Libraries) supplies only format-neutral primitives and graphical design contracts; filesystem safety policy remains with its owner. See [Architecture](docs/ARCHITECTURE.md) and [filesystem ownership](docs/FILESYSTEM_ENGINE_OWNERSHIP.md).

## User interface

The native GTK 3 desktop provides Overview, Analyse, Defragment, Growth Defrag, Recover, Test Media and Settings. Day, Night and Follow system modes use the approved Common palette and three checksum-verified shared MB Corpo faces. The allocation map retains left-to-right, top-to-bottom physical order and never invents resolution to fill the canvas. The [UI contract and concept artwork](docs/UI_VISION.md) describe presentation details.

## Test Media

`/Programs/defragmenter-test-media` builds deliberately fragmented **sacrificial** media for qualification. Its 21 registered slots use first-party C fixture creation and raw verification rather than host formatters or mounted target filesystems. The deterministic file-bearing corpus includes eight large files totalling 200 MiB plus boundary-sized files. State schema 3 binds a disk identity and build results; it cannot substitute a saved expected-file list for actual byte verification. Rebuild older schema 2 test disks.

**Test Media is destructive. Never use it on a system disk or irreplaceable data.** Development and validation require verified backups and disposable targets.

## Build

From `defragger/`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLD_ENABLE_WERROR=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Some isolated module and physical-media qualifications require independently built filesystem `.so` fixtures or additional infrastructure; local CTest alone is not a release qualification. See [Validation](docs/VALIDATION.md).

## Installation and releases

| Published asset | Role |
| --- | --- |
| `Defragmenter-<version>-amd64.deb` | Generic x86-64 Debian package (`defragmenter`). |
| `Defragmenter-<version>-local-folder.run` | Source-based, CPU-native local build and installation. |
| `RELEASE_SHA256SUMS.txt` | Exact checksums for both installers. |

Exactly two user-facing executables are installed: `/Programs/defragmenter` and `/Programs/defragmenter-test-media`. Private application helpers belong under `/System/Commands/Defragmenter`, **not** `/System/Filesystems`, which is reserved for filesystem modules. The package owns one illustrated manual at `/System/Resources/Documentation/Defragmenter.pdf`, containing the full GPL licence. Icons and themes belong under `/System/Resources`; Linux desktop metadata stays under `/usr/share`. During an upgrade, the old `Documentation/Defragmenter/` directory is removed only if empty.

The `.run` embeds source from the qualified Git commit and the exact pinned Common commit, never ambient untracked workspace files. Every push is qualified, but **only a `defragger/VERSION` change expresses release intent**. Publishing requires the exact approved source and governance baselines, both quality lanes, once-built SHA-bound assets and a write-once tag/release identity. Ordinary documentation commits do not publish. See [Validation](docs/VALIDATION.md) and [Audit Status](docs/AUDIT_STATUS.md); a completed prior audit is **not** proof that later source changes are qualified.

## Documentation

Start with the [documentation index](docs/README.md) for the engineering philosophy, architecture, decisions, ownership, roadmap, validation, safety status, references and [condensed changelog](CHANGELOG.md).

Full historical release prose remains available from the [published v1.8.0-281 source tag](https://github.com/Infiltrator-Projects/Defragmenter/tree/v1.8.0-281), without being duplicated in the current documentation.
