# Safety audit status

Status: **complete**

Completed: 2026-10-29

Applies to: audited source and release-governance baselines below
Audited source commit: 662258c4d47a3ca388a3cf9799f098993c7bdb4e
Audited release-governance commit: 662258c4d47a3ca388a3cf9799f098993c7bdb4e

Audited writer IDs: fat12, fat16, fat32, exfat, ntfs, ext4, xfs, affs, apfs, btrfs, pfs3, sfs, hfs, hfsplus, minix, ufs

This document records the current release safety case for the exact audited source baseline above. Publication still requires the exact release head to pass the self-hosted Project quality gate and the release-governance drift checks.

## FAT12/16 Test Media module integration review — 2026-10-30

The reviewed baseline above removes FAT12/16 formatting, FAT-entry writes and
directory-entry construction from Test Media. V4 formatting and V3 file APIs
run in the trusted installed SO; interleaved appends create the existing eight
large fragmented files and eight boundary files. The independent raw verifier
uses the actual data-area byte offset, including module layouts which are not
aligned to an absolute allocation-unit boundary. FAT32 retains an isolated,
explicitly temporary FAT32-only creator; no FAT12/16 fallback remains.

The shared host retains directory/file ownership and permission checks, bounded
descriptor I/O and mounted-target refusal. Defrag V2 still receives its complete
identity/capability validation. Other interfaces must validate their own ABI,
format identity and methods before opening a writable target. The OFS V3 caller
now checks every required method as well. FAT readiness is checked before disk
partitioning, and a loaded validated module is held across signature clearing,
formatting and population. The FAT ABI header is compared with the immutable
provider used by both CI lanes.

Local warnings-as-errors builds, full FAT12/16/32 corpus/relocation verification,
corruption rejection, eight missing/wrong-interface/identity/permission refusal
cases, OFS integration and Test Media core/install tests passed. The FAT16
consumer also passed with the exact released v0.1.6 SO. The affected full-corpus
and refusal cases passed ASan/UBSan. Publication still requires both complete
hosted quality lanes, package/install validation and exact-source/artifact gates.

## Mandatory OFS module integration review — 2026-10-29

The final incremental review includes the exact canonical OFS revision in the
consumer pin and the mirrored public header's kernel-only type includes. ABI
layout and process behavior are unchanged by those includes. OFS now compiles
its canonical engine into the native KO as well as the SO; all four DOS types
and seven block sizes have native read/write VM coverage. The process-host
write/recovery suite and its ASan/UBSan lane were rerun against the revised SO.

The source and governance delta through the exact baseline above migrates OFS
analysis, Defrag, Growth Defrag and new-journal Recover to the authoritative
OFS.so. A generic process host owns only descriptor I/O, stable target binding,
mounted-overlap refusal, advisory locks, trusted private durable files, SHA-256
and cooperative Stop. The module owns parsing, allocation, placement, journal
schema and recovery. Its no-import Boot profile is preserved; Linux keeps its
separate native KO until a future generic kernel SO bridge.

The reviewed write invariant is a complete, canonical, verified stage before
any source write. Strict journal fields bind canonical target path, stable
identity, capacity, geometry, immutable volume token and used-block hashes.
The staged/committing/committed phases precede source mutation or cleanup;
allocated blocks are committed with the root last and readback is verified.
Recover refuses altered stages and different targets. A staged-only journal
also requires the original used-block hash. Interrupted commits retain the
complete stage and journal. Missing/incompatible SOs never select the old
writer for a new OFS command. FFS and previously issued AFFS recovery journals
retain their exact compatibility path.

Test Media's OFS format and populate producers now call the same SO's V3 API.
Dataset generation stays in the application and the retained AFFS reader is an
independent payload oracle. The GUI and privileged preparation worker check the
required module before destructive preparation. Production hosts do not
contain test module-path overrides or fault-injection controls.

Local evidence includes warnings-as-errors builds, the native filesystem/core
suite and full Test Media corpus, standalone OFS Test Media verification,
portable/native OFS module mutation at all 28 profiles, independent content and
allocation checks, journal/device-write interruption and Stop recovery, wrong
target and altered-source/stage refusal, and the actual OFS engine plus generic
host under ASan/UBSan. The released FAT16 SO independently passed the existing
Defragmenter host's write/recovery suite and Boot Linux handoff. These are image
and VM results, not physical-media certification.

