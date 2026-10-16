# Documentation

This directory is the canonical documentation entry point for Defragmenter. The Infiltrator project family uses the same baseline document roles across repositories so readers can move between projects without relearning the structure.

## Canonical baseline

- [Engineering philosophy](PHILOSOPHY.md) — governing engineering and documentation standard.
- [Architecture](ARCHITECTURE.md) — ownership, layers, dependencies, safety boundaries and system contracts.
- [Design](DESIGN.md) — first-principles goals, non-goals, trade-offs and failure philosophy.
- [Decisions](DECISIONS.md) — durable architectural decisions, alternatives and consequences.
- [Roadmap](ROADMAP.md) — current foundation, near-term priorities and longer-term direction.
- [Validation](VALIDATION.md) — automated, destructive-path, manual and release evidence boundaries.
- [Project README](../README.md) — product overview, capabilities, build/use entry point and engineering ethos.
- [Changelog](../CHANGELOG.md) — retained consolidated history through the entries it contains. Later release-specific engineering notes live under `releases/` where retained; exact published identities and source remain the Git tags/releases rather than an inferred gap-free prose ledger.
- [Contributing](../CONTRIBUTING.md) — development, ownership and verification rules.
- [Security](../SECURITY.md) — vulnerability scope, reporting and response policy.

## Documentation authority

The baseline files have distinct responsibilities and should not compete as alternate sources of truth. Architecture describes where behaviour belongs; Design explains why; Decisions preserve durable choices; Roadmap describes direction; Validation states what evidence is required.

Code and tests remain authoritative for executable behaviour. Historical version tags and releases identify exact published source and assets under the project's write-once release policy. Release-specific safety qualification belongs in Audit Status rather than being copied into design documents.

## Specialist documentation

- [Audit status](AUDIT_STATUS.md) — current write-safety case and exact audited source/governance baselines.
- [References](REFERENCES.md) — filesystem/platform specifications and engineering sources.
- [Implementation README](../defragger/README.md) — source-tree layout, support matrix and local build notes.
- `releases/` — release-specific engineering notes where a detailed note was retained; these do not replace the exact tag/release identity.

## Maintenance rule

When a change moves an ownership boundary, support boundary, validation claim or major design decision, update the corresponding canonical document in the same change. Avoid copying the same status paragraph into several files; link to the authoritative document instead.

Filesystem-specific implementation detail belongs with the owning source/package unless it establishes a maintained cross-format contract.
