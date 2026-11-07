# Roadmap

This document records **remaining work and admission criteria**, not a diary of completed alpha iterations. Implemented operations and their bounded filesystem support are listed in [the source README](../defragger/README.md); exact claims require code and tests, not a checked box.

## Active engineering boundaries

- **Standalone filesystem ownership:** continue proving real module dispatch and removing duplicated private implementations only after parity and independent mutation/recovery evidence. A required `.so` must never have a hidden fallback. The full ownership contract is [here](FILESYSTEM_ENGINE_OWNERSHIP.md).
- **Audit-baseline integrity:** a later source or workflow change must be independently reviewed and explicitly re-audited before an exact-head release can qualify. See [Audit Status](AUDIT_STATUS.md). Documentation-only edits do not repair source-audit drift.
- **Media qualification:** continue representative sacrificial-device, power-loss/flush-boundary and privilege/environment testing separately from synthetic images and hosted CI. Record exact observed evidence; never infer physical-media success from tests.
- **Unsupported formats/features:** keep fail-closed limits explicit. For example, unsupported APFS sharing/encryption, complex Btrfs layouts, NTFS special streams and other unqualified metadata must not be advertised as general write support.
- **UFS variants:** retain separate UFS1/UFS2 identities; UFS1 Test Media creation requires its own explicit fixture and compatibility qualification rather than borrowing UFS2 evidence.

## Incomplete filesystem capability — ZFS/OpenZFS

**ZFS Defragment and Growth Defrag remain unimplemented.** Their absence is an
open capability gap, not a successful defragmentation qualification or a
permanent exclusion from InfiltratorOS. The independent `Filesystem-ZFS`
repository owns the missing write-capable module. Defragmenter must discover
its operation capabilities from that SO; it must not add a private ZFS writer,
pretend that read-only analysis passed mutation tests, or dispatch through an
external `zfs` executable.

ZFS's copy-on-write block pointers, transaction-group publication, snapshots,
clones, shared blocks and allocator policy require a filesystem-owned,
crash-recoverable rewrite/relocation implementation. OpenZFS's file rewrite
capability establishes that block rewriting is possible but does not itself
prove canonical physical packing or exact adjacent 10% Growth Defrag reserves.
The specific operation contract must be truthful and independently verified,
not inferred from a generic capability label. Unsupported pool layouts must
continue to fail closed until the module has a validated writer.

**Linux swap** remains analysis-only because it does not have regular files
or a file defragmentation contract.

## Completion and regression rule

A format capability is admitted only when correct module ownership, trusted ABI/identity checks, malformed-state refusal, independent full-payload validation, exact placement, Stop/recovery evidence and the current release gates agree. Performance changes must preserve durability boundaries and deterministic placement; a faster unverified write is not completion.

For durable design decisions see [Decisions](DECISIONS.md), and for required evidence see [Validation](VALIDATION.md).