Both quality lanes now fetch the exact OFS implementation recorded in
`defragger/shared/ofs-module-revision`, compare both public ABI headers, and
provision isolated normal/instrumented modules. The source and governance drift
gates remain exact. Publication still requires the complete exact-head quality
and sanitizer lanes, immutable tested packages and release verification; this
review does not waive or predeclare those hosted results.

## Full-surface allocation map qualification review — 2026-10-28

The incremental source delta from the previously audited FAT-module revision
`f4941ec61ed2f61f938658592851cc82f2eed6f6` to
`4951b3149125e8338a7ead4f5270df04b9de1cfa` consists of the
1.8.0-276 version decision and the reviewed desktop allocation-raster correction.
The correction changes desktop map geometry, raster painting, summary wording,
one native regression and the documented visual contract. It restores full-canvas
rendering for small volumes by magnifying the colours of genuine ordered native
allocation cells; it does not subdivide those cells or invent finer-grained
physical allocation data. Larger volumes still combine adjacent cells in
physical order. The added regression exercises magnification of individual and
already aggregated cells and the smaller-canvas reduction path.

The change does not alter filesystem detection, FAT module resolution or ABI,
writer placement, target admission, privilege checks, journal formats, durable
recovery or the publication scripts and workflows. The exact commit's
self-hosted quality lane passed its warnings-as-errors build, native GTK smoke
and complete native/filesystem/GUI test suite; its independent ASan/UBSan lane
also passed. Publication stopped at the deliberately strict audited-source
drift check, because this newer UI source and regression were not part of the
earlier FAT-module audit baseline. This review advances only the source
baseline to the exact inspected commit above. The governance baseline remains
at `f4941ec61ed2f61f938658592851cc82f2eed6f6`; no gate or release
assertion is relaxed. A fresh exact-head qualification, installer-artifact
checks and publication verification remain mandatory.

## Incremental FAT module migration review — 2026-10-28

The reviewed Defragmenter source and governance baseline is
`f4941ec61ed2f61f938658592851cc82f2eed6f6`; its tree is identical to the
tested FAT module-host branch. It uses the Filesystems source at
`3592e9db5f95824b608ccf269ce22fcefab05328`, which is included in the
Filesystems main-branch merge. The new host-service declarations match the
module ABI and the existing generic core declarations. The review compared the
migrated FAT source with the previously audited implementation: the meaningful
changes are the host-service redirection, opened-target FAT-width validation,
rejection of the ambiguous 4,085-cluster boundary and fixed-root journal
recovery validation.

The loader runs in a child process, opens the installed module through checked
root-owned paths without following symlinks, verifies its ABI and width, and
retains it through process exit. The FAT engine repeats the width check on the
opened target. Generic target identity, mounted-device checks, exclusive
locking, confirmation, journal paths and Stop handling remain in force. Test
path overrides and crash injection are compiled only into a separate test
executable; they are absent from the production loader.

The qualification evidence includes warnings-as-errors builds, independent
FAT format/read/write and filesystem checks, the full FAT12/16/32 Test Media
payload and relayout cases, incorrect-module and wrong-target rejection, and
recovery after four durable journal stages and twelve individual target writes
per format. FAT copies were checked after recovery. Both the host and modules
passed ASan/UBSan tests. The Filesystems Linux and OVMF runtime lanes passed for
FAT12 and FAT16, including a 255 MiB FAT12 volume with 64 KiB clusters. The
local device-topology test was skipped in this environment. These results do
not establish physical-media or actual power-loss behaviour.

The quality workflow now retrieves the exact Filesystems revision and builds
test modules in an isolated test directory, including instrumented modules in
the sanitizer lane. Its release decision, exact-source checks, artifact handoff
and publication controls were not relaxed. The mandatory self-hosted quality,
sanitizer and release verification lanes must still pass for the exact release
head.

