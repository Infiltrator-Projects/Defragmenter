# Safety audit status

Status: **complete**

Completed: 2026-10-06

Applies to: release version 1.8.0-252
Audited source commit: 04fd283dcf585a59caec14b8fad9d2a681cba327
Audited release-governance commit: 628eb90439a3e2653033bdab7acfe4cca0716fd0

Audited writer IDs: fat12, fat16, fat32, exfat, ntfs, ext4, xfs, affs, apfs, btrfs, pfs3, sfs, hfs, hfsplus, minix, ufs

This document records the current release safety case for the exact audited source baseline above. Publication still requires the exact release head to pass the hosted Project quality gate and the release-governance drift checks.

## Current safety case

Write-capable engines are admitted only when the parser, placement model, durable recovery contract and independent final verification agree on the same bounded on-disk subset. Unsupported, structurally ambiguous, mounted or identity-mismatched targets fail closed before authoritative writes.

- **FAT12/FAT16/FAT32** — native allocation/catalogue analysis, canonical relocation, Growth Defrag reserve and Recover.
- **exFAT** — native catalogue/relayout, Growth Defrag reserve and Recover.
- **NTFS** — native fail-closed preflight, run-oriented relocation, Growth Defrag reserve and Recover.
- **EXT2/EXT3/EXT4** — first-party native metadata validation, allocation mutation, staged commit and Recover.
- **XFS** — native raw catalogue, range-oriented planning, verification and Recover for the qualified layout.
- **Btrfs** — first-party bounded offline analysis and relocation for the qualified single-device subset, with unsupported sharing, encoding, snapshots and active transaction state rejected.
- **Amiga OFS/FFS** — native raw catalogue, relocation, verification and Recover for the qualified classic layout.
- **Amiga SFS0** and SFS2 — native supported-subset relayout and Recover with format-specific metadata validation.
- **Amiga PFS3** — bounded native allocation/anode analysis, relocation and Recover for the qualified subset.
- **Classic Macintosh HFS** — exact allocation/catalog analysis plus bounded offline Defragment, Growth Defrag and Recover.
- **HFS+/HFSX** — native staged transaction and Recover for the qualified clean/journal state.
- **Minix v1/v2/v3** — exact native analysis plus recoverable Defragment, Growth Defrag and Recover.
- **APFS** — exact checkpoint/spaceman/catalog analysis plus bounded offline Defragment, Growth Defrag and Recover for a **single active two-object checkpoint** qualified state.
- **UFS1/UFS2** — exact cylinder-group allocation and inode block-tree analysis plus bounded Defragment, Growth Defrag and durable Recover.

ZFS/OpenZFS and Linux swap remain analysis-only by design.

## Release controls and decision

The repository uses the `direct-main` development model. The active `protected-main` history contract, exact audited source/governance baselines and hosted **Project quality gate** collectively guard publication.

Ordinary main commits never publish. An **explicit release decision** is represented by a commit whose subject begins `Release <version>`. The exact head must pass the hosted full-suite and ASan/UBSan lanes before immutable release publication and APT refresh.
