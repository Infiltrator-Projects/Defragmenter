# Universal Defragmenter and filesystem SO migration

**Status (2026-11-07): APPROVED DESIGN, IMPLEMENTATION IN PROGRESS.**
This is the canonical, persistent statement of the requested architecture.
Do not reinterpret it as 21 independent defragmentation programs.

## The rule

**One Defragmenter determines WHAT changes to make. Each filesystem SO
translates those requests into HOW that filesystem safely makes them.**

`/System/Filesystems/*.so` are independently installed, independently versioned
filesystem implementations in their own Git repositories. Their primary job is
to understand the filesystem, expose trustworthy file/extent/allocation metadata,
and perform valid metadata/transaction operations. A filesystem SO must **not**
be required to reimplement Defragmenter's normal packing and Growth Defrag
placement policy. Defragmenter must **not** contain private filesystem
formatters, parsers, allocation tables or volume-specific writers.

### Responsibility boundary

| Defragmenter (one application) | Individual filesystem SO |
| --- | --- |
| Discover installed filesystem modules dynamically; no compiled format list | Advertise identity, ABI, features and capabilities |
| Provide consistent Analyse, map, Defrag, Growth Defrag, Stop, Verify and Recovery coordination | Identify/probe the volume and enumerate files, extents, free/pinned/shared ranges and format constraints |
| Decide file order, target ranges, packing policy, requested 10% adjacent reserve, scheduling and progress | Validate proposed placements against actual allocation rules and feature constraints |
| Enforce device identity, mounts/active use, generic I/O safety, cancellation and host journal mechanisms | Translate movements into correct metadata changes and *filesystem-specific* durable transactions |
| Coordinate verification, report incomplete/unsupported operations honestly | Implement or refuse transaction commit, interrupted recovery, checksums, snapshots/reflinks and filesystem-specific invariants |

**Important nuance:** metadata changes, journalling and recovery may *require*
filesystem-specific mechanisms; they cannot safely be replaced by generic raw
sector copies. A safe filesystem adapter may be substantial even when the
planning algorithm is shared. For example, ZFS needs copy-on-write block
pointer updates and transaction-group publication; that is translation, not
a second generic defragmentation algorithm.

## Target protocol (not yet an implemented ABI)

1. Discover an independently installed trusted `.so` and inspect its ABI
   version and declared features. Match the **module's advertised identity**;
   reject duplicate, malformed or missing modules.
2. Ask it to identify the volume, provide a complete certified inventory:
   stable object IDs, extent mappings, free/used/pinned/shared regions, alignment
   and allocation restrictions. Never infer that shared or pinned blocks are
   freely movable.
3. Ask the shared Defragmenter planner for a proposed Defrag or 10% Growth
   layout, with explicit failure if the constraints cannot be satisfied. This
   is a *proposed* plan, not authority to write.
4. Have the SO validate that exact plan and provide a filesystem-valid
   transaction/relocation mechanism. Defragmenter coordinates stop/progress and
   durable execution. If the SO cannot safely execute the layout, **fail closed**.
5. Have the SO publish/recover its own atomic filesystem state, then verify the
   actual allocation layout, all relevant file bytes and metadata invariants.
6. Count a required operation as **passed only when the operation really
   executed and passed its postconditions**. A skipped Defrag or Growth Defrag
   is a failed *Defrag qualification*. A separately successful Verify-files
   operation is not a successful Defrag qualification.

If a filesystem is structurally ineligible for file defragmentation (e.g.
Linux swap), do not falsely present it as a passed defragmentation engine:
exclude it explicitly from the applicable module-capability set, or report
the requested operation as not supported; never count a skip as a pass.

## Implemented today (do not overclaim)

- Libraries defines the Common V2 filesystem ABI. The independent
  `Filesystem-*` repositories own their format engines and advertise the
  capabilities each shared object implements.
- Defragmenter discovers Common V2 modules through `--list-installed-modules`.
  Dedicated workers use Common V2 for volume information and supported
  per-file extent maps, including NTFS. The GUI still uses a format registry
  and routes remaining whole-volume analysis, mutation and recovery through
  the format workers.
- On current `main`, OFS Test Media formats, populates and verifies its corpus
  through `OFS.so` using Common V2. OFS relocation and recovery still use the
  filesystem-specific transaction interface.