The 2026-10-14 repository-wide forensic pass rechecked dependency ownership and removed the FAT catalogue's private pathname-concatenation implementation. FAT now uses the same checked arithmetic and Common-owned lexical path contract already used elsewhere, so generic pathname construction has one stronger implementation while filesystem interpretation remains locally owned.

The second 2026-10-14 forensic pass identified a release-recovery hole rather than a filesystem defect: cancelling an in-progress `main` qualification could discard the only run carrying a VERSION-change release decision, and the once-built qualified artefact was retained for only one day. Main qualification is now serialized instead of cancelled, and its exact SHA-bound release artefact is retained for seven days. The semantic release regression fixes those properties as part of the governance contract. This preserves the existing single-build rule while making restartable publication survive ordinary multi-day service or propagation failures without rebuilding a qualified release.

The 2026-10-16 incremental review covers the two subsequent UFS Test Media commits, `00f8287b8c10efefc87f6b54c1c4b440e5a05078` and `7424678c7adc283da5e14cf52b426afcce0261d7`. The first bundles a bounded, little-endian UFS2 fixture creator, installs and resolves that package-owned helper, removes the external makefs prerequisite, and adds mandatory creation/analysis/defragmentation and staged-install coverage. The second runs that same creator/worker integration from release-artifact qualification. The creator's geometry, allocation bounds, inode/direct/indirect block construction, source-copy error handling and metadata accounting were reviewed alongside the build, installation, resolver and test changes. Existing filesystem mutation and recovery engines are unchanged. The former feature branches have identical changed-file contents on main; their separate commit ancestry is a result of squash integration.

The existing qualification of this exact source completed all 46 CTest cases and the ASan/UBSan lane; publication stopped at the correctly enforced stale-audit-baseline check. That UFS baseline subsequently qualified and published as 1.8.0-266. The current baselines also include the reviewed qualification and retry changes below. No release assertion, audit-drift check, history protection or test has been disabled or relaxed.

## Reviewed packaging-boundary correction — 2026-10-25

Audit source and release-governance baselines advanced to `76803476484156faacccb713b9cdcbb509e610d2`
for the Defragmenter **installation location and graphical entrypoint** change
introduced in PR #51. Source review examined the revised CMake target naming,
worker destination mapping, every application resolver and privileged-helper
path, Test Media creator lookup, package stage contracts, desktop entries and
corresponding tests. The GTK binary is now installed directly as
`/Programs/defragmenter`, with `/Programs/defragmenter-test-media` remaining
the second launchable application; the discarded third shell wrapper is not
installed. The application-owned worker tree is now under
`/System/Commands/Defragmenter`, so `/System/Filesystems` is not polluted
with Defragmenter-only programs. These changes do **not** alter filesystem
parsers, allocation writers, format-specific transactions, durable journal
semantics or recovery engines.

The full self-hosted release qualification against that exact source completed
all 47 native CTest tests and all 16 Test Media native filesystem checks, plus
the independent ASan/UBSan lane. The first run stopped at the intended
post-change audited-baseline drift control. The only change to a release-governance
input since the former baseline was the GTK/Xvfb smoke-test executable rename
from `./build/defragmenter-desktop` to `./build/defragmenter`, matching the
actual renamed binary. Both reviewed baselines now refer to that qualified
source/workflow commit; no audit check, approval restriction or test was disabled. The resulting immutable release remains subject
to passing the new CI run and publisher's exact-SHA checks.

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

## EXT fallback placement transaction review — 2026-10-24

The complete EXT2/3/4 Test Media corpus times out during working-image
fallback permutation, not the earlier high-address direct workspace. The
fallback stored each block's placement marker as a separate FULL-synchronous
SQLite WAL transaction. Source 4115692274c5b630d64d834b3735f692e3d5ff4f groups placement bookkeeping into
one transaction, synchronises the working-image bytes before committing its
placement state, and rolls back incomplete bookkeeping on error. No original
source bytes are touched by this fallback stage. The subsequent independent
stage verification, journal and source-commit recovery barriers, Stop contract,
mounted-target refusal, target binding and final payload checks are unchanged.

The review is of the source and recovery boundary, not of a passing CI result.
Complete normal and sanitizer qualification plus exact-artifact publishing
remain mandatory for 1.8.0-272.

