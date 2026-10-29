# Defragmenter source tree

This directory contains the application implementation and build root. Product overview, engineering ethos, release policy and current safety status are maintained in the [repository README](../README.md) and [Audit Status](../docs/AUDIT_STATUS.md) rather than duplicated here.

The current software version is defined by [VERSION](VERSION).

## Compatibility naming

The user-facing product is **Defragmenter**. The Debian/APT package identity is `defragmenter`. The Debian package and desktop identity remain `defragmenter`. The actual native GTK application is `/Programs/defragmenter` and the separate Test Media utility is `/Programs/defragmenter-test-media` (only two files, no wrapper), following InfiltratorOS's single native user-launchable application namespace. Private application helpers and filesystem-specific Defragmenter workers reside under `/System/Commands/Defragmenter`, reserving `/System/Filesystems` for OS filesystem implementations, the font-bootstrap helper under `/System/Commands`, approved artwork under `/System/Resources/Icons` and `/System/Resources/Themes/Defragmenter`, and documentation under `/System/Resources/Documentation/Defragmenter`. Host desktop-registration metadata remains under `/usr/share` for GTK/Mint integration; durable journal identities remain unchanged.

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
| Btrfs | Exact raw single-device CRC32C analysis | Native C bounded supported-subset relayout | Native C, exact 10% reserve | Yes |
| APFS | Exact native C analysis for bounded checkpoint/spaceman subset | Native C bounded supported-subset relayout | Native C, exact 10% reserve | Yes |
| Minix v1 / v2 / v3 | Exact, native C | Native C, fail-closed staged relayout | Native C, exact 10% reserve | Yes |
| UFS1 | Exact allocation + inode-tree fragmentation analysis | Native C bounded supported-subset relayout | Native C, exact 10% reserve | Yes |
| UFS2 | Exact allocation + inode-tree fragmentation analysis | Native C bounded supported-subset relayout | Native C, exact 10% reserve | Yes |
| ZFS / OpenZFS member | Bounded exact allocation + file-fragmentation analysis, native C | Not applicable by design | Not applicable by design | No |
| Linux swap | Exact inactive / aggregate active analysis | Not applicable | Not applicable | No |

Unsupported or structurally ambiguous layouts fail closed.
FAT operations require the matching current `FAT12.so`, `FAT16.so` or
`FAT32.so` installed by Filesystems in `/System/Filesystems`. The FAT worker is
a child-process module host; the filesystem and journal engine lives in the
selected module. If that module is absent or older than the defrag ABI, the
operation fails with an install/update message.

OFS likewise requires current `OFS.so`, with Defrag V2 and V3. The Amiga worker
selects that module for DOS0/2/4/6; it handles all seven block sizes, analysis,
metadata/data relocation, exact Growth reserve and staged recovery. OFS Test
Media formatting and population also use that SO. FFS and already-issued legacy
AFFS journals retain their separate compatibility implementation. Boot uses its
generic SO interface; Linux still uses native KOs until the later kernel bridge.

## Source layout

- `gui/ui/art/` — native desktop artwork only; there is no Python application/presentation implementation.
- `gui/filesystems/<format>/native/` — native implementations retained by Defragmenter; FAT is supplied by Filesystems.
- `native/` — authoritative C++17 GTK desktop, registry, mapper, operation dispatcher and privileged session.
- `src/core/` — filesystem-neutral native safety/runtime services.
- `test_media/` — separate destructive sacrificial-media utility.
- `tests/` — native regressions plus independent Python fixture/test drivers that exercise native binaries; Python is not a product control plane.
- `packaging/` — Debian and native local installer construction.
- `shared/infiltratr-common/` — exact pinned Common dependency.

Detailed ownership and transaction rules are in [Architecture](../docs/ARCHITECTURE.md).

## Production operations

**Defragment** places supported movable allocations into the earliest legal canonical layout. **Growth Defrag** applies the same placement model while reserving exactly 10% of each regular file's allocated length immediately after that file. **Recover** resumes or completes an interrupted supported transaction when its recovery contract permits it.

Stop is cooperative and takes effect only at a filesystem-safe boundary.

## Test Media

The package includes **Defragmenter Test Media**, a separate all-C GTK utility for manufacturing sacrificial test filesystems. It repeats destructive-target checks after privilege elevation and must never be pointed at a system disk or irreplaceable media.

All 21 Test Media slots use first-party C creators and raw payload verification. Target construction and verification require no host filesystem formatters, mounted target filesystems or external filesystem drivers. The native-fixture regression runs both relocation modes and byte verification for the additional write-capable fixtures. Protected schema 3 state records disk identity and build results; expected payloads come from compiled corpus policy. Rebuild schema 2 test disks before qualification or verification. These bounded fixtures exercise the qualified native subsets and are not general-purpose filesystem formatters.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLD_ENABLE_WERROR=ON
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')
[ "$jobs" -gt 1 ] && jobs=$((jobs - 1))
cmake --build build -j"$jobs"
ctest --test-dir build --output-on-failure
```

The aggregate suite covers native/filesystem behaviour, native GUI/service contracts, safety/transaction invariants, packaging and release gates. Python is used only where an independent fixture generator or black-box test driver gives stronger evidence than testing native implementation details through itself. Hosted CI also runs ASan/UBSan qualification.

For FAT-specific source unit tests, set `FAT_DEFRAG_SOURCE_DIR` to the
matching Filesystems `native/filesystems/fat/defrag` directory at configure
time. Normal runtime and integration tests also require the installed FAT
modules. CI builds the pinned Filesystems revision and uses a separate test-only
loader and disposable module directory instead of changing `/System`.

## Packaging

A numbered release publishes the generic amd64 Debian artifact, the hardware-native local compile/install `.run` artifact and `RELEASE_SHA256SUMS.txt`. GitHub supplies its standard tag source archives automatically.

The local `.run` source payload is materialised from committed Git objects for the exact superproject revision plus the exact pinned Common commit. Ambient untracked runner files are not payload inputs, and qualification verifies that property before publication.

The package/install contract is described in the repository [README](../README.md); exact release qualification is recorded in [Audit Status](../docs/AUDIT_STATUS.md).

## Documentation map

- [Documentation index](../docs/README.md)
- [Architecture](../docs/ARCHITECTURE.md)
- [Design](../docs/DESIGN.md)
- [Decisions](../docs/DECISIONS.md)
- [Validation](../docs/VALIDATION.md)
- [Audit status](../docs/AUDIT_STATUS.md)
- [References](../docs/REFERENCES.md)

## Installed program and icon layout

Exactly two user-facing executables are installed: `/Programs/defragmenter`
(the native GTK binary itself) and `/Programs/defragmenter-test-media`.
There is no `/Programs/defragmenter-desktop` launcher copy or shell wrapper.
Private Defragmenter command executables and filesystem-specific worker programs
reside in `/System/Commands/Defragmenter`, **not** in the operating system's
`/System/Filesystems` driver namespace. The two Cinnamon menu desktop
entries use the approved native `/System/Resources/Icons/defragmenter.png`.
Nemo's icon for the raw executable file is controlled by per-user file metadata,
and is not changed merely by a `.desktop` Icon line. No second icon artwork
has been invented for Test Media.
