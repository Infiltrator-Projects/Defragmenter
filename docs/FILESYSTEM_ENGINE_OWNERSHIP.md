# Filesystem engine ownership

**Goal:** one authoritative `/System/Filesystems/<FORMAT>.so` per filesystem format, used by consumers rather than copied into Defragmenter. The cross-project [unified module architecture](https://github.com/Infiltrator-Projects/Filesystems/blob/main/docs/UNIFIED_FILESYSTEM_MODULE_ARCHITECTURE.md) sets the intended contract. BOOT uses a generic module bridge; the current Linux kernel still has separate native KOs, and a generic kernel SO bridge must **not** be claimed as implemented.

## Separation of responsibility

Defragmenter owns its GUI, native application registry, target safety, mounted-overlap refusal, privilege and process lifetime, host raw I/O, bounded module services, Stop/progress, generic Defrag/Growth placement policy, and independent post-write verification. **The filesystem module** owns its on-disk parser, allocation geometry, format-specific constraints, safe relocation/metadata translation, and filesystem-specific atomic commit/recovery where migrated. Common owns only generic cross-project primitives.

**Intended end state:** Defragmenter plans a generic sequence of movements; a filesystem SO translates that plan into *filesystem-valid* operations. The SO must not duplicate the ordinary packing and 10% Growth Defrag decision logic. It must, however, own the mechanism that makes a movement and its metadata references valid. A ZFS copy-on-write transaction is not equivalent to writing a byte range at another offset.

Required modules load only from trusted paths with checked ABI, filesystem identity, ownership and version. A failed, missing or incompatible module **must fail closed**, never select an old private implementation. A working screen or superficially correct allocation map is not evidence that module dispatch happened.

## Established migrations

- **FAT12/FAT16/FAT32:** the required standalone modules implement production defrag/recovery and the Test Media V4 formatter/V3 file interface. Test Media retains an independent **read-only** FAT payload checker, not a competing formatter. Its readiness and privileged preparation validate modules before destructive disk changes.
- **OFS:** OFS.so supplies its Defrag V2 and Test Media V3 contracts. The host supplies only bounded I/O, target checks, Stop and durable-file capabilities. Old AFFS journal readers may remain solely to recover already-issued transactions, not for a new-operation fallback.
- **FFS:** FFS.so owns production defragmentation and Test Media fixture creation/verification. Old native FFS source can be used as an independent test oracle when built from the Filesystem-FFS repository, not linked into production.
- **exFAT:** the independent exFAT.so owns its defragmentation engine and the separate V5 Test Media file/format interface. The installed Defragmenter worker is a checked module host.
- **UFS1/UFS2:** separate filesystem distributions and checked read-only probes prevent a guess from a damaged superblock. UFS2 Test Media creation and payload inspection delegate to UFS2.so; UFS1 fixture creation requires its own qualification.
- **HFS+/HFSX:** native defrag/recovery and Test Media creation/verification have moved into HFSPLUS.so through versioned interfaces.
- **EXT2/EXT3/EXT4:** distinct module identities and ABI checks are enforced, including isolated EXT metadata-worker tests. Integration coverage must not accidentally select host-installed production modules.

Other format support remains subject to its current native worker/module, its own qualified source and the current tests. The existence of an independent integration workflow is not itself proof that every component or kernel consumer migrated.

## Generic planner transition (November 2026)

The core contains two **read-only, two-pass planners**. The function
`ld_generic_layout_plan` packs a provider-certified contiguous region;
`ld_generic_layout_plan_file_extents` plans one file across certified,
sorted disjoint free runs, preferring a single extent. The Growth reserve
must follow the final extent within its certified free range. Both enforce
alignment, refuse pinned/shared objects and malformed inventories, and
avoid publishing partial output after capacity or overflow failure.
The 10% reserve is rounded UP in whole provider allocation units:
`ceil(allocated_units / 10)`. Additional alignment padding is distinct.
These functions and tests live in Defragmenter, not filesystem repositories.

This is **not yet the universal production write path**. The independent OFS
pilot has exercised the shared host planner on certified free runs, but
existing SO-owned relocation/transaction engines remain while the common
inventory and transactional execution ABI is migrated. In the desired interface, modules expose a verified object/
extent inventory and constraints, validate the generic plan against their
actual geometry, execute it in filesystem-valid transaction boundaries,
recover interruptions and allow independent verification. The host must not
assume any raw block copy can safely implement a move. For ZFS, the module
does not yet expose a safe relocation/transaction translator, so neither
Defrag nor Growth Defrag can be reported as qualified.

## Universal discovery transition (November 2026)

The native operation engine now has an additive **generic Defrag V2 path**:
`--list-installed-modules` enumerates root-owned, non-writable-by-others
`/System/Filesystems/*.so` objects with the common
`infiltrator_filesystem_defrag_v2` interface. It obtains `format_id` and
operation capability bits **from each loaded module**, not a filesystem table
or guessed filename. For an unknown filesystem ID, the engine resolves the
matching installed SO and dispatches supported offline Defrag/Growth/Recover
through the same trusted host, after its ordinary target safety checks.
Conflicting identities, absent modules, mismatched ABIs and missing required
operation capabilities fail closed. Defrag and Growth also require recovery
capability.

This is **not yet universal GUI integration**. The existing desktop backend
registry, legacy format worker adapters, and the fixed 21-format Test Media
qualification layout remain for compatibility until their ABI consumers can
be converted and retested. In particular, the GUI does not yet automatically
offer every discovered V2 module. Do not retire any working legacy adapter
until an end-to-end module-only replacement is qualified.

Test Media's unmount loop now uses the native `umount2` kernel interface in
place of an external `umount` executable. Other Linux-facing external
utilities (`lsblk`, `partx`, `sfdisk`, `wipefs`, and optional
`udevadm`) and external application libraries remain; they must not be
described as removed. Low-level kernel interfaces remain necessary on the
Linux-hosted transitional InfiltratorOS, but file-system format semantics
must stay in the independently installed modules.

## Required proof

For each migrated format, test the real `dlopen`/ABI path and exercise:

1. Correct-module dispatch, exact format identity, version/ABI rejection and missing-module refusal.
2. No target write, including the signature-clearing region, on missing/incompatible module or failed preflight.
3. Independent before/after allocation, file-content, directory and geometry verification on deliberately fragmented media.
4. Both supported operations, exact Growth reserve, Stop, interrupted recovery and corruption rejection where the format is write-capable.
5. Format-specific unknown-feature/unsupported-layout refusal and a trusted source/dependency inventory.

The 21-slot Test Media corpus is application-owned as deterministic **test data**, but production format mutation belongs to the authoritative module wherever integrated. Independent verification is retained deliberately as a separate oracle. Tests may compile external original source as a hermetic fixture, never ship it as a parallel production engine.

## Installation distinction

`/System/Filesystems/*.so` holds independently installed filesystem engines. `/System/Commands/Defragmenter/` holds Defragmenter's **private process executables** (operation engine, mapper, format hosts and privileged helper). They have different owners and are not interchangeable. Defragmenter packaging and InfiltratorOS verification both use these paths.

See [Architecture](ARCHITECTURE.md), [Validation](VALIDATION.md) and the [audited release baseline](AUDIT_STATUS.md).
