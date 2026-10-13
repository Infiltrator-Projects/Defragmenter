# Design

## First-principles position

Defragmenter starts from on-disk filesystem structures, durability requirements and the behaviour the product must own. Specifications, mature implementations and external tools are evidence to study; they are not runtime authorities for Defragmenter's placement or recovery policy.

## Goals

- fail closed when metadata, geometry, target identity or feature support is uncertain
- keep filesystem parsing, placement and recovery with the filesystem that owns those rules
- make every authoritative write either valid, unchanged or recoverable after interruption
- verify completed mutation by reopening and rescanning the target
- keep generic mechanisms in Common without exporting Defragmenter-specific policy
- bound arithmetic, allocation growth and hostile metadata traversal

## Non-goals and limits

Defragmenter is not a general filesystem repair suite, an online defragmenter or a replacement kernel filesystem driver. It does not promise write support for every legal feature combination of every supported format.

Read-only analysis may cover formats for which mutation is intentionally unavailable. Unsupported means no write.

## Language and dependency policy

C remains the default for on-disk codecs, fixed-layout structures, raw filesystem transforms and direct storage work. C++17 is used where deterministic lifetime management, stronger value types or scoped process/protocol ownership materially improve filesystem-neutral application services. C++ must not introduce inheritance or virtual dispatch without a genuine polymorphic requirement, and working C is not converted merely for language uniformity.

The current split keeps raw filesystem writers and the storage-safety core in C. C++17 owns the native application registry, JSON/protocol values, allocation-map translation, operation dispatch, bounded child-process capture, privileged helper session and installed GTK desktop. Python is deliberately outside the product implementation boundary: `tests/` may use it for independent fixture construction and black-box orchestration of native binaries, but no Python application model, presentation layer, capability registry or privilege policy is retained.

Platform libraries and in-process filesystem libraries are used when their documented contract is the stronger engineering choice. External command-line repair or defragmentation programs are not part of production mutation paths.

The reusable first-party dependency is pinned to Infiltratr Common 1.19.38 at exact commit `7070c5812b50821fd7580101cb2289a3184f6b2c`. Generic parsing, arithmetic, byte-order, path construction, allocation-growth, journal key=value mechanics, JSON escaping/decimal conversion, exact I/O, generic durable-file mechanics and neutral design/typography identity belong there when their contracts match. Defragmenter consumes those contracts directly: the native GTK desktop resolves Common's palette, typography and metrics API and Test Media uses the same native design contract rather than carrying a second visual source of truth. Filesystem geometry, journal schemas, placement, target safety, transaction/recovery policy and domain-specific allocation-map meaning remain local. Duplicate filesystem policy and durability mechanics are kept out of both presentation and test tooling.

Release construction follows the same ownership rule. Qualification owns artefact creation once; publication owns only verification and distribution of those exact files. The hardware-native installer source payload is selected from committed Git objects and the exact Common revision rather than from the ambient build directory, so an untracked runner file cannot silently become part of a qualified release.

## Failure philosophy

A destructive operation has a higher evidence bar than a read-only operation. Unknown features, impossible geometry, stale recovery state, target replacement, mounted overlap, failed staging or failed verification stop mutation rather than triggering a best-effort guess.

Stop is cooperative and is honoured only where on-disk state is valid or recoverable. A writer returning zero is not enough; the final filesystem state must satisfy the independently checked postcondition.

## Decision quality

Design changes should identify the owning layer, the safety or correctness property being changed, alternatives considered, trade-offs and the evidence that will validate the change.

A replacement earns its place by improving correctness, recoverability, performance, resilience, usability or maintainability without weakening an established safety contract. "Newer", "shorter" or "more shared" is not sufficient by itself.
