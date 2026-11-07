# Defragmenter documentation

Current contracts are organised by **responsibility**, not by alpha release number. Start with the [product overview](../README.md) or [implementation/support matrix](../defragger/README.md).

- [Philosophy](PHILOSOPHY.md) — Infiltrator engineering standard.
- [Architecture](ARCHITECTURE.md) — native source ownership, trust, process and installation boundaries.
- [Filesystem engine ownership](FILESYSTEM_ENGINE_OWNERSHIP.md) — authoritative format modules, migration and proof.
- [Design](DESIGN.md) — goals, non-goals and engineering trade-offs.
- [Architectural decisions](DECISIONS.md) — retained long-lived design rationale.
- [Roadmap](ROADMAP.md) — active qualification and engineering boundaries, not finished-update diaries.
- [Validation](VALIDATION.md) — automated, destructive-path, recovery and physical evidence.
- [Audit Status](AUDIT_STATUS.md) — **exact release-gate baseline IDs**, current qualification limitations and safety case.
- [Technical references](REFERENCES.md) — external format/platform evidence.
- [UI vision](UI_VISION.md) — desktop design and physical-map rules.
- Installed user manual — eleven searchable guide pages plus the full GPL appendix, generated at package build time by `defragger/packaging/generate-manual.py` and installed flat at `/System/Resources/Documentation/Defragmenter.pdf`.
- [Changelog](../CHANGELOG.md) — concise milestones and immutable-history links.
- [Contributing](../CONTRIBUTING.md) and [Security](../SECURITY.md) — contribution and vulnerability rules.

**Authority:** code/tests define implemented behaviour; current maintained architecture and decisions define obligations; `AUDIT_STATUS.md` names auditable source/governance boundaries; the permanent quality gate decides release eligibility. None of those is interchangeable.

The old per-release engineering notes and detailed audit chronology were retired from the active documentation tree as duplicate history. They are preserved in the immutable [v1.8.0-281 repository snapshot](https://github.com/Infiltrator-Projects/Defragmenter/tree/v1.8.0-281/docs/releases) and Git history. Do not reconstruct a claim of successful qualification merely from historical prose.

When a contract changes, update its single owning document and link to it rather than copying the same announcement across multiple Markdown files.
