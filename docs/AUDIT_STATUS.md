# Safety audit status

Status: **complete**

Completed: 2026-09-22
Extended: 2026-09-29

Applies to: release version 1.8.0-224
Audited source commit: f321185a06adefb19277b8929b0d640d678838cf
Audited release-governance commit: e09599f6c84b31e7f29ec5da9e5f9db23807b9d9

This status records the 1.8.0-224 release audit. A newer `main` commit is not implicitly covered merely because the release version string has not changed; release eligibility still requires the exact-head quality gate and the source/governance drift checks described below.

Audited writer IDs: fat12, fat16, fat32, exfat, ntfs, ext4, xfs, affs, apfs, btrfs, pfs3, sfs, hfs, hfsplus, minix, ufs

This document records the current release safety case. Historical audit-development detail remains in Git history and immutable release tags rather than being repeated as a second changelog.

## Qualification evidence

The current audited production-source baseline is commit `f321185a06adefb19277b8929b0d640d678838cf` for release version 1.8.0-224. The delta from the qualified 1.8.0-223 storage baseline is confined to the native C++ GTK presentation shell: dashboard geometry, bounded standalone raster artwork, summary/action sizing, work-area fit, activity/footer surfaces and operation-page composition. Filesystem parsing, planning, mutation, recovery and privileged-operation semantics are unchanged. Publication remains conditioned on the exact-head hosted warnings-as-errors build, shipped native GTK Xvfb smoke test, complete CTest suite and ASan/UBSan lane; the release workflow will not publish this audit identity unless those gates pass.

The 1.8.0-224 capability baseline retains the fully qualified 1.8.0-223 filesystem, transaction and Day/Night semantics while restoring the last-good dashboard composition in the installed native C++ GTK client. The navigation rail remains Overview, Analyse, Defragment, Growth Defrag, Recover, Test Media and Settings; the selected-volume hero again uses its dedicated SSD raster, the Workbench-inspired sidebar art is bounded and non-expanding, summary gauges return to their established scale, the dense physical-position pixel map is elastic on short work areas, and the activity/footer hierarchy is restored. Day mode remains neutral rather than returning to pastel operation slabs. The correction does not reintroduce the retired Python runtime and does not change filesystem parsing, placement, mutation or recovery semantics. Each write capability remains behind its filesystem-specific preflight, durable transaction/recovery contract and final verification.

The release-governance baseline is `a3c3bf4fed0007178886d411e4172ce2b5302643`. The malformed-media matrix, transaction/recovery tests, native integration fixtures, GUI contract tests, release/package tests and architecture ownership checks remain part of the permanent **Project quality gate**. Environment-dependent destructive-media evidence remains supplementary and is not represented as hosted-CI proof.

## Current safety case

Write-capable engines are admitted only when the parser, placement model, durable recovery contract and independent final verification agree on the same bounded on-disk subset. Unsupported, structurally ambiguous, mounted or identity-mismatched targets fail closed before authoritative writes.

