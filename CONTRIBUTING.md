# Contributing

Defragmenter combines GTK orchestration, filesystem-neutral safety/runtime code and first-party filesystem engines. Contributions should preserve those boundaries and keep destructive behaviour demonstrably recoverable.

## Engineering rules

- Keep GTK presentation in `gui/ui/` and raw/filesystem policy below the presentation layer.
- Keep one filesystem registry and one authoritative implementation per filesystem.
- Keep filesystem-neutral raw device/runtime mechanics in `src/core/`; keep native application-service orchestration in `native/`; keep format semantics with the owning filesystem package.
- Reuse the pinned Infiltratr Common API when it is the correct generic abstraction; improve Common first if Defragmenter has the stronger generic implementation.
- Do not add external repair/defragmentation commands to production mutation paths.
- Treat unsupported, ambiguous or malformed on-disk state as a fail-closed result.
- Preserve target-identity, mounted-overlap, durable-recovery, safe-Stop and final-verification contracts.
- Add deterministic regression coverage for parser, planner, writer, transaction, safety, GUI/service or packaging changes.
- Do not create parallel Markdown for a subject already owned by the canonical documentation set.

## Language and dependency policy

C is the default for on-disk codecs, fixed-layout structures and direct storage work. Use C++17 when RAII, scoped ownership, stronger value types or explicit process/protocol ownership make a filesystem-neutral native component safer or clearer; do not introduce class hierarchies merely because C++ is available, and do not rewrite strong C for uniformity. Python is limited to GTK presentation/glue and must not become the authority for filesystem capability, mapping, mutation, recovery or privileged process safety.

Platform/system libraries are acceptable when their documented contract is the stronger engineering choice. Convenience alone is not a reason to move Defragmenter-owned semantics into a dependency.

## Build and validation

Clone recursively because the project pins Infiltratr Common:

```bash
git clone --recurse-submodules https://github.com/Infiltrator-Projects/Defragmenter.git
cd Defragmenter/defragger
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLD_ENABLE_WERROR=ON
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')
[ "$jobs" -gt 1 ] && jobs=$((jobs - 1))
cmake --build build -j"$jobs"
ctest --test-dir build --output-on-failure
```

Write-capable changes require evidence appropriate to the affected safety boundary. When a transaction or recovery contract changes, add interruption/fault coverage as well as a successful image path.

## Documentation and comments

Read `docs/README.md` for document authority. Architecture belongs in `docs/ARCHITECTURE.md`; rationale in `docs/DESIGN.md`; durable choices in `docs/DECISIONS.md`; direction in `docs/ROADMAP.md`; validation evidence in `docs/VALIDATION.md`; current release safety qualification in `docs/AUDIT_STATUS.md`.

Comments should preserve information expensive to reconstruct: on-disk units, ownership, target identity, transaction boundaries, durability assumptions, complexity limits and non-obvious format rules. Do not narrate straightforward syntax.

## Repository discipline

`main` is the authoritative development and release branch. Keep commits focused. Published tags/releases are immutable identities, and release publication is bound to the exact qualified commit.
