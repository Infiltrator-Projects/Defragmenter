# Safety audit status

Status: **complete**

Completed: 2026-08-25  
Extended: 2026-09-20

Applies to: release version 1.8.0-191
Audited source commit: c57d05eb7e1581a24edeee5cab8c873ff3224777
Audited release-governance commit: b52008be6aa7f19dae2190ddbdd22c7e946849a7

Audited writer IDs: fat12, fat16, fat32, exfat, ntfs, ext4, xfs, affs, sfs, hfsplus

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
- **Amiga OFS/FFS** — native raw catalogue, relocation, verification and Recover.
- **Amiga SFS0** — first-party native supported-subset relayout and Recover.
- **HFS+/HFSX** — native staged transaction and Recover for supported clean-journal states.

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

The quality gate now owns the release handoff as a direct dependent reusable-workflow job. Publication therefore remains attached to the same qualified run: if a failed quality job is rerun successfully, GitHub reruns its dependent publication job instead of relying on a separate `workflow_run` event that may never be emitted for the retry. The called release workflow still verifies the `protected-main` history rules, the exact tested `main` SHA, the audited production baseline and the audited release-governance baseline before publication. A release still requires an **explicit release decision** represented by a `Release <version>` commit whose exact head passes both quality lanes.

APT publication is likewise a direct reusable-workflow dependency of successful release publication, with manual `workflow_dispatch` retained only as a retry path. It verifies the immutable release tag/SHA before dispatching the central repository refresh, so release or APT retries cannot lose their handoff through `workflow_run` semantics. A release-head retry may contain documentation or release metadata only after the audited source commit; the exact-head gate still rejects any production/build/package drift.

Version 1.8.0-191 is the current audited release line. It retains the exact Common 1.19.10 pin, the retry-safe publication workflow, the GTK/GLib/X11 identity repair and complete PNG structural validation, while standardising the final graphite/cyan artwork on the canonical Infiltrator desktop colour family.

The audited source baseline at `ee3b3d68490b74c186ffea18d0e751b838fb57b0` also strengthens the root-owned trusted-directory walk: supported Linux kernels use `openat2()` with `RESOLVE_BENEATH`, `RESOLVE_NO_SYMLINKS` and `RESOLVE_NO_MAGICLINKS`; older kernels retain the previously audited `openat(O_NOFOLLOW)` component-by-component fallback and the same ownership/mode validation. The obsolete duplicate base-window About implementation has been removed, leaving the LINK-standard presenter as the single concrete About/licence/icon path.

Parser qualification now adds a deterministic malformed-media matrix across every installed native filesystem worker, rejecting hangs, signal termination and accidental identification of empty, truncated, all-ones or seeded-noise media. An opt-in `dm-log-writes`/`replay-log` harness is present for sacrificial block-layer FLUSH/FUA replay, but it is intentionally classified as environment-dependent evidence until run on suitable disposable devices; it does not replace the existing transaction/recovery fault-injection suite.

No filesystem placement algorithm, on-disk mutation format, transaction state machine or recovery semantic changed in 1.8.0-191. Any later change beneath audited production/build/package paths requires a new source audit baseline before release.

## Historical record

Earlier audit extensions documented individual implementation fixes, branding changes, Common migrations and release-pipeline corrections inline. Those details are preserved in Git history and release tags. Keeping them out of this current-state safety case prevents historical narrative from becoming a second changelog.
