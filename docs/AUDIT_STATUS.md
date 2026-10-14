# Safety audit status

Status: **complete**

Completed: 2026-10-14

Applies to: audited source and release-governance baselines below
Audited source commit: 5dc3bc8269e5557c8221aacd59e5491f91e32113
Audited release-governance commit: 5dc3bc8269e5557c8221aacd59e5491f91e32113

Audited writer IDs: fat12, fat16, fat32, exfat, ntfs, ext4, xfs, affs, apfs, btrfs, pfs3, sfs, hfs, hfsplus, minix, ufs

This document records the current release safety case for the exact audited source baseline above. Publication still requires the exact release head to pass the self-hosted Project quality gate and the release-governance drift checks.

The 2026-10-14 repository-wide forensic pass rechecked dependency ownership and removed the FAT catalogue's private pathname-concatenation implementation. FAT now uses the same checked arithmetic and Common-owned lexical path contract already used elsewhere, so generic pathname construction has one stronger implementation while filesystem interpretation remains locally owned.

The second 2026-10-14 forensic pass identified a release-recovery hole rather than a filesystem defect: cancelling an in-progress `main` qualification could discard the only run carrying a VERSION-change release decision, and the once-built qualified artefact was retained for only one day. Main qualification is now serialized instead of cancelled, and its exact SHA-bound release artefact is retained for seven days. The semantic release regression fixes those properties as part of the governance contract. This preserves the existing single-build rule while making restartable publication survive ordinary multi-day service or propagation failures without rebuilding a qualified release.

## Current safety case

Write-capable engines are admitted only when the parser, placement model, durable recovery contract and independent final verification agree on the same bounded on-disk subset. Unsupported, structurally ambiguous, mounted or identity-mismatched targets fail closed before authoritative writes.

Filesystem-neutral block topology follows the Linux sysfs object model rather than assuming every block kobject has identical relation directories: partition nodes contribute their containing disk plus `holders/`, while whole/stacked block devices additionally contribute `slaves/` and child partitions. Required relation/parent metadata still fails closed. A host-partition regression exercises this boundary through the production mount-topology API.

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
- **APFS** — exact checkpoint/spaceman/catalog analysis plus bounded offline Defragment, Growth Defrag and Recover for the qualified checkpoint state.
- **UFS1/UFS2** — exact cylinder-group allocation and inode block-tree analysis plus bounded Defragment, Growth Defrag and durable Recover.

ZFS/OpenZFS and Linux swap remain analysis-only by design.

## Release controls and decision

The repository uses the `direct-main` development model. The active `protected-main` history contract, exact audited source/governance baselines and self-hosted **Project quality gate** collectively guard publication.

Ordinary main commits never publish. A change to `defragger/VERSION` on protected `main` is the explicit release decision; commit-message wording is not part of the control plane. Main qualification runs are serialized so a later ordinary push cannot cancel and erase an earlier VERSION-changing release decision. The exact head must pass the self-hosted full-suite and ASan/UBSan lanes. Release artefacts are built once by that qualified head, retained for a bounded seven-day recovery window, and publication can only publish or resume those exact SHA-bound artefacts after checksum, audit-baseline and protected-history verification.

GitHub release correctness is independent of downstream APT propagation. Publication succeeds only after the GitHub release and all three expected asset identities/digests are exact; the public APT visibility check is observational and retryable. The project publisher treats version identity as write-once and rejects tag, target or asset drift rather than relying on GitHub's separate platform-level immutable-release setting.
