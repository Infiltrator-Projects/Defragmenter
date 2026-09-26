# Safety audit status

Status: **complete**

Completed: 2026-09-22
Extended: 2026-09-26

Applies to: release version 1.8.0-210
Audited source commit: c047cfa152b238c1b5684cd15fffaadef93e77a8
Audited release-governance commit: 1a61ff57bdf939fcbfcc84300b66b30411276113

Audited writer IDs: fat12, fat16, fat32, exfat, ntfs, ext4, xfs, affs, apfs, btrfs, pfs3, sfs, hfs, hfsplus, minix, ufs

This document records the current release safety case. Historical audit-development detail remains in Git history and immutable release tags rather than being repeated as a second changelog.

## Qualification evidence

The current audited production-source baseline is commit `c047cfa152b238c1b5684cd15fffaadef93e77a8`. In Project quality gate run 36216799567, that exact candidate completed the warnings-as-errors C/C++ build, the complete native/filesystem/GUI/release CTest suite and the independent hosted ASan/UBSan lane successfully. Self-hosted Local Linux / heavy qualification run 36216799410 likewise completed its dependency check, strict build and native/filesystem/GUI test subsets successfully. Both candidate runs stopped only at the deliberate stale audited-source control, which is advanced by version 1.8.0-210.

The reviewed Test Media delta strengthens the destructive qualification contract. Preparation now rejects system-backed and unsuitable targets more aggressively, binds confirmation and persisted state to a stable disk fingerprint instead of a reusable `/dev` pathname, stores root-owned state beneath `/var/lib`, waits for the expected repartitioned device topology instead of relying on a fixed sleep, verifies retained directory-test payload bytes rather than entry count alone, and adds deterministic boundary-sized files around common 512-byte and 4096-byte allocation boundaries. The malformed-media matrix is broadened across the native filesystem identifiers. These changes do not relax any production writer subset or recovery invariant.

The reviewed performance delta preserves the authoritative allocation map's exact row-major physical ordering while reducing unnecessary work. Resize-triggered remaps now use hysteresis instead of rescanning for small geometry changes, map rasterisation paints physical spans rather than performing the former full per-pixel source-cell search, and the shared parallel worker default leaves one online CPU available to the desktop while still applying media-specific I/O limits. APFS Test Media discovery also avoids a redundant AFFS probe. Filesystem interpretation, placement, mutation and final-verification semantics are unchanged.

The release-governance baseline is extended through `1a61ff57bdf939fcbfcc84300b66b30411276113`, which retains the pull-only APT ownership boundary and adds the intended self-hosted qualification run on main pushes. Defragmenter's release path validates its exact immutable GitHub release identity but never dispatches, authenticates to, waits on or synchronously verifies Infiltrator-Repository. The central repository independently discovers released packages on its own schedule, and the release-gate regression now rejects any reintroduction of cross-repository dispatch-token plumbing.

The malformed-media matrix, transaction/recovery tests, native integration fixtures, GUI contract tests, release/package tests and architecture ownership checks remain part of the permanent **Project quality gate**. Environment-dependent destructive media evidence is supplementary and is not represented as hosted-CI proof.

## Current safety case

Write-capable engines are admitted only when the parser, placement model, durable recovery contract and independent final verification agree on the same bounded on-disk subset. Unsupported, structurally ambiguous, mounted or identity-mismatched targets fail closed before authoritative writes.