- The **plan-only** core includes `ld_generic_layout_plan` for a certified
  contiguous region and `ld_generic_layout_plan_file_extents` for one file
  across disjoint, sorted provider-certified free runs, preferring a single
  fit and reserving the final Growth gap within the last data extent's run.
  Defrag V2 host ABI exposes optional appended, size-checked `plan_layout`
  and `plan_file_extents` callbacks. These functions do not certify an
  SO transaction or supply a general filesystem allocator. OFS has a
  bounded shared-planner pilot; **general production ABI migration is still
  incomplete**.
- Existing filesystem-specific production movers and their recovery paths are
  **preserved intentionally**. Do not delete them merely because the new
  planner compiles; retain them until equivalent end-to-end verified behaviour
  is proven using the common protocol.
- ZFS has a separate `Filesystem-ZFS` Git and `ZFS.so`, but its production
  Defrag/Growth writers and their safe transaction adaptor **do not exist**.
  ZFS passed fixture creation and integrity verification, not mutation.
- Test Media qualification no longer treats skipped required mutation
  operations as successes in current `main`; this is **not** in release
  `v1.8.0-288` as of the initial recording date.
- The latest *confirmed published* release at the time of this design
  recording is `v1.8.0-288`. Changes on `main` require all CI/audit/release
  gates before claiming they are published.

## Ordered migration checklist

- [ ] **Gate 0: Preserve baseline.** Verify all latest `main` tests,
      resolve source-audit changes, record source SHA, installed-module ABI
      inventory and physical-device Test Media result separately.
- [ ] **Gate 1: Specify the *next* common module ABI.** Versioned,
      size-checked inventory and capability discovery, block-size/unit rules,
      pinned/shared/reserved constraints, safe plan validation, transactional
      relocate/commit/rollback, Stop and postcondition inspection. One header
      owned by the interface authority, shared with SO repos; no duplicated
      divergent definitions. Keep older ABI compatibility only during migration.
- [ ] **Gate 2: Complete the generic planner.** Contiguous and disjoint
      free-run plan-only functions are implemented. Remaining work includes
      complete reserved/pinned/shared interval modelling, allocation across
      multiple objects and independently observable exact postconditions.
      No raw physical write belongs in the planner.
- [ ] **Gate 3: Finish the OFS pilot.** Shared-planner calls are already
      integrated in a bounded OFS pilot. Require complete inventory, safe
      transaction translation, exact-placement and full-byte comparisons,
      Stop/recovery, interruption replay and sacrificial physical-media
      evidence before replacing any existing production engine.
- [ ] **Gate 4: Generalize the host/GUI.** Replace hardcoded format switches
      and worker lists with dynamic discovery and capability-driven actions.
      Convert Test Media qualification from fixed 21-format assumptions to the
      discovered module capability set. Required skips fail; formats with no
      file-defrag semantics are explicitly inapplicable rather than falsely
      qualified.
- [ ] **Gate 5: Migrate remaining independent SOs one by one.** Start with
      formats whose allocation constraints can be expressed in the generic
      planner; adapt the planner for harder cases before migrating NTFS, Btrfs,
      APFS and ZFS. Preserve metadata, journal/recovery and feature refusal
      semantics. Never combine source repos or replace SO ownership with a
      monolithic Filesystems library.
- [ ] **Gate 6: ZFS capability.** Extend ZFS.so from read-only analysis to
      certified file/extents inventory, transaction translator and verified
      write/recovery for explicitly supported pool layouts. Do not label an
      ordinary block rewrite as guaranteed contiguous packing or 10% growth.
- [ ] **Gate 7: Remove superseded workers and dependencies.** Only after
      equivalence, audit source/package dependencies, rebuild/release through
      CI and rerun qualification against sacrificial media. The kernel and
      other OS services may still be needed as a transitional platform.
- [ ] **Gate 8: Release and document.** Publish verified `.deb`, native
      `.run` and module releases, confirm repository propagation separately.
      Change the documentation from *design* to *implemented* only when proven.

## Ground rules

- Single `main` branch in each repo; keep filesystem implementations in
  their independently owned Git repositories.
- No filesystem engines quietly copied back into Defragmenter.
- No operation is called successful merely because a fixture was formatted,
  a byte-verify passed, or a previously skipped module returned exit code 0.
- No unsupported filesystem layouts silently rewritten.
- Preserve working release `1.8.0-288` until a replacement is built, audited,
  published and independently verified.
- **Do not lose or redefine this architecture without an explicit user decision.**

See `docs/FILESYSTEM_ENGINE_OWNERSHIP.md`, `docs/ROADMAP.md`,
`defragger/native/filesystem_defrag_abi.h`, and
`defragger/src/core/ld_generic_layout.{h,c}` for related current code.
