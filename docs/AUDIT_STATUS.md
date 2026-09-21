# Safety audit status

Status: **complete**

Completed: 2026-08-25  
Extended: 2026-09-21

Applies to: release version 1.8.0-192
Audited source commit: 81576b9fa40bfa6423c41bbb9ddd352b725368b9
Audited release-governance commit: 4ce08b40e91e47d8ae60450eec86a2840e97232e

Audited writer IDs: fat12, fat16, fat32, exfat, ntfs, ext4, xfs, affs, apfs, btrfs, pfs3, sfs, hfs, hfsplus, minix

This document records the **current** write-safety case. Historical audit-development detail remains available in Git history and immutable release tags rather than being repeated here.

## Current safety case

The enabled writers share the following release requirements:

| Risk | Required contract | Principal evidence | Remaining limit |
| --- | --- | --- | --- |
| Wrong target or mounted overlap | exact target confirmation, descriptor identity/capacity checks and mounted-overlap refusal, including hard-link aliases of mounted images | native mountinfo/loop-backing identity regressions, safety tests and filesystem worker checks | a compromised privileged OS is outside the model |
| Unsupported or corrupt metadata | complete pre-write validation and fail-closed feature checks | negative fixtures, filesystem-native tests and the cross-worker deterministic malformed-media matrix | untested feature combinations remain unsupported |
| Interrupted mutation | durable filesystem-specific transaction state before recovery can be required | transaction/fault-injection and Recover suites | hardware that lies about persistence is outside the model |
| Unsafe Stop | cooperative Stop only at unchanged, valid or recoverable boundaries | worker/transaction Stop regressions | Stop is not an arbitrary mid-write abort |
| Privileged supervisor loss | closed control output cannot abandon a root writer; the native helper owns a process group, detects transport failure, requests cooperative SIGINT and waits for exit | native live-child tests verify Stop, queued Stop, control EOF, broken output, delayed safe completion and child reaping; real worker safety tests remain separate | controlled-child tests establish supervision, not physical-media crash safety |
| Silent payload/layout damage | reopened read-only verification before success | disposable-image verification and payload/layout checks | shared parser assumptions can still create common-mode risk |
| Publication from unaudited source | exact source/governance baselines plus exact-head release gates | `tests/test_release_gate.py` and GitHub workflows | protects project publication, not downstream repackaging |
| Destructive Test Media misuse | separate utility, system-disk refusal and repeated confirmation | Test Media safety tests | an explicitly selected sacrificial disk can still be erased |

The validation method and its limits are defined in [VALIDATION.md](VALIDATION.md).

## Audited write scope

The completed audit covers these first-party write/recovery engines:

- **FAT12/FAT16/FAT32** — native direct analysis, canonical relocation, exact Growth Defrag reserve and Recover.
- **exFAT** — native catalogue/relayout, exact Growth Defrag reserve and Recover.
- **NTFS** — native fail-closed preflight, canonical supported-subset relocation, exact Growth Defrag reserve and Recover. Plan-database/digest resource ownership uses a narrow C++17 RAII unit behind the existing C ABI; raw planning, relocation and worker control remain C.
- **EXT2/EXT3/EXT4** — native staged transaction using the linked libext2fs API in-process, followed by verification and Recover.
- **XFS** — native raw userspace catalogue, planning, metadata rewrite, verification and Recover for the explicitly supported v5 contract.
- **Btrfs** — exact first-party raw analysis plus a bounded offline Defragment, exact 10% Growth Defrag and durable Recover contract for a single-device CRC32C subset. The writer requires level-0 mutable roots in one unprofiled mixed data/metadata block group, skinny metadata, NODATASUM unencoded full regular-file extents with single inline references, no log root/snapshots/qgroups/device-replace/balance state, and no unsupported feature bits; everything else fails closed.
- **Amiga OFS/FFS** — native raw catalogue, relocation, verification and Recover.
- **Amiga SFS0/SFS2** — first-party native supported-subset relayout and Recover. SFS2 structure version 4 uses its native 48-bit file-size object field and 32-bit extent block counts rather than truncating them to SFS0 geometry.
- **Amiga PFS3** — first-party native exact allocation/anode-chain analysis, bounded offline canonical relayout, exact 10% Growth Defrag and durable Recover for the qualified small-disk subset. The writer rejects superindex/large-file mode, nested directories, links and special entries before authoritative writes.
- **Classic Macintosh HFS** — exact native allocation/catalog analysis plus bounded offline Defragment, exact 10% Growth Defrag and durable Recover. Mutation is restricted to clean, writable classic-HFS volumes whose regular-file data/resource fork extent maps are complete in their three inline catalog descriptors; Extents Overflow-backed regular-file forks fail closed.
- **HFS+/HFSX** — native staged transaction and Recover for supported clean-journal states.
- **Minix v1/v2/v3** — native fail-closed staged relayout, exact 10% Growth Defrag and durable Recover for the supported exact-analysis subset.

