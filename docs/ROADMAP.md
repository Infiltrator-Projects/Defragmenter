# Roadmap

This document defines the Linux feature-completion target. The released source and tests remain authoritative for what is supported today; unchecked items are not release claims.

## Current foundation

- [x] Broad first-party native allocation analysis across the currently registered filesystem families.
- [x] Qualified Defragment, Growth Defrag and Recover engines for FAT12/16/32, exFAT, NTFS, ext2/3/4, XFS v5, Amiga OFS/FFS, Amiga SFS0/SFS2, bounded Amiga PFS3, bounded Classic HFS, bounded single-device Btrfs, bounded APFS, HFS+/HFSX, Minix v1/v2/v3 and the qualified UFS1/UFS2 subset.
- [x] Exact target confirmation, descriptor-level identity binding, mounted-overlap refusal, durable recovery state, cooperative Stop and final read-only verification.
- [x] Full filesystem, GUI, architecture, safety, packaging and release regression gate.
- [x] Exact pinned Common dependency with filesystem-neutral mechanisms kept out of filesystem engines.

## Linux feature-completion backlog

A filesystem writer is complete only when Defragment, exact 10% Growth Defrag and Recover share a validated placement model, reject unsupported source state before authoritative writes, retain durable recovery material across interruption and independently verify the final image.

- [x] **Amiga SFS2** — exact native allocation/fragmentation analysis, SFS2 48-bit file-size and 32-bit extent qualification, offline native relayout, exact 10% Growth Defrag and Recover.
- [x] **Amiga PFS3** — first-party identification, exact allocation/anode-chain fragmentation analysis for the qualified small-disk subset, bounded offline native relayout, exact 10% Growth Defrag, Recover and a deterministic first-party Test Media creator.
- [x] **Classic Macintosh HFS** — exact native analyser plus bounded recoverable native Defragment/Growth Defrag/Recover support for clean volumes with complete inline regular-file fork extent maps; unsupported overflow-backed regular-file forks fail closed.
- [x] **Btrfs** — exact raw allocation/fragmentation analysis plus a fail-closed recoverable offline Defragment/Growth Defrag/Recover contract for the qualified single-device CRC32C, level-0 mixed-group subset.
- [x] **APFS** — exact spaceman-backed allocation/fragmentation analysis, bounded recoverable offline Defragment/Growth Defrag/Recover for the single-active-checkpoint/direct-CIB/flat-tree subset, exact 10% reserve qualification and a deterministic first-party raw C Test Media creator/verifier.
- [x] **Minix v1/v2/v3** — exact native analyser plus recoverable native Defragment/Growth Defrag/Recover support, including exact 10% reserve qualification.
- [x] **UFS1/UFS2** — exact cylinder-group allocation mapping and inode block-tree fragmentation analysis for both variants, with a bounded clean/non-journalled/snapshot-free native Defragment/Growth Defrag/Recover contract, exact 10% reserve qualification, durable staged recovery and genuine makefs UFS1/UFS2 integration coverage.
- [ ] **ZFS/OpenZFS member** — replace summary mapping with exact allocation/fragmentation analysis for a bounded supported on-disk feature set, then add a recoverable offline writer contract only where pool/member semantics make deterministic safe mutation provable.
- [x] **GTK compatibility migration** — native mapper/operation-dispatch/privileged-helper services are the sole control plane; GTK consumes the native manifest directly, and the source-only Python registry, filesystem declarations and compatibility dispatchers have been removed so Python is presentation/glue only.
- [x] **Test Media completeness** — every filesystem slot now has an implemented creator path; APFS uses the first-party raw C bounded fixture/verifier rather than a manual reserved partition.

Linux swap is complete by design as analysis-only: defragmentation, Growth Defrag and Recover are not meaningful operations for swap and are therefore not backlog items.

## Performance and qualification

Performance work may proceed alongside feature completion, but it must not weaken deterministic placement, resource bounds or recovery semantics. New writers must gain malformed-media tests, successful disposable-image tests, independent post-operation verification and interruption/recovery qualification before release.

The opt-in dm-log-writes/replay-log lane remains environment-dependent evidence for destructive durability boundaries and does not replace filesystem-specific transaction tests.

## Admission rule

A proposed capability enters the supported matrix only when its ownership is clear and there is a credible validation path. Features that require pretending uncertain on-disk behaviour is known do not qualify.

## Completion rule

The Linux target is feature-complete when every item above is either checked with implementation/tests/user-visible behaviour/documentation in agreement, or explicitly removed from product scope by a documented design decision. A checkbox or release number cannot substitute for missing evidence.
