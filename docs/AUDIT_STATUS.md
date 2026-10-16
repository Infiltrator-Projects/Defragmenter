# Safety audit status

Status: **complete**

Completed: 2026-10-16

Applies to: audited source and release-governance baselines below
Audited source commit: eca7037aff3641512ea458cf7533b966ea6978a6
Audited release-governance commit: eca7037aff3641512ea458cf7533b966ea6978a6

Audited writer IDs: fat12, fat16, fat32, exfat, ntfs, ext4, xfs, affs, apfs, btrfs, pfs3, sfs, hfs, hfsplus, minix, ufs

This document records the current release safety case for the exact audited source baseline above. Publication still requires the exact release head to pass the self-hosted Project quality gate and the release-governance drift checks.

The 2026-10-14 repository-wide forensic pass rechecked dependency ownership and removed the FAT catalogue's private pathname-concatenation implementation. FAT now uses the same checked arithmetic and Common-owned lexical path contract already used elsewhere, so generic pathname construction has one stronger implementation while filesystem interpretation remains locally owned.

The second 2026-10-14 forensic pass identified a release-recovery hole rather than a filesystem defect: cancelling an in-progress `main` qualification could discard the only run carrying a VERSION-change release decision, and the once-built qualified artefact was retained for only one day. Main qualification is now serialized instead of cancelled, and its exact SHA-bound release artefact is retained for seven days. The semantic release regression fixes those properties as part of the governance contract. This preserves the existing single-build rule while making restartable publication survive ordinary multi-day service or propagation failures without rebuilding a qualified release.

The 2026-10-16 incremental review covers the two subsequent UFS Test Media commits, `00f8287b8c10efefc87f6b54c1c4b440e5a05078` and `7424678c7adc283da5e14cf52b426afcce0261d7`. The first bundles a bounded, little-endian UFS2 fixture creator, installs and resolves that package-owned helper, removes the external makefs prerequisite, and adds mandatory creation/analysis/defragmentation and staged-install coverage. The second runs that same creator/worker integration from release-artifact qualification. The creator's geometry, allocation bounds, inode/direct/indirect block construction, source-copy error handling and metadata accounting were reviewed alongside the build, installation, resolver and test changes. Existing filesystem mutation and recovery engines are unchanged. The former feature branches have identical changed-file contents on main; their separate commit ancestry is a result of squash integration.

The existing qualification of this exact source completed all 46 CTest cases and the ASan/UBSan lane; publication stopped at the correctly enforced stale-audit-baseline check. That UFS baseline subsequently qualified and published as 1.8.0-266. The current baselines also include the reviewed qualification and retry changes below. No release assertion, audit-drift check, history protection or test has been disabled or relaxed.

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

## Release-effective qualification and retry review — 2026-10-16

The current reviewed source replaces `assert()` in the native desktop policy and live-map regressions with unconditional failure-reporting checks. The Release CTest run completed all 46 cases and the ASan/UBSan lane passed at `67fe8eea61d01945809755870b65f40eb3dc49cf`; those runs now exercise the affected checks instead of compiling them away. Filesystem parsers, mutation engines, recovery semantics and privilege policy are unchanged.

Release intent now follows the unpublished `VERSION` across corrective commits instead of depending on the immediately preceding push. The actual workflow decision is executed against disposable Git histories covering initial intent, a correction without another version bump, an exact-commit retry, ordinary maintenance after a tag, and an advanced version. Fixtures isolate their own Git signing policy. These behavioural fixtures passed locally with the intended Git Bash runtime. The source and governance baselines above identify the complete reviewed implementation; the final release head must still pass the full mandatory qualification and exact-artifact publication gates.

## Package naming review — 2026-10-16

The incremental review from `1e744115b688984f0e428484c3aa56b77a2be3e5` to `eca7037aff3641512ea458cf7533b966ea6978a6` covers the canonical `defragmenter` Debian identity, its package-owned icon and native-installer queries, and the `Libraries` repository URL and dependency diagnostics. The native installer continues to build and verify the same package version. Versioned Provides, Breaks and Replaces retain upgrades from `infiltrator-defragmenter` and `linux-defragger`; executable paths and the pinned library revision are unchanged. Matching packaging and dependency tests use the canonical names.

At the reviewed source commit, all 46 Release CTest cases and the ASan/UBSan lane passed in [qualification run 38979033117](https://github.com/Infiltrator-Projects/Defragmenter/actions/runs/38979033117). Qualification then stopped at the expected source-baseline drift check. The source diff contains no filesystem parser, mutation engine, recovery or privilege-policy changes. The governance diff changes only two dependency-step display labels. The baselines above now identify these reviewed changes; the final publication head must still pass every existing qualification, audit-drift, checksum and exact-artifact gate.