- **FAT12/FAT16/FAT32** — native allocation/catalogue analysis, canonical relocation, exact 10% Growth Defrag reserve and Recover.
- **exFAT** — native catalogue/relayout, exact 10% Growth Defrag reserve and Recover.
- **NTFS** — native fail-closed preflight, run-oriented relocation, ordinary alternate-data-stream relocation, exact Growth Defrag reserve and Recover; compressed/encrypted/sparse stream forms remain fail-closed when their complete on-disk semantics are outside the qualified writer.
- **EXT2/EXT3/EXT4** — first-party native on-disk superblock/group/bitmap/inode validation, extent and legacy-indirect traversal, allocation mutation, metadata checksum maintenance, staged commit and Recover. Production mutation no longer links libext2fs; e2fsprogs/libext2fs remains test-only independent fixture/oracle evidence.
- **XFS** — native raw userspace catalogue, range-oriented planning, allocation-metadata reconstruction, verification and Recover for the qualified v5 contract; realtime-device and shared reflink data remain fail-closed.
- **Btrfs** — exact first-party analysis plus bounded offline Defragment, exact 10% Growth Defrag and Recover for the single-device CRC32C, level-0 mixed-group subset, including ordinary checksum-tree-protected regular-file data with checksum verification/remapping; unsupported profiles, sharing, encoding, snapshots/qgroups and active transaction state fail closed.
- **Amiga OFS/FFS** — native raw catalogue, relocation, verification and Recover for the qualified classic layout; allocation is trusted only with a valid root bitmap-valid word, and DOS\\6/DOS\\7 long-name layouts fail closed.
- **Amiga SFS0/SFS2** — native supported-subset relayout and Recover with format-specific metadata checksums, 107-character namespace validation, SFS2 48-bit file-size encoding and 32-bit extent geometry.
- **Amiga PFS3** — bounded small-disk native allocation/anode analysis, recursive user-directory traversal, validated hard-link preservation, soft-link payload relocation and rollover-file anode-chain relocation, Defragment, exact 10% Growth Defrag and Recover; SUPERINDEX/LARGEFILE and remaining special entries stay fail-closed.
- **Classic Macintosh HFS** — exact allocation/catalog analysis plus bounded offline Defragment, exact 10% Growth Defrag and Recover for clean volumes, including regular-file forks whose complete extent maps continue through the Extents Overflow B-tree.
- **HFS+/HFSX** — native staged transaction and Recover for the qualified clean/journal state.
- **Minix v1/v2/v3** — exact native analysis plus recoverable Defragment, exact 10% Growth Defrag and Recover.
- **APFS** — exact checkpoint/spaceman/catalog analysis plus bounded offline Defragment, exact 10% Growth Defrag and Recover for a **single active two-object checkpoint** with validated retained historical checkpoint objects, direct-CIB, unencrypted/unsealed, snapshot-free, flat-tree state; shared/cloned/sparse/unsupported state fails closed.
- **UFS1/UFS2** — exact cylinder-group allocation and inode block-tree fragmentation analysis plus bounded clean, non-journalled, snapshot-free Defragment, exact 10% Growth Defrag and durable Recover, including fragment-sized tails, cross-cylinder-group source layouts and sparse regular files.

ZFS/OpenZFS and Linux swap are complete by design as analysis-only. ZFS exactness is bounded to the qualified single-disk topology and now verifies OFF, Fletcher2, Fletcher4, SHA-256 and SHA-512/256 block checksums plus the qualified compression forms; unsupported block forms, read-critical MOS features, remaining checksum/compression algorithms and active log-space-map state are rejected rather than approximated. Raw ZFS mutation is outside product scope because Defragmenter does not replace the filesystem's CoW/TXG transaction engine or claim an exact persistent post-file reserve that ZFS itself may relocate.

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

The audited production tree consumes Infiltratr Common 1.19.35 at exact commit `7cc5de3de0e94ed2cfcff0840bbb5346eb5c9c9f`. CMake, the local installer, the Git submodule and release regressions assert the same released pin.

Common owns generic checked arithmetic, strict numeric/config parsing, endian access, bounded growth, path/string primitives, exact I/O, generic durable-file operations, design metrics/semantic roles, typography identity and canonical font provenance. Defragmenter retains filesystem geometry and interpretation, target-safety policy, placement, transaction/recovery semantics and allocation-map meaning. This boundary is enforced by architecture tests rather than documentation alone.

## Release controls and decision

The repository uses the `direct-main` development model. The active `protected-main` history contract, exact audited source/governance baselines, immutable release checks and hosted **Project quality gate** collectively guard publication.

Ordinary main commits never publish. An **explicit release decision** is represented by a commit whose subject begins `Release <version>`. The exact head must pass the hosted full-suite and ASan/UBSan lanes; release automation then verifies the version, audit baselines, source/workflow drift and immutable tag identity before creating the GitHub release.

APT refresh remains a direct release-workflow stage, but publication ownership is pull-based: Defragmenter validates and records its immutable released identity without cross-repository dispatch credentials, while Infiltrator-Repository independently discovers the release on its own schedule.