## InfiltratorOS native executable layout and administrator-session review — 2026-10-25

The source change through `61cf2032642ea1a6207fddbc3e8763e97d8ab9b5` migrates all three user-launchable
Defragmenter entry points to the established InfiltratorOS `/Programs`
namespace: `defragmenter` (launcher), `defragmenter-desktop` (GTK program)
and `defragmenter-test-media` (GTK test-media program). The same
source revision updates both desktop launchers, the Test Media program resolver,
the Debian package installation inventory and size/accounting, and staged
package/native-installer regression fixtures. The independently installed
package remains named `defragmenter`; private filesystem workers and the
privileged helper remain under `/usr/lib/defragmenter`. A dpkg-managed upgrade
removes obsolete package-owned `/usr/bin` files; no alias or duplicate public
executable is deliberately added.

The graphical desktop previously scheduled a privileged helper launch 200 ms
after opening its window, before any protected operation was requested. The
new behaviour retains unprivileged volume discovery and requests authorization
only when `run()` needs the helper; privileged operations still pass through the
unchanged `pkexec` helper, authorization, target-safety checks, journal and
filesystem-specific recovery controls. A focused regression asserts that
the eager startup timer is absent while the requested-operation helper route
remains present. Dismissing authorization must not be confused with a broken
application executable.

The safety review is limited to installation paths, desktop launch policy,
packaging and their qualification tests. No disk-format parser, allocation
planner, mutation engine, durable recovery protocol, helper command whitelist
or privileged-helper executable is changed. This audit entry does **not**
declare CI or physical hardware qualification complete: both mandatory
self-hosted normal and sanitizer lanes must pass on the exact subsequent
revision before promotion or a versioned release. The release-governance audit
baseline is unchanged.

## Native OS private runtime and documentation migration — 2026-10-25

The bounded application packaging change through `449d079c963a4ab6250fa064bba86008dd425f62` finishes the migration
begun by 1.8.0-273. All user-facing programs remain in `/Programs`.
Filesystem-specific private workers, the mapper, operation engine,
privileged helper, and Test Media format creators move from the
package-owned `/usr/lib/defragmenter` tree to
`/System/Commands/Defragmenter`; the font bootstrap moves to
`/System/Commands/defragmenter-ensure-fonts`.
The approved runtime icon and UI artwork are installed under the native
`/System/Resources/Icons` and `/System/Resources/Themes/Defragmenter`
categories, and product README/license under
`/System/Resources/Documentation/Defragmenter`.
Legacy package-owned `/usr/lib/defragmenter` and
`/usr/share/doc/defragmenter` paths are intentionally omitted. Linux
desktop launcher/icon-cache metadata is retained solely for integration with
the transitional Mint host.

Installed paths, static privileged-command allowlisting, runtime resolution,
packaging manifests, staged install, and end-to-end installer fixtures are
updated together. CMake's build target retains the stable name
`defragmenter-ext-metadata-worker` but uses the installed and build-output
filename `ext-metadata-worker` to fit OFS's 30-character name limit.
No parser, allocation planner, low-level mutation writer, recovery
transaction phase, journal schema, mounted-target safety policy, authorization
policy or command allowlist semantics are loosened. Root-owned installed
helpers still use direct absolute paths and user-initiated `pkexec` elevation.

The native path classification is reviewed against InfiltratorOS's actual
filesystem specification. This is a source/packaging-path audit only;
it does not declare this exact head CI-qualified or imply that all OS
resources already satisfy OFS's filename limits. Complete normal and
sanitizer self-hosted CI, exact-artifact and publication gates remain
mandatory before official release; physical OFS/InfiltratorFS boot qualification
is separate.

### Native layout correction (1.8.0-275)

The real GTK executable is installed as `/Programs/defragmenter` (no redundant
shell launcher or `defragmenter-desktop` copy); Test Media is the second
program. Defragmenter's private command workers are now in
`/System/Commands/Defragmenter`, never in
`/System/Filesystems`. Both menu icon fields point to the canonical
`/System/Resources/Icons/defragmenter.png`. Raw ELF icon rendering inside
Nemo requires per-user custom icon metadata; do not claim that menu icons
change executable file icons.
