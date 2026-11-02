# Architecture

## System boundaries

Defragmenter is a **native C/C++ application**. GTK presents operations and completed evidence; filesystem-neutral services enforce identity, privilege, I/O and cancellation boundaries; the selected filesystem engine supplies format-specific interpretation and mutation. Neither UI discovery nor an available command proves write eligibility.

```text
GTK 3 desktop (C++17)
  → native/runtime.cpp registry + mapper + operation dispatcher
  → typed worker protocol + bounded process supervision
  → filesystem-neutral C device/identity/privilege/Stop services
  → trusted format-specific .so host, or explicitly bounded native engine
  → raw image or unmounted block device
```

Current source ownership:

- `native/`: the single application registry (C++17), GTK desktop, mapper, operation dispatcher and privileged-helper session. No parallel Python application/policy graph is shipped.
- `src/core/`: C-based filesystem-neutral safety, one-open identity and capacity binding, mounted-overlap checks, exact I/O, result/Stop plumbing.
- `gui/filesystems/*/native/`: format workers and module hosts; their source provenance and supported ABI must be established individually. Remaining transitional implementations must not act as fallbacks for required external modules.
- `test_media/`: independent, destructive qualification fixtures; not a general-purpose disk manager.
- Pinned Common/Libraries: generic checked arithmetic, parsing, byte order, exact-I/O, durable-file and presentation primitives; not filesystem placement or transaction policy.

C is appropriate for on-disk layouts, parsers and bounded writer logic; C++17 provides value and lifetime ownership in the filesystem-neutral service layer. Python is limited to independent test fixture construction and black-box native-process orchestration.

The [format-module ownership contract](FILESYSTEM_ENGINE_OWNERSHIP.md) is authoritative for which implementations have moved into standalone `.so` modules. The target is one format engine used by every relevant consumer; the current Linux kernel bridge is not implied to exist.

## Device and mutation admission

A raw write may begin only after all relevant checks succeed:

1. Resolve one exact target; establish device/inode or block identity, capacity and supported filesystem format.
2. Reject mounted/overlapping targets, inconsistent topology and unexpected device replacement, including aliases/hard links.
3. Require an independently qualified native analysis, declared operation and supported geometry/features for a new Defragment/Growth operation.
4. Recheck target and format inside the privileged worker; verify the required filesystem module's trusted path, identity and ABI when the operation uses one.
5. Build and validate an exact canonical plan, including the per-file 10% Growth Defrag reserve where applicable.
6. Persist recovery journal and verified staging **before** source mutation can require restoration.
7. Perform bounded durable writes, honour Stop at safe/recoverable boundaries and then reopen the disk read-only for independent final verification.

Discovery labels, GPT identifiers, filenames or libblkid hints choose candidate readers; they never authorise mutation. Unknown feature bits, broken metadata, missing trusted modules and stale identity fail closed. A native worker must not fall back to a private parser/writer when its mandatory SO is missing.

Recover is **not** gated on a new successful analysis: after interruption, source metadata may be unreadable. It is admitted through an existing user-bound journal and the worker's own schema, target identity/capacity, staged-byte hash and transaction-phase checks.

## Privilege, process and recovery lifetime

The privileged helper launches only constrained commands with `posix_spawn` and process-group ownership; stdin is isolated from the GUI protocol. Cancellation first requests the worker's cooperative path and waits until the actual child exits, preserving recovery state as needed. A closed protocol channel, failed subprocess or incorrect result identity is not success.

The mapper preserves physical disk order in output; bounded capture and a bounded number of map cells prevent unbounded GUI memory. Exact FAT width/probe selection and module ABI are checked against the opened volume rather than trusting generic FAT labels. Mounted-image checking uses inode/device identity to catch hard-link aliases.

Durable transaction records bind one target and progress monotonically. Journal cleanup occurs only after verified completion. Source, staging and recovery identity are never guessed after process restart. No claim is made that process tests reproduce physical power loss or storage hardware lying about flush durability.

## Installation and release

Two user-facing executables are installed: `/Programs/defragmenter` and `/Programs/defragmenter-test-media`. Trusted private command workers, including mapper, operation engine, privileged helper and the EXT metadata worker, install under `/System/Commands/Defragmenter`. This matches the actual CMake/package manifest and InfiltratorOS verifier. `/System/Filesystems` is reserved for independently installed filesystem drivers, **not** those private executables.

The generic `.deb` and source-based `.run` are built from the versioned source; `.run` packages source from exact committed Git objects plus the pinned Common commit. Release intent is a `defragger/VERSION` change. The permanent main qualification, sanitizer lane, source/governance audit identities, linear protected history and once-built checksum-bound release artifacts are mandatory. Publication never rebuilds the qualified files and never modifies an existing version identity.

[Audit Status](AUDIT_STATUS.md) records the audited source and governance commit SHAs. **An audited ancestor does not automatically qualify later changes in executable, test or workflow inputs.** See [Validation](VALIDATION.md) for the exact release-gate evidence and [Decisions](DECISIONS.md) for the permanent rationale.

## Test Media

The 21-slot native C creator exercises disposable media through first-party format interfaces, with independent raw verification of exact logical payloads. No host formatter, target mount or ZFS pool import/export is used for fixture creation/verification. State schema 3 binds disk identity, corpus version and build results; the expected corpus is compiled policy, not editable saved state. ZFS and swap are analysis-only. Tests must still prove the actual external SO was used wherever mandated.

See [Design](DESIGN.md), [References](REFERENCES.md) and [UI vision](UI_VISION.md) for specialist requirements.
