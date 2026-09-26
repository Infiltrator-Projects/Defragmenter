# Safety audit status

Status: **complete**

Completed: 2026-09-22
Extended: 2026-09-26

Applies to: release version 1.8.0-213
Audited source commit: e7a5e04662e986d43474db5bfb9be534c3229f93
Audited release-governance commit: 36be157f932006672f4dfae1c41b83317a914f46

Audited writer IDs: fat12, fat16, fat32, exfat, ntfs, ext4, xfs, affs, apfs, btrfs, pfs3, sfs, hfs, hfsplus, minix, ufs

This document records the current release safety case. Historical audit-development detail remains in Git history and immutable release tags rather than being repeated as a second changelog.

## Qualification evidence

The current audited production-source baseline is commit `e7a5e04662e986d43474db5bfb9be534c3229f93`. In Project quality gate run 36224060894, that exact candidate completed the warnings-as-errors C/C++ build and all 44 native/filesystem/GUI/release CTest tests successfully; the independent hosted ASan/UBSan lane also passed. The run then stopped only at the deliberate stale completed-audit control, which this 1.8.0-213 audit advance resolves. The self-hosted Local Linux run for this exact candidate was queued but had not executed when this audit was advanced, so no new self-hosted result is claimed as evidence here; the preceding released 1.8.0-212 baseline remains the latest completed self-hosted qualification.

The reviewed GUI lifecycle delta moves physical-volume discovery and first-party identification probing off the GTK main thread. Discovery returns an isolated snapshot, GTK state is mutated only by the newest generation on the main loop, and stale overlapping refreshes are discarded. The safe unmount-and-continue path now waits for asynchronous rediscovery before revalidating the selected device and resuming mutation.

The reviewed allocation-map delta removes resize-triggered filesystem analysis. Window-size changes rerasterise the already-authoritative physical allocation sample only; explicit Analyse and post-mutation refresh remain the operations that reread filesystem state. This preserves monotonic physical map semantics while preventing UI geometry from causing disk I/O or parser work.

The reviewed identity-control delta aligns presentation with the existing fail-closed mutation planner. Defragment, Growth Defrag and Recover controls now remain disabled until first-party native Analyse has positively verified the physical filesystem identity. The planner independently retains the same identity check, so the change removes a misleading enabled-button state without weakening any writer boundary.

The release-governance baseline remains `36be157f932006672f4dfae1c41b83317a914f46`; workflow semantics are unchanged. The malformed-media matrix, transaction/recovery tests, native integration fixtures, GUI contract tests, release/package tests and architecture ownership checks remain part of the permanent **Project quality gate**. Environment-dependent destructive media evidence is supplementary and is not represented as hosted-CI proof.

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
