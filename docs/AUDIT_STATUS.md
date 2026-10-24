# Safety audit status

Status: **complete**

Completed: 2026-10-23

Applies to: audited source and release-governance baselines below
Audited source commit: 11804bbb6ba94801aee6176613bb3eb4529240cb
Audited release-governance commit: d89dff435d41eb36c2e2fec8d679f87f0d7d4ada

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

## Canonical alpha naming review — 2026-10-17

The current review covers the canonical `defragmenter` package, commands, native desktop and operation helpers, installer payload identity, installed resource directories and journal-state root. Build targets, environment settings, tests and documentation use the same names. Package migration metadata and aliases have been removed. No installation migration is supported or required for this alpha release.

The exact Libraries dependency advances from 1.19.38 to 1.19.46. Its reviewed changes add bounded Calendar date-provider loading, clock presentation/localization, design-record validation and the canonical System Settings provider identity. Arithmetic, pathname construction, filesystem-neutral persistence primitives and filesystem parsers are unchanged by that dependency delta. Libraries' complete local contract suite passed at the new pin.

The own-source changes are product identities and consistent path/build substitutions. The same UID-bounded journal policy now uses the canonical state root, and installer payload discovery and removal use matching canonical markers. Journal and request magic strings use the canonical product namespace; earlier namespace strings are not accepted. Filesystem formats, mutation algorithms, journal structure and privilege boundaries are unchanged. The final release head must pass all 46 Release CTest cases, ASan/UBSan, desktop smoke, artifact installation, audit-drift and exact-byte publication checks. This review does not substitute for those mandatory results.

## Incremental native-font bootstrap audit — 2026-10-18

The production-source delta from the previously reviewed baseline to
`c144ebda5d8eb646da813f68223bd2b9b1529f83` affects only `defragger/packaging/ensure-native-fonts.sh`.
Its implementation is a reviewed copy of Libraries/Common 1.19.58's
independent three-face bootstrap. The old MBLink archive is no longer fetched
and the obsolete version 1.000 MB Corpo filenames are not recreated. It uses
the three SHA-256-verified, version 1.010 Title faces from the immutable
InfiltratorOS commit `66c17fe9ea3ea9678b4028ffea1862a37d488f79`,
prefers a verified local font seed, rejects symlinked native resource paths,
and atomically publishes only validated font bytes. The Fontconfig
registration and package-owned/unowned lifetime contracts are preserved.

The `c144ebda5d8eb646da813f68223bd2b9b1529f83` qualification completed all 46 CTest cases and the
ASan/UBSan lane successfully. Its release-gate test rejected the source
change for the correct reason: the audited source baseline had not yet
been advanced. This incremental review now advances *only* the source
baseline to that exact font-script revision; the independent governance
baseline is unchanged. The audited filesystem writers, analysis engines,
privilege boundaries, on-disk formats, durable journal and recovery logic
have not changed. Mandatory post-audit qualification must still pass
before any new release can be published.

## Recovery, analysis and raster correctness review — 2026-10-23

The current baselines cover the reviewed corrections from the 1.8.0-270
release-readiness review. No writer placement algorithm, on-disk journal schema,
staging identity, privilege namespace or durable transaction phase has changed.

Recovery admission is separate from new-layout admission. A restarted desktop
may offer Recover for a supported offline writable target with an existing
user-specific journal even when the source cannot pass normal analysis. This
only reaches the existing native validator: journal magic, operation/phase,
stable target identity/capacity and verified staged bytes remain authoritative.
Normal Defragment and Growth Defrag still require native identity and exact
writer qualification. The APFS regression demonstrates a real committing
journal and verified stage with inconsistent partially copied source metadata;
analysis rejects that image and Recover restores its exact payload and layout.

Btrfs read-only analysis now verifies the CRC32C primary superblock and every
consumed tree block using the same checksum code as the qualified writer.
Failed filesystem-tree walks and malformed allocation items propagate failure
rather than becoming a plausible exact result. Other checksum algorithms are
explicitly unsupported. The malformed structural fixture recomputes its CRC,
so its geometry test still reaches and exercises the independent structural
validator rather than merely duplicating checksum rejection.

The allocation raster consumes whole native analyser cells in physical order.
Small volumes use no more pixels than their proven allocation cells; FAT
clusters are not expanded into sector pixels. Larger inputs combine adjacent
cells. Painting and the integer-density caption share one small pure geometry
function; repeated-pixel and sector-expansion machinery has been removed.

The APT observer selects the Debian package identity from the qualified
artifact instead of the obsolete catalogue ID. Both catalogue and Packages
index must contain the qualified version and SHA-256. The real observer is
executed against a real Debian fixture with valid, stale and altered-digest
HTTP responses; only its retry clock is shortened. The quality preflight now
names the additional parser prerequisites used by that behavioural test.

Maintained documentation identifies the actual Libraries 1.19.46 gitlink and
separately identifies the reviewed 1.19.58 font bootstrap. Historical release
notes retain their original dependency identities.

Local warnings-as-errors builds and the available native/transaction/parser
regressions passed. All locally runnable sanitizer cases passed; this container
exposes no physical partition nodes and cannot create the GTK X server sockets.
Those environment limits do not waive the permanent full-suite, GTK smoke,
ASan/UBSan, audit-drift, exact-artifact installation and publication gates. The
exact 1.8.0-271 release head must pass them before publication is accepted.

## Driver-free Test Media and complete-corpus review — 2026-10-23

The release-readiness review reproduced a false successful verification when schema 2 state omitted every expected Btrfs file. That failure class is removed: schema 3 state binds disk identity, corpus version and exactly one build result per slot, with no saved file list, size list or hash list. The production parser rejects missing/duplicate/unknown/truncated records and incompatible schemas. Native verification derives all expected files and bytes from compiled corpus policy; UFS2 regenerates every large-file and boundary-file hash rather than accepting mutable expectations. Existing schema 2 test disks require rebuilding.

