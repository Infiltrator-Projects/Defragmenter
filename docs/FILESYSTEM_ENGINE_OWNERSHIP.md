# Filesystem engine ownership

**Goal:** one authoritative `/System/Filesystems/<FORMAT>.so` per filesystem format, used by consumers rather than copied into Defragmenter. The cross-project [unified module architecture](https://github.com/Infiltrator-Projects/Filesystems/blob/main/docs/UNIFIED_FILESYSTEM_MODULE_ARCHITECTURE.md) sets the intended contract. BOOT uses a generic module bridge; the current Linux kernel still has separate native KOs, and a generic kernel SO bridge must **not** be claimed as implemented.

## Separation of responsibility

Defragmenter owns its GUI, native application registry, target safety, mounted-overlap refusal, privilege and process lifetime, host raw I/O, bounded module services, Stop/progress and independent post-write verification. **The filesystem module** owns its on-disk parser, allocation geometry, format-specific planner/relocation, metadata changes, journalling and recovery where migrated. Common owns only generic cross-project primitives.

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
