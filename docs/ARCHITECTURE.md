# Architecture

Defragmenter separates GTK presentation, filesystem-neutral safety/runtime services, per-filesystem engines, recovery state and reusable Common mechanisms. That separation is a correctness boundary: the UI expresses intent and presents completed state, while raw-storage and filesystem code owns the rules that can change on-disk data.

## First-principles design

Defragmenter starts from filesystem structures, durability rules and target-safety requirements rather than treating a mounted filesystem driver or external repair utility as the specification.

First principles does not mean reimplementing every mechanism. Linux supplies raw block I/O and process primitives; GTK supplies presentation; system libraries may provide documented in-process APIs; Infiltratr Common supplies reusable first-party primitives. Defragmenter retains ownership of filesystem interpretation, placement, mutation, recovery and user-visible safety policy.

A dependency is chosen because its contract is stronger for the job, not because it is convenient. Unsupported or ambiguous layouts remain unsupported instead of being guessed.

## Structure

```text
GTK 3 presentation
        ↓
Python coordinators / service models (migration boundary)
        ↓
C++17 application services
registry / mapper / operation dispatcher / privileged session
        ↓
filesystem worker protocol
        ↓
filesystem-neutral native C safety/runtime core
        ↓
per-filesystem native C analysers / planners / writers
        ↓
raw image or unmounted block device

Infiltratr Common 1.19.10
        ↓
checked arithmetic / parsing / endian / path / exact-I/O /
allocation-growth / JSON / durable-file primitives
```

The source tree reflects those responsibilities:

```text
defragger/
├── gui/ui/                 presentation and user interaction
├── gui/core/               shared application protocol/data contracts
├── gui/engine/             worker resolution, orchestration and read-only compatibility I/O
├── gui/backends/           plugin contracts and single registry
├── gui/filesystems/        authoritative per-filesystem implementations
├── native/                 C++17 application services and protocol ownership
├── src/core/               filesystem-neutral native C safety/runtime services
├── test_media/             destructive sacrificial-media utility
├── tests/                  native, filesystem, GUI, safety and release evidence
├── packaging/              Debian/native installer construction
└── shared/                 pinned Infiltratr Common dependency
```

## Contracts and ownership

`native/runtime.cpp` is the native application registry used by the production C++ mapper and operation dispatcher. The existing Python registry remains only at the staged GTK/plugin compatibility boundary while that layer is migrated; automated manifest and real-fixture parity checks prevent the two representations from silently diverging. Each filesystem package still owns its probe, analyser, placement rules, writer and verifier, and native C below a filesystem package remains an implementation detail rather than a second filesystem engine.

`src/core/` owns mechanics that are genuinely filesystem-neutral: exact raw I/O adaptation, target identity/capacity checks, overlap-aware mounted-target rejection, Stop state, resource defaults and machine-readable result emission. Filesystem geometry, metadata interpretation, transaction stages and recovery rules stay with the owning filesystem.

GTK objects stay in the presentation layer. Runner, policy and storage models exchange plain values and typed events rather than making presentation code responsible for raw-device or filesystem policy.

## Native language boundary

C and C++ are first-class implementation languages with different jobs. C remains the strongest fit for filesystem structures, codecs, raw parsers, planners, writers and the storage-safety core because those components map directly to fixed-layout data, kernel interfaces and explicit byte-oriented algorithms.

C++17 is used where scoped ownership and stronger value types materially improve filesystem-neutral application work. The native registry, JSON/protocol model, allocation-map translation, operation dispatcher, bounded child-process capture and privileged helper session live in `defragger/native/`. Those native binaries are the only mapper/operation-dispatch/privileged-helper control plane installed with the application; the older Python dispatcher sources remain in-tree only for migration/parity regression evidence. The privileged helper uses `posix_spawn` rather than post-`fork` C++ work and owns its child process group explicitly.

The NTFS plan database also uses narrow, non-inheriting RAII owners for SQLite statements, transaction rollback and OpenSSL digest state behind the existing C-facing filesystem implementation. None of these uses creates a class hierarchy around the raw filesystem engines. Working C is not rewritten merely because C++ is available, and C++ is not avoided where it gives a stronger ownership model.

## Target safety and privilege boundary

Write-capable operations target only an unmounted block device or regular filesystem image. Selection is not treated as authority: the project revalidates target identity, capacity and mounted overlap across open/privilege boundaries before authoritative mutation.

The privileged helper accepts a constrained command contract and a user-specific recovery namespace. It launches fixed commands with `posix_spawn` into a dedicated process group, ignores SIGPIPE in the supervisor, treats a closed GUI protocol pipe as a transport failure, requests the writer's cooperative SIGINT path and waits for the child to exit before the helper can terminate. Filesystem workers still perform their own target and format validation; privilege does not bypass safety policy.

Every GUI mutation, including a user-owned image and Recover, uses that privileged journal namespace. Child stdin is isolated from the GUI protocol. Stop waits for writer initialisation but can interrupt silent read-only analysis immediately. Completion is emitted only after the child is reaped and the session can accept the next request.