- **FAT12/FAT16/FAT32** — native allocation/catalogue analysis, canonical relocation, exact 10% Growth Defrag reserve and Recover.
- **exFAT** — native catalogue/relayout, exact 10% Growth Defrag reserve and Recover.
- **NTFS** — native fail-closed preflight, bounded relocation, exact Growth Defrag reserve and Recover; persistent journal mechanics are separated from filesystem-specific placement/recovery semantics.
- **EXT2/EXT3/EXT4** — first-party native on-disk superblock/group/bitmap/inode validation, extent and legacy-indirect traversal, allocation mutation, metadata checksum maintenance, staged commit and Recover. Production mutation no longer links libext2fs; e2fsprogs/libext2fs remains test-only independent fixture/oracle evidence.
- **XFS** — native raw userspace catalogue, planning, allocation-metadata reconstruction, verification and Recover for the qualified v5 contract.
- **Btrfs** — exact first-party analysis plus bounded offline Defragment, exact 10% Growth Defrag and Recover for the single-device CRC32C, level-0 mixed-group subset; unsupported profiles, sharing, encoding, snapshots/qgroups and active transaction state fail closed.
- **Amiga OFS/FFS** — native raw catalogue, relocation, verification and Recover for the qualified classic layout; allocation is trusted only with a valid root bitmap-valid word, and DOS\\6/DOS\\7 long-name layouts fail closed.
- **Amiga SFS0/SFS2** — native supported-subset relayout and Recover with format-specific metadata checksums, 107-character namespace validation, SFS2 48-bit file-size encoding and 32-bit extent geometry.
- **Amiga PFS3** — bounded small-disk native allocation/anode analysis, Defragment, exact 10% Growth Defrag and Recover with validated root-extension roving/delete-directory/filename geometry and directory-entry extension bounds.
- **Classic Macintosh HFS** — exact allocation/catalog analysis plus bounded offline Defragment, exact 10% Growth Defrag and Recover for clean volumes whose regular-file fork maps are completely understood.
- **HFS+/HFSX** — native staged transaction and Recover for the qualified clean/journal state.
- **Minix v1/v2/v3** — exact native analysis plus recoverable Defragment, exact 10% Growth Defrag and Recover.
- **APFS** — exact checkpoint/spaceman/catalog analysis plus bounded offline Defragment, exact 10% Growth Defrag and Recover for a **single active two-object checkpoint**, direct-CIB, unencrypted/unsealed, snapshot-free, flat-tree subset; shared/cloned/sparse/unsupported state fails closed.
- **UFS1/UFS2** — exact cylinder-group allocation and inode block-tree fragmentation analysis plus bounded clean, non-journalled, snapshot-free Defragment, exact 10% Growth Defrag and durable Recover.

ZFS/OpenZFS and Linux swap are complete by design as analysis-only. ZFS exactness is bounded to the qualified single-disk topology and rejects unsupported block forms, read-critical MOS features, checksums/compression and active log-space-map state rather than approximating them. Raw ZFS mutation is outside product scope because Defragmenter does not replace the filesystem's CoW/TXG transaction engine or claim an exact persistent post-file reserve that ZFS itself may relocate.

## Safety invariants

The audited writers preserve these release invariants:

1. target identity and physical capacity are captured and revalidated before authoritative publication;
2. mounted or overlapping raw targets are refused;
3. mutation begins only from a fully validated catalogue/plan within the writer's declared subset;
4. recovery material is durably published before source bytes can require recovery;
5. transaction state cannot silently switch target, regress phase or reuse stale identity;
6. Stop is observed only at a valid clean or recoverable boundary;
7. success requires independent reopened verification of payload, metadata and requested layout;
8. unsupported or ambiguous filesystem state does not write;
9. GUI/helper control loss cannot detach an active privileged writer from supervision.

## Dependency baseline

The audited production tree consumes Infiltratr Common 1.19.27 at exact commit `3ef3710df6563df305b6d8e2dc9d1a41c61843ba`. CMake, the local installer, the Git submodule and release regressions assert the same released pin.

Common owns generic checked arithmetic, strict numeric/config parsing, endian access, bounded growth, path/string primitives, exact I/O, generic durable-file operations, design metrics/semantic roles, typography identity and canonical font provenance. Defragmenter retains filesystem geometry and interpretation, target-safety policy, placement, transaction/recovery semantics and allocation-map meaning. This boundary is enforced by architecture tests rather than documentation alone.

## Release controls and decision

The repository uses the `direct-main` development model. The active `protected-main` history contract, exact audited source/governance baselines, immutable release checks and hosted **Project quality gate** collectively guard publication.

Ordinary main commits never publish. An **explicit release decision** is represented by a commit whose subject begins `Release <version>`. The exact head must pass the hosted full-suite and ASan/UBSan lanes; release automation then verifies the version, audit baselines, source/workflow drift and immutable tag identity before creating the GitHub release.

APT refresh remains a direct release-workflow stage, but publication ownership is pull-based: Defragmenter validates and records its immutable released identity without cross-repository dispatch credentials, while Infiltrator-Repository independently discovers the release on its own schedule.
