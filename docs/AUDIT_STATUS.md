# Safety audit status

Status: **complete**

Completed: 2026-09-22
Extended: 2026-10-04

Applies to: release version 1.8.0-250
Audited source commit: ff00f2575ee40c6c9d25137647eac73d9b673833
Audited release-governance commit: 628eb90439a3e2653033bdab7acfe4cca0716fd0

This status records the 1.8.0-250 release audit. A newer `main` commit is not implicitly covered merely because the release version string has not changed; release eligibility still requires the exact-head quality gate and the source/governance drift checks described below.

Audited writer IDs: fat12, fat16, fat32, exfat, ntfs, ext4, xfs, affs, apfs, btrfs, pfs3, sfs, hfs, hfsplus, minix, ufs

This document records the completed 1.8.0-250 release safety case and the explicit release decision after warnings-clean qualification. Re-running the release gate uses the audited source baseline named above. Historical audit-development detail remains in Git history and immutable release tags rather than being repeated as a second changelog.

## Qualification evidence

The 1.8.0-250 extension corrects one native ZFS analysis geometry bound exposed by physical Test Media qualification. An ordinary OpenZFS single-disk pool produced a MOS metadnode with `nlevels=6`; Defragmenter's bounded exact analyser rejected it only because `ZFS_DNODE_MAX_LEVELS` was hard-coded to 5. OpenZFS derives `DN_MAX_LEVELS` as 12 for the current minimum block and block-pointer geometry, so the analyser now accepts levels 1 through 12 while retaining the existing indirect-block shift, overflow, checksum, compression, vdev and allocation bounds. The regression boundary now rejects level 13 instead of level 6. The complete hosted native/filesystem/GUI suite and the hosted ASan/UBSan lane passed on audited source commit `ff00f2575ee40c6c9d25137647eac73d9b673833`; the exact-head 1.8.0-250 full and sanitizer gates remain mandatory before publication. No writer, UI, Common pin, Test Media formatter/populator or release-governance workflow changed.

The 1.8.0-249 extension retires the uninstalled Python GUI launcher, its Gtk.Application lifecycle and two startup-only artwork helpers, removing 89 net application-code lines. The installed launcher still executes the native C++17 GTK desktop. Remaining Python modules are retained source/test fixtures; authoritative native engines under gui/filesystems remain intact.

The real native GTK desktop regression now saves an isolated Night preference before construction, checks the loaded Common palette and verifies the approved 96-pixel window icon. It passed normally and under ASan/UBSan, alongside its existing progress, administrator transport and bounded Stop checks. Retained GUI model/About checks, architecture checks and Python type-checking passed. The exact-head hosted full-suite and sanitizer gates remain mandatory before publication. Production native desktop code, artwork, MB font assets, Common pin, filesystem analysis/writers, Test Media and release governance are unchanged from 1.8.0-248.

The 1.8.0-248 extension changes only the EXT descriptor-refresh function. A native C fixture reproduced a read-only scan failing when the first refreshed descriptor fails its checksum but the next descriptor/bitmap pair is valid. Descriptor refresh now permits at most three reads, retains the previous descriptor after rejection, and accepts a replacement only after checksum verification. Real I/O errors still stop the read; persistently invalid descriptors and bitmaps remain rejected. Writable scans still never refresh their locked descriptor snapshot.

The regression covers transient refresh failure for both block and inode bitmaps, exactly three reads for persistent descriptor rejection, retained descriptor bytes, rejected cache fills and writable snapshot behavior. It passed normally and under ASan/UBSan. Native allocation/metadata-map and malformed-input checks passed locally. The complete local rebuild encountered missing extracted test-library links; exact-head hosted warnings-as-errors and full/sanitizer gates remain mandatory before publication. The user's physical group 1062 remains unverified. The Common pin, desktop, Test Media, filesystem writers and release governance are unchanged from 1.8.0-247.

The 1.8.0-247 extension addresses the remaining NTFS qualification cost and EXT read-only cache behavior observed in the user's logs. An address-ordered maximum tree preserves the exact canonical earliest-fitting free-run policy and Growth reserves while eliminating the per-stream full free-run search. Native C tests compare both modes against an independent linear reference and check 100,000 exact placements in 100,000 isolated free runs (about 0.04 CPU seconds locally). These timings cover planning, not the user's physical drive.

NTFS analysis now requests both availability decisions from the same MFT catalogue. Each mode plans against its own private copy of the original bitmap; allocation JSON remains unchanged by tentative plans. The dirty flag retained from validated volume metadata, malformed-record count, hibernation state and unsupported-layout rejection are checked before qualification. Separate writer preflight commands use the same qualification routine, and actual mutations still rescan, rebind and enforce the original mount, identity, transaction and recovery guards. Read-only fixture checks compare combined/standalone decisions, dirty-state rejection, unchanged allocation ranges and unchanged source hashes. The mapper conservatively validates both qualification fields and keeps a fallback for workers without the combined protocol.