The mapper probes FAT geometry when discovery supplies only a generic FAT name. Its map-output allowance scales with the requested cell count and remains bounded by the GUI maximum; ordinary child-command capture retains its separate default limit. Mounted-image checks compare device and inode identity so hard-link aliases do not bypass refusal.

Paths and device-provided metadata are external input. A previously valid path may refer to a different object later, so persistent transactions bind to stable target and filesystem identity where the format exposes it.

## Filesystem engine contract

Read-only plugins may probe and map a filesystem without implementing mutation. A write-capable plugin additionally owns a first-party native mutation path and an explicit Recover contract.

Production writers do not mount the target, ask a mounted filesystem to choose placement, or launch external repair/defragmentation utilities. In-process system libraries are permitted where their documented API is part of the chosen implementation, but Defragmenter remains responsible for placement, transaction, verification and failure policy.

Workers communicate through typed phase/live-range/result records. Human-readable logs are diagnostic text, not an API.

## Transaction and recovery contract

A write operation follows one architecture-wide sequence:

1. identify the filesystem and declared operation;
2. revalidate target identity and mounted state;
3. scan the complete source model and reject unsupported states;
4. construct and validate the canonical target plan;
5. establish persistent recovery material before source bytes can require recovery;
6. perform bounded durable mutation at filesystem-safe transaction boundaries;
7. honour Stop only at a valid or recoverable boundary;
8. reopen the target read-only and verify the required postcondition before success.

Recovery state is monotonic and tied to one target. An unfinished transaction cannot be silently replaced by unrelated state. A successful writer return is not sufficient evidence of success without the final verification pass.

## Failure model

Failure is explicit. Unknown features, contradictory geometry, malformed metadata, target replacement, missing recovery material, unavailable dependencies and failed verification reject the affected operation.

A numeric or apparently valid result is never substituted merely to keep an operation moving. For destructive paths, uncertainty is a reason not to write.

The project assumes the kernel, libc, required libraries and storage hardware honour their documented contracts. It does not claim recovery from a compromised root environment or hardware that falsely acknowledges persistence and later loses data.

## Common

`shared/infiltratr-common` is pinned to Infiltratr Common 1.19.10 at exact commit `33e69c0a462b56d388881d89c4eb49f72fa0b0fe`.

Common is authoritative for reusable mechanisms whose semantics are genuinely generic. If Defragmenter has a stronger implementation of a generic primitive, the preferred direction is to improve Common until its contract preserves that correctness, performance and resilience, then remove the local duplicate.

For this pin, Common also owns deterministic finite-decimal conversion used by the C++ JSON adapter, checked allocation sizing used by local runtime wrappers, allocation-free key=value line parsing for native journals/manifests, POSIX lexical path joining/concatenation, native typography/structural design identity, immutable MB Corpo asset provenance and the complete layered Linux MBLINK Day/Night semantic role set. The GTK adapter is generated from Common's design JSON and consumes matching titlebar/connection/heading/summary/detail/note/state roles; Test Media consumes the same native design API directly. Defragmenter's no-font-fallback policy remains product-local because the package installs the verified Common-described faces itself. Python compatibility analysis remains read-only; generic durable publication/removal and exact write I/O are not duplicated there because authoritative mutation is native.

Do not move filesystem policy, target-safety decisions or transaction semantics into Common merely to reduce line count.

## Verification and assurance

Correctness is enforced at several levels: warnings-as-errors native builds, unit and parser tests, disposable filesystem-image tests, recovery/fault-injection tests, GUI/service regressions, architecture/safety checks, ASan/UBSan qualification, packaging tests and exact-head release gates.

The exact release revision must satisfy the project quality gate. The current write-safety decision is recorded separately in [AUDIT_STATUS.md](AUDIT_STATUS.md), while [VALIDATION.md](VALIDATION.md) defines what each class of evidence proves and does not prove.

## Build and release contract

Release artifacts are built from the exact qualified `main` revision. Published tags and assets are immutable identities. The Debian package/native installer, Common pin, audit source baseline and release-governance baseline are checked as part of publication rather than treated as post-release bookkeeping.

Publication is part of the quality-gate dependency graph rather than an event-only afterthought: the release reusable workflow depends directly on both the local qualification and hosted sanitizer jobs, and the APT refresh reusable workflow depends directly on successful immutable release publication. This preserves the dependency across GitHub job retries, while exact-SHA checks and the explicit `Release <version>` commit requirement prevent an unrelated successful run from publishing artifacts.

## Specialist documents

- [Design](DESIGN.md) — first-principles goals, non-goals, trade-offs and failure philosophy.
- [Decisions](DECISIONS.md) — durable architectural choices and consequences.
- [Validation](VALIDATION.md) — evidence classes, destructive-path validation and release criteria.
- [Audit status](AUDIT_STATUS.md) — current write-safety case and exact audited baselines.
- [References](REFERENCES.md) — filesystem/platform specifications and engineering sources.
