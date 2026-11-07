# Validation

Validation distinguishes **built**, **tested**, **audited**, **published** and **physically qualified** states. Those states are not interchangeable. Current source/version truth is in the [implementation README](../defragger/README.md), release/audit truth in [Audit Status](AUDIT_STATUS.md), and design rationale in [Decisions](DECISIONS.md).

## Permanent automated qualification

The self-hosted Project quality gate and ASan/UBSan lane require, as applicable:

- Native C/C++ warnings-as-errors compilation and ABI/architecture tests.
- Native GUI, registry, mapper and privileged-session tests, including process-group cancellation, broken protocol/EOF and Stop reaping.
- Filesystem parsing and geometry, malformed/truncated/noise images, unsupported features, target identity/mounted-overlap refusal and bounded allocation-map accounting.
- Disposable fragmented-media writer tests for every qualified operation, with independent structural and **byte-for-byte** payload verification.
- Fault-injected journal/staging/transaction tests showing unchanged, valid or explicitly recoverable interruption states.
- Source-based local installer, generic Debian package, exact version/asset/metadata and third-party-source inventory tests.
- Read-only independent filesystem integration workflows that demonstrate trusted SO identity/ABI and absence-of-module failure without mutating the runner's `/System/Filesystems`.
- Release-invariant tests, exact audited source and governance baselines, and once-built SHA-bound artefact handoff. The publication phase must never rebuild.

Test-only independent fixture generators may use Python, libext2fs/e2fsprogs and comparable tools when needed to manufacture independent evidence; the production filesystem write path must not delegate to host filesystem repair or format utilities. Source-shape tests are appropriate for architectural restrictions, but functional regressions must prefer observable native behaviour.

The module qualification must prove **real dispatch**, not merely loadability or a convincing GUI result. Incompatible format, missing module, bad owner/permissions, absent ABI or invalid analysis must stop **before destructive preparation**, including partition/signature clearing.

## Destructive-path postconditions

Every write-capable format must pass its bounded supported-subset contract:

1. Generate an independently verified fragmented image; establish physical and filesystem identity.
2. Analyse and qualify the original metadata, content and allocation state.
3. Perform Defragment and, separately, Growth Defrag against disposable images.
4. Verify **all** logical payload bytes, directory/metadata invariants, no forbidden overlap and canonical placement; Growth requires the **exact** documented 10% reserve.
5. Inject failures near durable-phase transitions; require safe unchanged/valid/recoverable outcomes and test Recover, Stop and journal tampering.
6. Reject malformed, dirty, mounted, stale, incompatible or unsupported states before authoritative source changes.

Coverage includes the qualified bounded NTFS named streams, EXT metadata and checksum handling, XFS allocation-tree/AG reserve pressure, SFS2 48-bit/32-bit fields, PFS3 anode/directory and special-entry limits, APFS older-checkpoint preservation, Btrfs CRC32C/checksum-tree state, classic HFS Extents Overflow, sparse UFS files and FAT/OFS external module dispatch. These are **regression obligations**, not a claim that every physical volume supports mutation.

The native allocation map must account for each physical span exactly in used/free/unknown/outside/bad primary states, with fragmentation and directory overlays bounded by used allocation. The renderer cannot invent map cells or hide gaps.

## Test Media contract

All 21 registered slots require first-party C creation and raw verification, not host formatters, mounting or ZFS pool utilities. File-bearing fixtures use the deterministic eight large files of 4, 6, 10, 16, 26, 34, 44 and 60 MiB (**200 MiB total**) plus files of 0, 1, 511, 512, 513, 4095, 4096 and 4097 bytes. Independent readers re-open on-disk mappings and verify all payload bytes after creation and each supported operation. ZFS and swap remain analysis-only; **if included in a Defragment/Growth Defrag qualification run, missing write operations are qualification failures and make the run exit nonzero**. Running the separate Verify files action may still succeed for these formats, but that validates retained data only and must never be reported as production-defragmentation qualification.

The protected **schema 3** state file binds disk fingerprint, corpus version and exactly one build result per slot. The expected file list and bytes are compiled policy; missing/duplicate/unknown records, truncated state, mismatched disk identity, an altered payload and obsolete schema 2 are all rejected. Existing schema 2 disks must be rebuilt.

The 16 full-corpus native-fixture cases run independently with bounded four-way concurrency and per-case timeouts. Fixture success is no substitute for actual module-host tests and real physical-device qualification.

## Recovery and external evidence

The opt-in `defragger/tests/destructive/run_dm_log_writes_replay.sh` replays sacrificial block-layer writes at FLUSH/FUA boundaries using `dm-log-writes` and a caller-supplied independent read-only checker. It requires root and disposable devices. This does **not** model persistence of the separate recovery journal, so transaction/fault-injection suites remain mandatory.

Hosted images cannot establish behaviour for arbitrary hardware, controller flush integrity, all kernel/privilege environments or every untested feature combination. Physical-media testing must use sacrificial targets, preserve evidence and report only what was observed.

## Release admission

`defragger/VERSION` is the explicit immutable version decision. The exact intended `main` commit must pass both full mandatory qualification lanes and the release/source-governance invariants. The audited **source** baseline protects all tracked production, build, packaging, dependency and **test** inputs; the separately audited **governance** baseline protects qualification/publication workflows and scripts. Later drift is a release failure until explicitly re-audited, even if all filesystem tests pass.

A completed audit recorded for an older commit does not qualify later source changes. The evidence for a failing release-invariant step must be reported as such; never relabel the audit SHA without actually reviewing the delta. Tagged assets and public catalogue/APT observations are separate release stages. Published versions and asset SHA-256 identities are write-once under the project publisher's rules.

## Maintenance

Every reproduced defect requires a focused regression at its owning boundary. Preserve production/source test separation, malformed-state refusal, stable target identity, complete recovery evidence and independent payload oracles. Shorter documentation is not grounds to weaken these tests.