EXT read-only bitmap rejection permits at most three checksum-verified descriptor/bitmap attempts. Lazy-uninitialised flags are refreshed before synthesis. Read/write failures and checksum rejection leave the cache invalid, so a repeated same-group request cannot accidentally accept rejected bytes. Tests demonstrate a valid updated pair after an old descriptor snapshot, persistent block/inode bitmap rejection, malformed descriptor rejection and unchanged writable snapshots. No checksum is skipped. Mounted live views are labelled as such; persistent inability to verify them remains a failure. The user's physical group 1062 has not been reproduced and this audit does not assert disk corruption or guaranteed success for an actively changing filesystem.

Native worker progress is excluded from error text after forwarding to the desktop. Local read-only analysis runs in a dedicated process group; Stop signals that group and retains a five-second force-stop fallback, preventing subordinate processes from holding the UI's output pipes open. The native desktop regression exercises streamed progress, final map acceptance, administrator transport and a shell with a child process during cancellation. Normal and ASan/UBSan GTK runs passed using the canonical MB fonts. A desktop whose executable was replaced by an update requires reopening before another operation, avoiding future mixed desktop/worker sessions. Main desktop and Test Media colours, MB typography and the previously qualified layout are unchanged.

The new native cache/index/protocol tests, combined NTFS fixture checks and existing EXT allocation/metadata classifier checks passed locally. The complete local CTest run encountered the known unavailable secure-directory syscalls and Unix-display sockets; those guards remain intact and unrestricted full-suite and sanitizer hosted gates are mandatory before publication. The production filesystem external-tool prohibition and native architecture checks passed. Source/build changes in this extension are bound to the exact commit above; the release-governance baseline remains unchanged.

The production/build/Test Media baseline for this release is the source commit recorded above. The 1.8.0-247 review covers a rebuilt Test Media workspace, NTFS catalogue scaling and analysis progress. The approved graphical colours and Common MB Corpo A/S font identities and weights are retained. Disk selection, Build → Qualify → Verify and activity remain visible while filesystem layouts, details and logs scroll in their own results pane. All 21 tiles select an inspector; full diagnostics and clipboard copying remain available. Device discovery, protected-device guards, fingerprint confirmation and worker lifetime behavior are preserved.

NTFS logical stream membership now uses one sorted index keyed by base owner, attribute type and case-sensitive attribute name. This removes repeated catalogue-wide searches from movable-stream qualification, fragmentation accounting and preserved-primary preflight. The index returns counts in the original catalogue order. Split-stream rejection, lowest-VCN validation, movable qualification, canonical placement, transaction publication, checksums and recovery semantics are preserved. A regression covers base/extension records, different names and types and a 100,000-stream catalogue. The release build completed the indexing step in 0.014 CPU seconds locally; this is not evidence of the user's physical-volume runtime.

Mapper progress is emitted on stderr while stdout retains the final allocation JSON. Captured filesystem-worker progress is forwarded immediately. The desktop consumes these events through both local and administrator transports, shows NTFS MFT record counts, reports the two availability-check phases, shows elapsed time and clears stale map/gauge values. The local path now drains stdout and stderr asynchronously, waits for both EOFs and process completion, and preserves Stop supervision before accepting the final validated map. Progress never grants writer qualification. EXT bitmap rejection adds its group, bitmap block and expected/actual checksums; checksum validation is not bypassed or represented as a confirmed diagnosis of physical corruption.

Local qualification passed the warnings-as-errors full native build, real GTK tests with the verified bundled MB font families/weights, fixed disk/workflow/footer controls, all 21 interactive tiles, selected failure diagnostics, full-detail dialogs, live-log final-output retention and clipboard copying. The native desktop regression proves progress arrives before the final JSON, administrator events do not contaminate allocation JSON, failed qualification leaves mutation disabled, and cancellation clears pending analysis. The GTK regressions, NTFS index and process-capture tests passed ASan/UBSan. A native NTFS fixture produced valid allocation JSON and scan/qualification progress without changing source bytes. Architecture and GUI contract checks passed. The local full CTest run exercised the complete suite; recovery-directory/secure-open tests that require unavailable sandbox syscalls are left to the mandatory unrestricted hosted gates rather than weakening those checks.

The reviewed release-governance baseline remains `628eb90439a3e2653033bdab7acfe4cca0716fd0`. The local BigBedroom lane provisions Node.js for the pinned Python type checker, requires Xvfb tools and fails missing prerequisites. The mandatory hosted full-suite and ASan/UBSan lanes, main-only publication handoff, immutable release identity and Common 1.19.38 pin are preserved. This release decision is eligible for publication only after those exact-head hosted gates pass. Environment-dependent physical-media testing remains supplementary and is not claimed as hosted-CI proof.

The previously qualified FAT12/FAT16/FAT32, exFAT, NTFS, EXT, XFS, Btrfs, Amiga OFS/FFS/SFS/PFS3, HFS/HFS+, Minix, APFS and UFS writer contracts, and the analysis-only ZFS/swap boundary, remain covered by the permanent **Project quality gate**. The 1.8.0-244 ZFS exact dnode/indirect geometry corrections, stage-specific qualification failures and buffered output draining are retained.

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
