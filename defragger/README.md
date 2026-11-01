# Defragmenter implementation

This is the CMake project root. [The product README](../README.md) covers operations, installers and release policy. `VERSION` is the sole source-version file; the current executable/support truth is in code and tests.

## Filesystem support

All write capability is limited to the parser's **explicitly qualified, clean and recoverable subset**. “Yes” is not blanket support for arbitrary filesystem features.

| Family | Analyse | Defragment / exact 10% Growth / Recover |
| --- | --- | --- |
| FAT12 / FAT16 / FAT32 | Exact supported volumes | Yes, matching authoritative `.so` required |
| exFAT | Exact supported volumes | Yes, verified exFAT.so |
| NTFS | Exact supported volumes | Yes, fail-closed preflight; unsupported streams refused |
| EXT2 / EXT3 / EXT4 | Exact supported volumes | Yes, qualified subset and independently checked modules |
| XFS v5 | Exact supported volumes | Yes, qualified raw-layout subset |
| Amiga OFS / FFS | Exact supported volumes | Yes, verified OFS.so / FFS.so |
| Amiga SFS0 / SFS2 | Exact supported volumes | Yes, qualified layout subsets |
| Amiga PFS3 | Exact supported volumes | Yes, bounded supported subset |
| Classic HFS / HFS+ / HFSX | Exact supported volumes | Yes, clean supported subsets |
| Btrfs | Exact bounded single-device subset | Yes, bounded unshared supported subset |
| APFS | Exact bounded checkpoint/spaceman subset | Yes, supported unencrypted/unshared subset |
| Minix v1 / v2 / v3 | Exact supported volumes | Yes, bounded staged writer |
| UFS1 / UFS2 | Exact bounded cylinder-group subset | Yes, clean, non-journalled, snapshot-free subset |
| ZFS / OpenZFS | Bounded exact analysis or safe summary | **No — analysis only** |
| Linux swap | Exact inactive / aggregate active analysis | **No — analysis only** |

A missing, untrusted or incompatible required `.so` rejects the operation. Module identity, format, ABI, mounted state and target identity are rechecked at the corresponding trust boundaries. Legacy format code is not a silent production fallback. See [module ownership](../docs/FILESYSTEM_ENGINE_OWNERSHIP.md).

## Source map

| Path | Responsibility |
| --- | --- |
| `native/` | C++17 GTK desktop, one registry, mapper, dispatcher and privileged session |
| `src/core/` | Filesystem-neutral device, identity, mounted-overlap and bounded I/O safety |
| `gui/filesystems/*/native/` | Format-specific native workers or trusted `.so` hosts |
| `test_media/` | 21-slot destructive fixture creator and independent raw verifier |
| `tests/` | Native regressions, fault injection, independent Python fixture/black-box tools |
| `cmake/` | Native build graph and integration |
| `packaging/` | Generic Debian and CPU-native source installers |
| `shared/infiltratr-common/` | Exact pinned first-party Common dependency |
| `ci/` | Qualification prerequisites and auditable release procedure |

The installed GUI and operation policies are native C/C++; the old Python application graph is not shipped. Stop is cooperative at safe transaction boundaries, and an interrupted operation retains durable recovery state where required.

## Build and qualification

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLD_ENABLE_WERROR=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

FAT source-unit tests can use `FAT_DEFRAG_SOURCE_DIR` pointing to the matching pinned Filesystems `native/filesystems/fat/defrag` source. Module integration uses isolated, independently built `.so` files and test-only loader paths; tests must never alter live `/System/Filesystems` to simulate success. The permanent workflow also executes ASan/UBSan and independent filesystem cases; [Validation](../docs/VALIDATION.md) distinguishes those results from physical-media evidence.

## Test Media and package layout

`defragmenter-test-media` manufactures deliberately fragmented sacrificial disks without external formatters or mounted target filesystems. The deterministic file-bearing corpus has 200 MiB of large files and eight boundary-sized files. Protected schema 3 state binds disk identity and per-slot results; expected bytes come from the compiled corpus. Schema 2 test disks must be rebuilt.

Only `/Programs/defragmenter` and `/Programs/defragmenter-test-media` are user-launchable. Internal native workers belong to `/System/Commands/Defragmenter`; independent filesystem `.so` drivers belong to `/System/Filesystems`. Product documentation/icons/themes belong to `/System/Resources`, desktop integration to `/usr/share`. The package builder rejects the old `/System/Filesystems/Defragmenter` private-worker layout.

For safety requirements and release-baseline status, read [Architecture](../docs/ARCHITECTURE.md), [Validation](../docs/VALIDATION.md) and [Audit Status](../docs/AUDIT_STATUS.md).