- **APFS** — exact native checkpoint/spaceman allocation and catalog-fragmentation analysis plus bounded offline Defragment, exact 10% Growth Defrag and durable Recover. Mutation is restricted to a single active two-object checkpoint with no historical descriptor-ring objects, one direct CIB/allocation bitmap, no internal-pool or pending free-queue state, one unencrypted/unsealed/snapshot-free volume, current-XID flat catalog/extent-reference roots, and plain unshared non-sparse regular-file extents. Unsupported APFS states fail closed before authoritative writes.

Read-only support for other formats is not promoted to write support by this audit. Unsupported or ambiguous layouts continue to fail closed.

## Safety invariants

The audited writers are expected to preserve these invariants:

1. writes remain bound to the selected target and expected capacity;
2. mounted or overlapping raw targets are refused;
3. no metadata rewrite proceeds from an unvalidated catalogue or plan;
4. recovery material is durable before source bytes can require recovery;
5. transaction state cannot silently regress or switch targets;
6. Stop is observed only at a valid/recoverable boundary;
7. success requires reopened verification of payload and layout;
8. unsupported or structurally unsafe states do not write;
9. loss of the GUI/helper protocol cannot detach an active privileged writer from supervisor shutdown.

Architecture ownership is defined in [ARCHITECTURE.md](ARCHITECTURE.md); design rationale is in [DESIGN.md](DESIGN.md).

## Dependency baseline

The audited production tree consumes Infiltratr Common 1.19.10 at exact commit `33e69c0a462b56d388881d89c4eb49f72fa0b0fe`. CMake, the native installer, submodule identity and release regressions verify the same released pin.

Common owns reusable mechanisms such as checked arithmetic, strict and locale-independent numeric parsing, endian access, bounded growth, path/string helpers, JSON escaping, exact I/O, generic durable file publication/removal, neutral design metrics/typography identity and canonical MB Corpo asset provenance. Common 1.19.10 also owns the complete layered Linux MBLINK Day/Night semantic roles consumed by Defragmenter's generated GTK adapter and Test Media native theme. Defragmenter retains filesystem structure, target-safety, placement, transaction/recovery semantics, domain-specific allocation-map meaning and user-facing failure policy.

## Release controls and decision

The repository uses a `direct-main` development model. The active main ruleset provides the permanent history protections checked by release automation; publication itself is guarded by the exact-head **Project quality gate**, exact audit baselines and immutable tag/release checks.

The quality gate now owns the release handoff as a direct dependent reusable-workflow job. Its primary full-suite lane and sanitizer lane run on GitHub-hosted Linux, so release qualification does not depend on an intermittently available home runner. The separate self-hosted `local-quality.yml` workflow remains an optional manual heavy-qualification lane and does not gate publication. Publication remains attached to the same qualified hosted run: if a failed quality job is rerun successfully, GitHub reruns its dependent publication job instead of relying on a separate `workflow_run` event that may never be emitted for the retry. The called release workflow still verifies the `protected-main` history rules, the exact tested `main` SHA, the audited production baseline and the audited release-governance baseline before publication. A release still requires an **explicit release decision** represented by a `Release <version>` commit whose exact head passes both hosted quality lanes.

APT publication is likewise a direct reusable-workflow dependency of successful release publication, with manual `workflow_dispatch` retained only as a retry path. It verifies the immutable release tag/SHA before dispatching the central repository refresh, so release or APT retries cannot lose their handoff through `workflow_run` semantics. A release-head retry may contain documentation or release metadata only after the audited source commit; the exact-head gate still rejects any production/build/package drift.