Fifteen remaining host-created slots now use bounded first-party raw C codecs: FAT12/16/32, exFAT, NTFS, EXT2/3/4, XFS v5, Btrfs, classic HFS, HFS+, Minix v3, ZFS and swap. Shared corpus accounting requires every expected file exactly once and complete logical byte coverage. Format codecs follow the current mappings after relocation; they do not use the original physical placement as a verification oracle. Metadata allocation, native unit geometry, pointer/run bounds, complete tail lengths, filesystem-specific checksums, clean journal/log state, mirror/header construction and reused-slot signature clearing were reviewed with the actual production readers and unchanged writers. The native readers are reused where their public bounded contracts fit; no alternate mutation implementation is introduced.

Target formatter/mount/pool-tool paths, mutable expected-list state, unused allocator-anchor profiles and optional host-package hints have been removed. Creator readiness uses the canonical slot registry and correctly identifies ZFS as built in. UFS2 remains bound exclusively to the package-owned private C helper; no host makefs is accepted. The production external-tool regression now covers Test Media. Canonical large-file sizes remain 4, 6, 10, 16, 26, 34, 44 and 60 MiB, plus the eight boundary cases. OFS/FFS directory stress and the existing SFS/PFS3/APFS contracts remain qualified separately. The new Btrfs fixture uses 16 KiB equal sector/leaf geometry within the existing level-zero writer contract. ZFS and swap remain analysis-only.

Local optimized fixture qualification exercised every additional file-bearing fixture through native creation, exact analysis, complete payload verification and deliberate corruption rejection. Both native relocation modes passed for every additional write-capable format, including the full 200 MiB corpus and tails. The strict state/corpus regression executes the production parser and verification accounting. Mandatory post-audit qualification still requires the exact versioned release head to pass the complete 48-case self-hosted suite, ASan/UBSan, GTK smoke, artifact installation and immutable exact-byte publication. No existing writer qualification, recovery phase, audit-drift rule or release assertion is relaxed.

The 1.8.0-272 final-run follow-up reviews a test-orchestration-only change after both self-hosted lanes exhausted the uniform 240-second command budget in full-corpus EXT2 relocation. Mutation commands now have a finite 900-second budget; creation, analysis and verification retain 240 seconds. The complete 200 MiB corpus, both relocation modes, byte-for-byte oracle, corruption rejection, production durability barriers and unchanged 45-minute job limits remain mandatory. This timing allowance does not establish a passing result: the corrected exact head must still qualify before publication.

## Bounded independent full-corpus qualification — 2026-10-23

The 1.8.0-272 full-corpus regression passed FAT12, FAT16, FAT32, exFAT, NTFS and EXT2 but exhausted the 1,800-second aggregate test limit before reaching the remaining filesystem cases. Both mandatory lanes failed closed and correctly prevented publication. Extending only the per-command deadline did not solve that serial-orchestration bottleneck.

The reviewed source at `d89dff435d41eb36c2e2fec8d679f87f0d7d4ada` replaces the serial sixteen-format Python driver invocation with one separately named CTest case per filesystem. Each case still executes the unchanged first-party creator, complete expected-file and byte oracle, exact native analyser, both actual relocation modes where supported, independently reopened postconditions and intentional live-payload corruption rejection. ZFS and swap retain their bounded analysis-only contracts. All sixteen cases use independent temporary images and journals, and the self-hosted quality and sanitizer lanes run them with bounded four-way concurrency after their remaining native/architecture/safety tests. Each case retains its 1,800-second watchdog; both job-level 45-minute bounds and unchanged exact-source/artifact publication gates remain in force.

The source and the single affected workflow are audited at the same commit above; no filesystem writer, transaction, privilege, format codec, recovery policy or release-publisher implementation has changed. This audit records the justified reorganization of qualification, not a claim that the new exact release head has already passed. Publication still requires successful complete normal and sanitizer lanes, artifact installation, release-history validation and checksum-bound publication. Physical power-loss testing remains independent environment-dependent evidence.

## EXT transaction flush-frequency correction — 2026-10-24

The independent full-corpus CI run at `b075bc1dd5029f306ccfb0276c0e69a69736e106` passed all 47 regular non-fixture cases and all 46 sanitizer non-fixture cases. Thirteen of sixteen full-corpus filesystem cases, including XFS, NTFS and Btrfs, also passed in both lanes. EXT2, EXT3 and EXT4 alone timed out at the 900-second individual Defragment command watchdog; publication was correctly skipped. The log demonstrates a timeout, not a verified storage error or successful qualification.

Review of the first-party EXT direct workspace identified an unnecessary durability amplification: each short physically fragmented input run and each short canonical output run independently triggered `fsync`, irrespective of the configured maximum 64 MiB transaction budget. On the fragmented qualification corpus this creates thousands of device-wide syncs, contending with the three simultaneous EXT jobs. Source commit `11804bbb6ba94801aee6176613bb3eb4529240cb` changes both paths to accumulate transferred block counts *across* run boundaries and sync at each bounded budget or at phase completion. No journal or recovery format was changed; no postcondition was removed. The workspace is synced before its digest and `direct-staged` phase publish, canonical payloads are synced before metadata mutation, and restored source bytes are synced before discarding the recovery journal. Incomplete workspace staging still cannot modify live data. Timeout diagnostics now retain the trailing native phase transcript for subsequent root-cause analysis.

This audit records the review and focused correction. The claim of release qualification remains conditional upon complete self-hosted CI, sanitizer, GTK, source-drift, artifact-installation and publication checks for the exact final head; testing a real power-loss interruption remains separate environment-dependent evidence.
