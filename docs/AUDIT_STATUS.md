# Safety audit status

**Status of the recorded safety audit: complete.** This statement describes the **specific audited baselines below**, not unconditional approval of every subsequent `main` commit or a claim of physical-media qualification.

Status: **complete**

Audited source commit: 7bccc8fd5e49d1246a5de0b77fda4bc9a073984f
Audited release-governance commit: c1216fc232225bf082b2ff0fdaaeca50b360c5f7

These exact lines are an **executable release-gate contract** used by `defragger/ci/release.sh` and `defragger/tests/test_release_gate.py`. Do not remove, re-label or advance them without the corresponding independently reviewed source/governance audit. The code gate separately checks ancestry and the exact tracked source and governance diff.

## Qualification distinction (reviewed 2026-11-02)

The reviewed main source at `681287cb3875e58cb31d1f63da11b30809aacc2f` was **seven commits ahead** of the source-audit baseline. Those changes include EXT worker/identity/registry integration, pinned EXT module revisions, test changes and qualification workflow changes. This documentation pass **does not audit or approve** those executable/governance deltas.

The quality gate for that exact head ([run 41302016151](https://github.com/Infiltrator-Projects/Defragmenter/actions/runs/41302016151)) completed its ASan/UBSan lane successfully, but failed **Enforce release invariants** with `qualified source/test/install input changed after the audited source baseline`. Fourteen independent filesystem integration workflows passed for that head. This is a real release-admission blocker pending a separate audit, not a filesystem-install path discrepancy and not a Markdown syntax defect.

Earlier audit evidence includes [run 41190013223](https://github.com/Infiltrator-Projects/Defragmenter/actions/runs/41190013223), whose 42 general tests, 16 full-corpus cases and independent sanitizer checks passed before its previous source-baseline mismatch was re-audited. A passing ancestor does not replace qualification of a new head.

## Safety invariants

- Every new write requires qualified native filesystem analysis, exact format/geometry, stable device identity and capacity, mounted-overlap refusal and a supported plan.
- Unsupported features, inconsistent metadata, unknown target identity or a required missing/untrusted/ABI-incompatible `.so` cause **pre-write refusal**. No private-engine fallback.
- Recovery material and verified staging are persisted before authoritative source mutation can require rollback; transaction phases are checked and Stop is honoured at safe/recoverable boundaries.
- Recover can be admitted from a valid pre-existing user-bound journal even when fresh source analysis is impossible. The journal, target identity, staged bytes and phase are validated by the actual native worker.
- Final success requires reopening the result and checking canonical placement, exact Growth reserve where available, structural integrity and complete logical payload identity.
- Filesystem-neutral Linux topology checks distinguish partition parents, holders and whole/stacked-device slaves; hard-link aliases cannot bypass image mount checks.

These invariants apply across the bounded FAT12/16/32, exFAT, NTFS, EXT2/3/4, XFS v5, Amiga OFS/FFS/SFS/PFS3, HFS/HFS+, Btrfs, APFS, Minix and UFS variants wherever write operations are admitted. Special format states not explicitly qualified fail closed. **ZFS/OpenZFS and swap remain analysis-only.**

## Filesystem module and Test Media boundaries

The intended single-owner design is documented in [Filesystem Engine Ownership](FILESYSTEM_ENGINE_OWNERSHIP.md). Migrated SO-backed production workers must execute the actual external module with trusted path/version/ABI checks; test environments use isolated SO roots. FAT and OFS Test Media formatting/population are module-owned. Migration may not quietly retain a second production formatter, writer or journal engine.

The 21 Test Media slots must keep their independent first-party C creator/raw verifier, deterministic 200 MiB plus boundary-file corpus, schema 3 disk-identity binding, full-byte verification and corruption refusal. A successfully formatted image or a saved results file alone is not evidence of correctness.

## Release and physical assurance

Development uses direct-to-protected `main`. A `defragger/VERSION` change requests a release; ordinary documentation updates do not. Both permanent self-hosted quality/sanitizer lanes, exact source/governance baseline comparisons and protected linear history must qualify the same intended commit. Publication uses only its once-built SHA-bound `.deb`, `.run` and manifest; version identity and digests are write-once. Public APT propagation is observed separately.

Synthetic, sanitizer and dm-log-writes tests are important evidence but are **not** physical-media power-loss proof. No completed source audit, published release or passing test alone claims universal on-disk write safety.

## Historical audit trail

The original 47 KB chronological audit diary, reviewed intermediate SHAs and release-by-release notes remain available at [v1.8.0-281/docs/AUDIT_STATUS.md](https://github.com/Infiltrator-Projects/Defragmenter/blob/v1.8.0-281/docs/AUDIT_STATUS.md), with the exact immutable history in Git. This maintained document contains the active audit identities, current known qualification gap and durable safety contract, not repeated superseded baseline announcements.
