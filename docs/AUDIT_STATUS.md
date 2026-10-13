# Safety audit status

Status: **complete**

Completed: 2026-10-13

Applies to: audited source and release-governance baselines below
Audited source commit: a6b508415f7d005d1edfe8d566b58af823db7bf3
Audited release-governance commit: a6b508415f7d005d1edfe8d566b58af823db7bf3

Audited writer IDs: fat12, fat16, fat32, exfat, ntfs, ext4, xfs, affs, apfs, btrfs, pfs3, sfs, hfs, hfsplus, minix, ufs

This document records the current release safety case for the exact audited source baseline above. Publication still requires the exact release head to pass the self-hosted Project quality gate and the release-governance drift checks.

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

The repository uses the `direct-main` development model. The active `protected-main` history contract, exact audited source/governance baselines and self-hosted **Project quality gate** collectively guard publication.

Ordinary main commits never publish. A change to `defragger/VERSION` on protected `main` is the explicit release decision; commit-message wording is not part of the control plane. The exact head must pass the self-hosted full-suite and ASan/UBSan lanes. Release artefacts are built once by that qualified head and publication can only publish or resume those exact SHA-bound artefacts after checksum, audit-baseline and protected-history verification.

GitHub release correctness is independent of downstream APT propagation. Publication succeeds only after the GitHub release and asset digests are exact; the public APT visibility check is observational and retryable, so repository/CDN delay cannot invalidate an otherwise verified immutable GitHub release.
