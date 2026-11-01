# Roadmap

This document records **remaining work and admission criteria**, not a diary of completed alpha iterations. Implemented operations and their bounded filesystem support are listed in [the source README](../defragger/README.md); exact claims require code and tests, not a checked box.

## Active engineering boundaries

- **Standalone filesystem ownership:** continue proving real module dispatch and removing duplicated private implementations only after parity and independent mutation/recovery evidence. A required `.so` must never have a hidden fallback. The full ownership contract is [here](FILESYSTEM_ENGINE_OWNERSHIP.md).
- **Audit-baseline integrity:** a later source or workflow change must be independently reviewed and explicitly re-audited before an exact-head release can qualify. See [Audit Status](AUDIT_STATUS.md). Documentation-only edits do not repair source-audit drift.
- **Media qualification:** continue representative sacrificial-device, power-loss/flush-boundary and privilege/environment testing separately from synthetic images and hosted CI. Record exact observed evidence; never infer physical-media success from tests.
- **Unsupported formats/features:** keep fail-closed limits explicit. For example, unsupported APFS sharing/encryption, complex Btrfs layouts, NTFS special streams and other unqualified metadata must not be advertised as general write support.
- **UFS variants:** retain separate UFS1/UFS2 identities; UFS1 Test Media creation requires its own explicit fixture and compatibility qualification rather than borrowing UFS2 evidence.

## Deliberate exclusions

ZFS/OpenZFS and Linux swap remain **analysis-only by design**. ZFS physical relocation belongs to its copy-on-write transaction-group allocator; an exact persistent 10% adjacent physical reserve is not a stable ZFS postcondition. Swap has no file defragmentation contract.

## Completion and regression rule

A format capability is admitted only when correct module ownership, trusted ABI/identity checks, malformed-state refusal, independent full-payload validation, exact placement, Stop/recovery evidence and the current release gates agree. Performance changes must preserve durability boundaries and deterministic placement; a faster unverified write is not completion.

For durable design decisions see [Decisions](DECISIONS.md), and for required evidence see [Validation](VALIDATION.md).
