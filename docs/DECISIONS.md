<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Architectural decision record

**Filesystem ownership decision (2026-10-29):** [one authoritative `.so` per format](FILESYSTEM_ENGINE_OWNERSHIP.md). Existing internal format engines are temporary until module integration passes independent tests; they are not permanent alternatives to the same filesystem's canonical driver.

## Purpose

This document records the small set of architectural decisions that materially
shape Defragmenter. [ARCHITECTURE.md](ARCHITECTURE.md) states the current system
contracts, [DESIGN.md](DESIGN.md) states the first-principles rationale, and this
record preserves the alternatives considered, selected approach and consequences.

These decisions are intentionally few. Routine implementation choices belong in
source comments, tests or Git history rather than being promoted into permanent
architecture records.

## ADR-001 — Use direct offline userspace mutation

**Context.** The project needs deterministic physical placement, explicit
recovery boundaries and independent post-operation verification across multiple
filesystems.

**Alternatives considered.**

- invoke filesystem-specific external defragmentation/repair utilities;
- mount the filesystem and request relocation through the kernel driver;
- implement placement and metadata mutation directly in first-party userspace
  engines.

**Decision.** Write-capable engines operate on unmounted targets and own
filesystem parsing, placement, staging, metadata updates, recovery and final
verification. System libraries may be linked in-process where their semantics
are understood, but external mutation commands are not part of the production
write path.

**Consequences.** Placement is deterministic and auditable, but the write
support matrix is deliberately narrower and implementation/verification effort
is materially higher. Unsupported feature combinations must fail closed rather
than falling back to opaque external behaviour.

## ADR-002 — Treat uncertainty as a pre-write failure

**Context.** A storage tool cannot safely infer intent or metadata meaning when
geometry, feature flags, transaction state or target identity are ambiguous.

**Alternatives considered.**

- best-effort continuation with warnings;
- automatic repair or feature downgrading;
- rejection until the state is explicitly supported and validated.

**Decision.** Unknown or contradictory state rejects mutation before the
affected structure becomes authoritative.

**Consequences.** Some valid but unsupported filesystems are refused. This is
accepted in exchange for a smaller, reviewable safety envelope and a clear
interpretation of every enabled write path.

## ADR-003 — Define Growth Defrag as an exact postcondition

**Context.** A vague policy such as "leave some free space" is difficult to test,
compare across filesystems or recover deterministically.

**Alternatives considered.**

- percentage targets applied approximately at volume level;
- heuristic gaps based on file size or available space;
- an exact per-file reserve.

**Decision.** Growth Defrag requires exactly 10% of the regular file's allocated
length to remain free immediately after that file, subject only to the documented
fixed-metadata boundary rule.

**Consequences.** The operation has a precise oracle and consistent semantics
across writers, but layouts that cannot satisfy the exact reserve are rejected
instead of receiving a weaker approximation.

## ADR-004 — Persist recovery state before authoritative source mutation

**Context.** Process termination, Stop requests and host interruption can occur
between writes. Anonymous memory cannot be the only source of recovery truth
after the source filesystem may have changed.

**Alternatives considered.**

- in-memory rollback state;
- best-effort reconstruction after a crash;
- a durable, phase-checked transaction record plus verified staging/workspace
  material.

**Decision.** Before entering a phase from which authoritative writes may require
recovery, sufficient transaction state is durably published. Phase transitions
are monotonic, recovery artefacts are path-bound to the selected journal, and
cleanup occurs only after final verification.

**Consequences.** Operations incur extra I/O and temporary-storage cost, but
interruption states are constrained to unchanged, valid or explicitly
recoverable states.

## ADR-005 — Keep generic mechanics in Common and filesystem policy local

**Context.** Defragmenter shares generic primitives with other Infiltrator
projects, but filesystem safety semantics must remain reviewable in the owning
engine.

**Alternatives considered.**

- duplicate all helpers in Defragmenter;
- move broad storage policy into Common;
- share only semantics-neutral mechanics.

**Decision.** Common owns generic parsing, checked arithmetic, array growth,
byte order, escaping, path/string helpers, exact I/O, generic durable-file
publication/removal and neutral design contracts where semantics match.
Defragmenter's native core owns product-local filesystem-neutral target safety
and transaction-binding mechanics. Filesystem engines retain volume identity,
geometry, placement, transaction stages, recovery meaning and fail-closed
policy. Python may be used by independent test fixtures/drivers, but it owns no
product control plane, mutation, durability, presentation or filesystem policy.

**Consequences.** Generic code is reused without obscuring the safety boundary.
A smaller codebase is not treated as a sufficient reason to move
application-specific policy into Common.

## ADR-006 — Bind release publication to the exact qualified commit and inputs

