# Safety audit status

Status: **complete**

Completed: 2026-09-22
Extended: 2026-10-03

Applies to: release version 1.8.0-240
Audited source commit: 54803a734c88501f9f1578b803e00a5f085da254
Audited release-governance commit: 54803a734c88501f9f1578b803e00a5f085da254

This status records the 1.8.0-240 release audit. A newer `main` commit is not implicitly covered merely because the release version string has not changed; release eligibility still requires the exact-head quality gate and the source/governance drift checks described below.

Audited writer IDs: fat12, fat16, fat32, exfat, ntfs, ext4, xfs, affs, apfs, btrfs, pfs3, sfs, hfs, hfsplus, minix, ufs

This document records the completed 1.8.0-240 release safety case and the explicit release decision after warnings-clean qualification. Re-running the release gate uses the audited source baseline named above. Historical audit-development detail remains in Git history and immutable release tags rather than being repeated as a second changelog.

## Qualification evidence

The current audited production/build/Test-Media source baseline is commit `54803a734c88501f9f1578b803e00a5f085da254` for release version 1.8.0-240. Write-capable filesystem engines are unchanged from the qualified 1.8.0-239 storage baseline. This pass removes the remaining UI-thread redraw waste after the allocation-map cache: static hero/drive/sidebar imagery is geometry-cached, Test Media bounds child-output work per GTK dispatch, and the 58 px header / 210 px sidebar-art holdovers are aligned to the 44 px titlebar and 195 px Infiltrator OS navigation rail. The production dependency remains current Infiltratr Common 1.19.38 at exact commit `7070c5812b50821fd7580101cb2289a3184f6b2c`; current Common main is the same commit.

The supplied 1.8.0-237 physical-media rerun proves that the previous UFS creator and main Test Media action-button corrections are active on the target system, while ZFS still stops during exact native qualification. The 1.8.0-237 reader already supports bounded fat/multi-block ZAP block structures, but its dnode admission check still required the historical `DMU_OT_OBJECT_DIRECTORY` value. Current OpenZFS can encode MOS ZAP metadata through the newer `DMU_OTN_ZAP_*` object-type form. The 1.8.0-238 reader recognises the ZAP byteswap class in that modern unencrypted encoding while continuing to reject encrypted or non-ZAP metadata, preserving the same bounded read-only safety contract.

The Test Media modal button correction is independent of filesystem behaviour. Dialog buttons now receive explicit application-priority normal, hover, focus/default and disabled styling from the dark Infiltrator palette, with background images, shadows, text shadows and inherited opacity cleared. This prevents GTK/Mint's default-button state from rendering the safe Cancel/Close action as a pale surface with unreadable near-white text while retaining the more-specific blue/red primary and destructive button semantics.

The 1.8.0-236 physical-media rerun proves the UFS and visual fixes are effective: UFS2 now formats and reaches the populated state, and the Test Media action controls use the intended dark shell treatment. The same rerun advances ZFS beyond the corrected metaslab-tail geometry and exposes the next bounded-reader gap: `features_for_write` resides in a fat/multi-block ZAP directory. The 1.8.0-237 reader validates the fat-ZAP header and leaf geometry, scans only a bounded amount of object data, validates entry/name/value chunk chains, and resolves the requested uint64 key without depending on the ZAP pointer-table hash. Malformed chains, unsupported value forms and oversized directories remain fail-closed.

The supplied physical-media log proves that UFS failed before native qualification because Debian/Ubuntu `makefs` rejected the unsupported `maxbpg` option, while ZFS was successfully created and populated but the native exact analyser then rejected its non-metaslab-aligned top-vdev `asize`. The UFS live creator now uses the same portable option subset as the existing makefs integration test. The ZFS reader now follows OpenZFS metaslab accounting: only complete metaslabs contribute allocatable space and any trailing partial-metaslab bytes remain reserved, with DVA bounds checked against that aligned allocatable region.

Test Media's bottom-right Qualify, Verify and Build controls no longer carry toolkit-owned `suggested-action` / `destructive-action` classes. Their normal, hover and disabled states are explicitly owned by Test Media. The 1.8.0-236 correction also stops using Common's light filled `button_background_rgb` action role for this dark shell: the controls now use the same dark card/background surfaces as the rest of Defragmenter, with accent/fault confined to borders and hover semantics. Background images, shadows, text shadows and inherited opacity are explicitly cleared for these states so host GTK styling cannot wash them back to pale slabs.