Version 1.8.0-192 is the current audited release line. It retains the exact Common 1.19.10 pin and all previously qualified writers while removing the duplicate Python filesystem control plane, making the native C++ manifest the sole capability/operation authority, sharing only filesystem-neutral target-binding mechanics across writers, and splitting NTFS persistent journal representation/durability from its filesystem-specific placement and recovery policy. UFS mutation remains deliberately fail-closed in the installed worker until its roadmap qualification is complete.

The 1.8.0-192 audit extension covers the production/control-plane consolidation and restored executable packaging/test contracts through `81576b9fa40bfa6423c41bbb9ddd352b725368b9`. The native registry/mapper/operation engine are now the only filesystem capability and dispatch authority; source-only Python filesystem registries/plugins and duplicate dispatcher/helper paths have been removed. Transaction target identity/capacity binding is shared in the native core while filesystem-specific volume identity, phase meaning, placement ordering and Recover semantics remain local. NTFS persistent journal parsing/publication/cleanup is isolated in its transaction component without moving NTFS placement or recovery policy out of the worker. The hosted sanitizer build/tests passed after the NTFS split boundary fixes, and the exact release head must still pass both permanent hosted quality lanes before publication.

The audited source baseline also strengthens the root-owned trusted-directory walk: supported Linux kernels use `openat2()` with `RESOLVE_BENEATH`, `RESOLVE_NO_SYMLINKS` and `RESOLVE_NO_MAGICLINKS`; older kernels retain the previously audited `openat(O_NOFOLLOW)` component-by-component fallback and the same ownership/mode validation. The obsolete duplicate base-window About implementation has been removed, leaving the LINK-standard presenter as the single concrete About/licence/icon path.

Parser qualification now adds a deterministic malformed-media matrix across every installed native filesystem worker, rejecting hangs, signal termination and accidental identification of empty, truncated, all-ones or seeded-noise media. An opt-in `dm-log-writes`/`replay-log` harness is present for sacrificial block-layer FLUSH/FUA replay, but it is intentionally classified as environment-dependent evidence until run on suitable disposable devices; it does not replace the existing transaction/recovery fault-injection suite.

The 2026-09-20 audit extensions add Minix, SFS2 and the bounded PFS3 writer/Test Media path to the 1.8.0-191 source baseline. Any later change beneath audited production/build/package paths requires a new exact source audit baseline before release.

## Historical record

Earlier audit extensions documented individual implementation fixes, branding changes, Common migrations and release-pipeline corrections inline. Those details are preserved in Git history and release tags. Keeping them out of this current-state safety case prevents historical narrative from becoming a second changelog.


The 2026-09-20 audit extension additionally covers the Minix v1/v2/v3 writer introduced at `b8b979dd885dbb082815dad7005f4132c7b2b40c`. Hosted qualification completed with the native Minix unit suite, an end-to-end transaction fixture, the full 41-test CTest suite and ASan/UBSan lane. The writer remains fail-closed to the exact native subset accepted by its analyser and requires durable staged recovery material before authoritative source replacement.


The 2026-09-20 SFS2 audit extension covers the SFS2-capable native SFS engine at `82412471f25e1a8761374069c22ebfc662e00bdd`. Qualification includes deterministic SFS2 structure-version-4 analysis, Defragment, exact 10% Growth Defrag and interruption/Recover paths, plus an independent sparse large-file fixture that forces a file above 4 GiB and an extent above 65,535 blocks so the SFS2-only 48-bit file-size and 32-bit extent fields are exercised rather than inferred from SFS0 behaviour. The writer remains fail-closed to the validated object-container, extent-tree, bitmap and transaction structures accepted by the native analyser.


The 2026-09-20 PFS3 audit extension covers the bounded native PFS3 engine and first-party Test Media creator qualified at `0c330deca961669d30b1985ef3063fd526ca8ac6`. The supported writer contract is deliberately narrower than the full PFS3 format: 512-byte logical sectors, 1 KiB reserved blocks, split anodes, small-disk index mode and regular files directly in the root directory. Superindex/large-file mode, nested directories, links and special entries fail closed before mutation. Qualification covers exact bitmap-index/allocation-bitmap decoding, anode-chain fragmentation analysis, production Defragment, exact 10% Growth Defrag, durable interruption/Recover, deterministic malformed-media rejection, and a first-party 1 GiB Test Media image carrying a deterministic 25 MiB file in 100 extents with payload-corruption detection. The complete 43-test CTest suite and hosted ASan/UBSan lane passed before this audit baseline was advanced.