**Context.** A green test result is not meaningful for release assurance if the
published source can differ from the tested source, if qualification can be
weakened after audit, or if a packaging step can absorb ambient runner files
that are not part of the repository revision.

**Alternatives considered.**

- mutable release branches;
- manual publication after an earlier CI run;
- archiving the current working directory with an exclusion list;
- exact-head qualification with a write-once version identity and explicit
  source-input inventory.

**Decision.** A release is eligible only from a versioned release commit whose
exact head passes the permanent quality gate and matches the active audit
baselines. The audited source baseline includes every tracked input that can
alter what qualification builds, installs or accepts, including tests and the
source-install entry point. The local native installer is constructed from
committed Git objects for that exact Defragmenter revision plus the exact pinned
Common commit; untracked workspace state is not an input. The publisher treats
each version as write-once: an existing tag must already resolve to the
qualified commit, the release must target that commit, and the exact qualified
asset set/digests must match. APT publication resolves the same release identity.
These guarantees are project-enforced and do not depend on GitHub's separate
platform-level immutable-release setting.

**Consequences.** Release administration is stricter and occasionally requires
an explicit audit-baseline advance, but there is a traceable chain from source
commit to tests, safety decision and distributed artifacts. Adding another
source input requires deliberately adding it to the audited boundary rather
than relying on an exclusion convention to keep unrelated files out.

## ADR-007 — Use C++17 for filesystem-neutral application services without rewriting strong C engines

**Context.** The raw filesystem engines are deliberately C-oriented and already
match fixed-layout data and low-level storage interfaces well. The application
layer also needs typed registry data, JSON/protocol ownership, process lifetime
management and a persistent privileged session, where scoped C++ ownership
removes cleanup and post-`fork` hazards that were awkward to express safely in
the former Python orchestration.

**Alternatives considered.**

- retain Python as a permanent production application/control layer;
- preserve a Python application model indefinitely as compatibility/test code;
- rewrite the filesystem engines into C++ for language uniformity;
- keep the filesystem/storage boundary in C and move only the
  filesystem-neutral application services to C++17.

**Decision.** C remains authoritative for raw filesystem parsing, planning,
mutation and the storage-safety core. C++17 owns the native registry, map
translation, operation dispatch, process/protocol values, privileged-helper
lifetime and installed GTK desktop. The privileged helper uses `posix_spawn`
and explicit process-group ownership. Once native parity and black-box native
regressions existed, the retired Python application/model/presentation graph was
removed rather than maintained as a second representation of product state.
Python remains permitted under `tests/` as an independent fixture and driver
language only.

**Consequences.** The project gains stronger scoped ownership without imposing
an object model on disk algorithms and no longer pays the maintenance cost of a
non-shipping parallel application. Filesystem capability, mapping, operation,
presentation and privileged-process authority all have one native source of
truth. Language choice remains evidence-driven rather than a purity rule.

## ADR-008 — Keep ZFS mutation inside the ZFS transaction engine

**Context.** Defragmenter can parse a bounded ZFS/OpenZFS subset deeply enough
to reconstruct exact allocation and regular-file physical fragmentation.
Mutation is different: relocating a ZFS block changes a copy-on-write graph
whose authoritative publication is a transaction-group operation. References
may also be retained by snapshots, clones, deduplication/block-cloning state,
indirect vdev mappings or other pool features. Defragmenter's Growth Defrag
contract additionally requires an exact 10% physical reserve immediately after
each file and requires that reserve to be a meaningful postcondition.

**Alternatives considered.**

- implement an independent raw ZFS transaction-group writer;
- invoke an external OpenZFS rewrite command as Defragmenter's production
  mutation path;
- keep ZFS analysis native and exact where provable, while leaving ZFS
  relocation to the filesystem's own transaction engine.

**Decision.** ZFS/OpenZFS is analysis-only in Defragmenter. The native reader
may advertise exact allocation/fragmentation only inside its explicitly
qualified single-disk feature subset and must fall back or fail closed whenever
the current allocator state cannot be reconstructed exactly. Defragment,
Growth Defrag and Recover are not exposed for ZFS. Defragmenter will not
implement a parallel raw ZFS transaction engine and will not weaken ADR-001 by
silently delegating its write contract to an external command.

**Consequences.** ZFS receives useful first-party physical-layout analysis
without creating a second implementation of TXG publication, snapshot/reference
accounting and pool recovery. The generic 10% Growth Defrag operation is
intentionally unavailable because ZFS's allocator owns future physical
placement, so an adjacent physical reserve is not a stable filesystem
postcondition. If a future in-process ZFS interface can provide Defragmenter's
full deterministic placement and recovery contract, this decision may be
revisited.

## Review rule

A future change should add or amend an ADR only when it changes one of these
architectural choices or introduces another decision with similarly broad,
long-lived consequences. Implementation details and historical release notes
should not accumulate here.
