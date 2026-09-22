# Defragmenter source tree

This directory contains the application implementation and build root. Product overview, engineering ethos, release policy and current safety status are maintained in the [repository README](../README.md) and [Audit Status](../docs/AUDIT_STATUS.md) rather than duplicated here.

The current software version is defined by [VERSION](VERSION).

## Compatibility naming

The user-facing product is **Defragmenter**. The Debian/APT package identity is `infiltrator-defragmenter`. Established executable, desktop application ID, runtime/configuration paths and recovery/journal identities retain their `linux-defragger` compatibility names so upgrades and persisted state continue to work.

## Filesystem support

| Filesystem | Analyse / Map | Defragment | Growth Defrag | Recover |
|---|---|---|---|---|
| FAT12 / FAT16 / FAT32 | Exact | Native C | Native C, exact 10% reserve | Yes |
| exFAT | Exact | Native C | Native C, exact 10% reserve | Yes |
| NTFS | Exact | Native C, fail-closed preflight | Native C, exact 10% reserve | Yes |
| ext2 / ext3 / ext4 | Exact | Native C staged writer | Native C, exact 10% reserve | Yes |
| XFS v5 | Exact | Native C raw userspace writer | Native C, exact 10% reserve | Yes |
| Amiga OFS / FFS | Exact | Native C | Native C, exact 10% reserve | Yes |
| Amiga SFS0 | Exact allocation + file-extent analysis | Native C supported-subset relayout | Exact 10% reserve | Yes |
| Amiga SFS2 | Exact allocation + 48-bit file/32-bit extent analysis | Native C supported-subset relayout | Native C, exact 10% reserve | Yes |
| Amiga PFS3 | Exact allocation + anode-chain analysis for qualified subset | Native C bounded supported-subset relayout | Native C, exact 10% reserve | Yes |
| HFS+ / HFSX | Exact | Native C, fail-closed preflight | Native C, exact 10% reserve | Yes |
| Classic Macintosh HFS | Exact, native C | Native C bounded supported-subset relayout | Native C, exact 10% reserve | Yes |
| Btrfs | Exact raw single-device analysis | Native C bounded supported-subset relayout | Native C, exact 10% reserve | Yes |
| APFS | Exact native C analysis for bounded checkpoint/spaceman subset | Native C bounded supported-subset relayout | Native C, exact 10% reserve | Yes |
| Minix v1 / v2 / v3 | Exact, native C | Native C, fail-closed staged relayout | Native C, exact 10% reserve | Yes |
| UFS1 | Exact allocation + inode-tree fragmentation analysis | Native C bounded supported-subset relayout | Native C, exact 10% reserve | Yes |
| UFS2 | Exact allocation + inode-tree fragmentation analysis | Native C bounded supported-subset relayout | Native C, exact 10% reserve | Yes |
| ZFS / OpenZFS member | Summary read-only analysis, native C | Not implemented | Not implemented | No |
| Linux swap | Exact inactive / aggregate active analysis | Not applicable | Not applicable | No |

Unsupported or structurally ambiguous layouts fail closed.

## Source layout

- `gui/ui/` — GTK presentation, coordinators and user interaction.
- `gui/core/` — Python presentation-side protocol, device-discovery and path contracts.
- `gui/filesystems/<format>/native/` — authoritative per-filesystem native implementations.
- `native/` — authoritative C++17 registry, mapper, operation dispatcher and privileged session.
- `src/core/` — filesystem-neutral native safety/runtime services.
- `test_media/` — separate destructive sacrificial-media utility.
- `tests/` — native, filesystem, GUI, safety and release regressions.
- `packaging/` — Debian and native local installer construction.
- `shared/infiltratr-common/` — exact pinned Common dependency.

Detailed ownership and transaction rules are in [Architecture](../docs/ARCHITECTURE.md).

## Production operations

**Defragment** places supported movable allocations into the earliest legal canonical layout. **Growth Defrag** applies the same placement model while reserving exactly 10% of each regular file's allocated length immediately after that file. **Recover** resumes or completes an interrupted supported transaction when its recovery contract permits it.

Stop is cooperative and takes effect only at a filesystem-safe boundary.

## Test Media

The package includes **Defragmenter Test Media**, a separate all-C GTK utility for manufacturing sacrificial test filesystems. It repeats destructive-target checks after privilege elevation and must never be pointed at a system disk or irreplaceable media.

Formatting utilities used by Test Media are fixture-generation tools only; they are not part of production defragmentation. OFS/FFS, SFS, bounded PFS3 and bounded APFS qualification media are manufactured by first-party raw C creators.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLD_ENABLE_WERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The aggregate suite covers native/filesystem behaviour, GUI/service contracts, safety/transaction invariants, packaging and release gates. Hosted CI also runs ASan/UBSan qualification.

## Packaging

A numbered release publishes the generic amd64 Debian artifact, the hardware-native local compile/install `.run` artifact and `RELEASE_SHA256SUMS.txt`. GitHub supplies its standard tag source archives automatically.

The package/install contract is described in the repository [README](../README.md); exact release qualification is recorded in [Audit Status](../docs/AUDIT_STATUS.md).

## Documentation map

- [Documentation index](../docs/README.md)
- [Architecture](../docs/ARCHITECTURE.md)
- [Design](../docs/DESIGN.md)
- [Decisions](../docs/DECISIONS.md)
- [Validation](../docs/VALIDATION.md)
- [Audit status](../docs/AUDIT_STATUS.md)
- [References](../docs/REFERENCES.md)