Test Media now carries the saved Build Test Disk state and diagnostic for every registered filesystem into later stages. Qualify and Verify emit a terminal status for every one of the 21 slots instead of silently skipping a filesystem that never reached the populated state. This makes UFS/ZFS build failures visible rather than leaving their rows at `Waiting`, while ZFS and swap retain their intentional analysis-only/no-Defragment qualification states when successfully built.

The four writable failures observed in the supplied 1.8.0-231 physical-media qualification are addressed at their actual boundaries. FAT12's insufficient-whole-object staging condition was already removed by 1.8.0-233's journalled rolling-cluster scheduler. XFS live media no longer manufactures fragmented directory metadata that the qualified writer deliberately protects, while retaining the full heterogeneous 200 MiB fragmented regular-file corpus. Btrfs live media is now explicitly created inside the documented single-device CRC32C level-0 mixed-group contract, with portable 4 KiB mixed geometry, NODATACOW/NODATASUM retained files and two durable separated extents per target file; broader checksum-tree and deeper-tree behaviour remains covered by native fixture tests and fail-closed cases. Classic HFS sacrificial media reconciles the MDB free-block counters to the allocation bitmap after host-side population and before invoking the unchanged strict production preflight, so the fixture itself is internally consistent without weakening rejection of inconsistent user volumes.

The previously unreported UFS slot also had an independent Test Media oracle defect: its raw payload verifier counted the eight manifest targets and directory-stress files but omitted the eight retained boundary-sized files. The oracle now includes those files. The ZFS analysis-only live profile retains the complete 200 MiB fragmented-file corpus while avoiding thousands of unrelated directory records that could push the disposable pool outside the bounded exact-reader topology. The native GUI also decodes the POSIX child wait status so an ordinary exit code 1 is reported as 1 rather than raw wait value 256, and its Result column now recognises the worker's actual `verified` / `verify-failed` protocol statuses.

Hosted and self-hosted regression coverage locks the 21-slot accounting, XFS/Btrfs/ZFS bounded live profiles, UFS boundary-file count, HFS MDB reconciliation and decoded worker status. The destructive physical-media rerun remains environment-dependent evidence: this audit does not represent the hosted suite as proof that a particular MMC/USB device has completed the corrected 21-slot run. Publication remains conditioned on the exact-head warnings-as-errors build, shipped native GTK Xvfb smoke test, complete CTest suite and ASan/UBSan lane.

The release-governance baseline is `c42c943d1d0d60a55d47cf5e936afbe7ff88b715`. The malformed-media matrix, transaction/recovery tests, native integration fixtures, GUI contract tests, release/package tests and architecture ownership checks remain part of the permanent **Project quality gate**. Environment-dependent destructive-media evidence remains supplementary and is not represented as hosted-CI proof.

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

The audited production tree consumes Infiltratr Common 1.19.38 at exact commit `7070c5812b50821fd7580101cb2289a3184f6b2c`. CMake, the local installer, the Git submodule and release regressions assert the same released pin.

Common owns generic checked arithmetic, strict numeric/config parsing, endian access, bounded growth, path/string primitives, exact I/O, generic durable-file operations, design metrics/semantic roles, typography identity and canonical font provenance. Defragmenter retains filesystem geometry and interpretation, target-safety policy, placement, transaction/recovery semantics and allocation-map meaning. This boundary is enforced by architecture tests rather than documentation alone.

## Release controls and decision

The repository uses the `direct-main` development model. The active `protected-main` history contract, exact audited source/governance baselines, immutable release checks and hosted **Project quality gate** collectively guard publication.

Ordinary main commits never publish. An **explicit release decision** is represented by a commit whose subject begins `Release <version>`. The exact head must pass the hosted full-suite and ASan/UBSan lanes; release automation then verifies the version, audit baselines, source/workflow drift and immutable tag identity before creating the GitHub release.

APT refresh remains a direct release-workflow stage, but publication ownership is pull-based: Defragmenter validates and records its immutable released identity without cross-repository dispatch credentials, while Infiltrator-Repository independently discovers the release on its own schedule.
