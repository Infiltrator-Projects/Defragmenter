# Contributing

Defragmenter accepts changes only when they preserve the write-safety case and the single-source-of-truth architecture.

Read [PHILOSOPHY.md](docs/PHILOSOPHY.md) before making a substantial change. It is the repository entry point to the canonical Infiltrator Engineering Philosophy and defines the expected engineering, review, comment and documentation standard.

Before changing code, read:

- `docs/ARCHITECTURE.md`
- `docs/DESIGN.md`
- `docs/DECISIONS.md`
- `docs/VALIDATION.md`
- `docs/AUDIT_STATUS.md`
- `defragger/README.md`

Engineering rules:

- Prefer one authoritative implementation over compatibility layers.
- Do not introduce a second filesystem registry, write-policy table, package manifest or theme authority.
- Filesystem-specific parsing and mutation semantics belong with their native filesystem package.
- The native C++ registry and operation engine own application-wide dispatch and user-facing operation policy.
- Filesystem-neutral device, mount, identity, I/O and Stop safety belong under `src/core/`.
- Use Libraries for genuinely generic mechanisms when its contract is at least as strong as the local requirement.
- Do not move product-specific disk safety, staging or recovery semantics into Common merely to reduce local line count.
- Python under `tests/` is independent fixture and black-box orchestration tooling only. Production application, presentation, capability and privilege policy remain native C/C++; `gui/ui/art/` contains native desktop artwork, not a Python compatibility graph.
- Treat parser uncertainty, target-identity uncertainty and recovery uncertainty as fail-closed conditions.
- Update maintained documentation when an ownership, safety, release or support contract changes.

Validation rules:

- Keep first-party C/C++ warnings clean under the permanent warnings-as-errors build.
- Add boundary-oriented tests for changed behaviour; do not couple tests to private implementation merely to increase coverage.
- Parser changes need valid, malformed and truncated evidence.
- Writer changes need disposable-image mutation plus independent verification and recovery evidence.
- Release/build changes need package/install/release contract tests.
- Run the full project suite before release; focused local tests are development aids, not a release substitute.

Repository hygiene:

- Development is direct-to-protected-`main`; do not create routine feature or release branches.
- Do not commit generated packages, build trees, caches, qualification output or forensic scratch files.
- `VERSION` is the explicit release decision. Ordinary commits do not publish.
- Never rebuild a qualified release in the publication phase.