The 2026-09-20 Classic HFS audit extension covers the bounded native writer at `cff4c684019dbec0492639b067c3278b2f94a5d1`. The writer and existing exact analyser share one parser source for the MDB, allocation bitmap, Extents Overflow B-tree and Catalog B-tree. Mutation requires the volume to be recorded as cleanly unmounted and not hardware/software locked, rejects HFS wrappers containing embedded HFS+/HFSX, preserves the special files and B-tree topology in place, and relocates only regular-file data/resource forks whose complete maps fit in the three inline catalog extents. Qualification exercises deterministic fragmented data-fork payloads, canonical Defragment, exact 10% Growth Defrag reserve, durable interruption/Recover, clean-volume rejection and final payload/layout verification. The complete 43-test CTest gate and hosted ASan/UBSan lane passed before this audit baseline was advanced.


The 2026-09-20 Btrfs audit extension covers the bounded native writer qualified at `e24d3eec8422a1e304d67dddc7b73e36ac8af816`. The writer consumes the same native chunk/root/extent/filesystem-tree model as the exact analyser, validates CRC32C on every mutable tree root, stages the complete filesystem prefix before source mutation, relocates only unshared NODATASUM regular-file extents, rewrites the corresponding filesystem/extent-tree records with updated generations and CRC32C, invalidates stale free-space-cache state and publishes updated superblock mirrors. It rejects multi-device/striped profiles, non-level-0 mutable roots, snapshots/subvolumes, qgroups, active log/balance/device-replace state, encoded/checksummed/shared/sparse extents and unsupported feature bits before authoritative writes. Qualification uses an independently manufactured checksummed Btrfs fixture with deliberately fragmented file extents; it exercises Defragment, exact 10% Growth Defrag, durable interruption/Recover and fail-closed encoded-state rejection. The complete 43-test CTest suite and hosted ASan/UBSan lane passed before this audit baseline was advanced.


The 2026-09-20 APFS analysis extension is qualified at `e0f9fdc0f290f5c2d2763a537345e9ba4e53453a`. An independently constructed Fletcher-valid fixture contains an active checkpoint map, ephemeral spaceman object, direct chunk-info block and allocation bitmap, container and volume object maps, one unencrypted APFS volume, a flat catalog and deliberately fragmented regular-file extents. The native analyser proves exact free/used accounting from spaceman, identifies the fragmented file from catalog extent continuity, rejects a bitmap/catalog disagreement, rejects multiple-volume input outside the bounded contract and rejects sparse extents. Both the hosted ASan/UBSan lane and the complete 44-test CTest suite passed; the remaining local gate failure at that source revision was solely the expected audit-baseline drift that this document advances.


The 2026-09-21 APFS writer/Test Media audit extension is qualified at `9b6afeae4b1458419b3fb87544613731436f97f5`. The writer stages the complete bounded APFS filesystem before source mutation, binds the source path/identity/capacity and complete source digest, relocates only regular-file blocks whose catalog records have one matching physical extent reference with refcount one, rewrites the catalog and extent-reference roots, updates the direct spaceman allocation bitmap without changing allocation cardinality, recomputes Fletcher checksums, and independently reopens the staged and committed image through the production analyser. Growth Defrag reserves exactly ceil(file_blocks × 10%) free blocks immediately after each supported file. Stop during authoritative replay retains the durable journal and verified stage for Recover.

Qualification uses both an independently manufactured Python image and the first-party all-C Test Media creator. The fixture contains a single active checkpoint, direct spaceman CIB/bitmap, flat object maps, one unencrypted volume, flat catalog/extent-reference roots and a deliberately fragmented two-block regular file. Permanent regressions exercise Defragment, exact 10% Growth Defrag, payload preservation, Recover after an interrupted source-open boundary, refusal of a stale checkpoint ring, refusal of a shared physical extent and corruption detection in Test Media verification. The hosted ASan/UBSan lane passed with the complete sanitizer-visible APFS and Test Media suites at this source baseline. The self-hosted local quality lane may be rerun independently when that runner is available; release publication is qualified by the permanent hosted full-suite and sanitizer lanes.
