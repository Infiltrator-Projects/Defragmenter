# Validation

## Purpose

Validation distinguishes implemented behaviour from behaviour that has actually been demonstrated. A successful build proves compilation; it does not by itself prove filesystem correctness, crash consistency or safe recovery.

Defragmenter therefore uses stronger evidence for destructive operations than for read-only analysis. The current release-specific safety decision and exact audited baselines are recorded in [AUDIT_STATUS.md](AUDIT_STATUS.md).

## Automated evidence

The project quality gate combines:

- warnings-as-errors native builds;
- parser, geometry, checksum and allocation-model tests;
- a deterministic malformed-image matrix that drives every installed native filesystem identifier with empty, truncated, all-ones and seeded-noise media and rejects hangs, signals or accidental acceptance;
- disposable filesystem-image mutation tests;
- target-safety, privilege, Stop and transaction regressions, including mounted-image hard-link identity and native privileged-helper tests that launch controlled children and verify SIGINT, delayed safe completion and reaping on Stop, control EOF and broken output;
- GUI/service and typed worker-protocol tests, plus C++ mapper contract checks and real-fixture parity against native filesystem analysis;
- architecture/Common/release-contract tests;
- AddressSanitizer and UndefinedBehaviorSanitizer qualification;
- package/native-installer construction from the exact tested source.

Automated checks cover ordinary behaviour, important boundaries, malformed/error cases and release/package contracts appropriate to the affected subsystem.

The native supervisor fixtures exercise the production supervision code through a test-only child resolver; they do not themselves write a filesystem. Real disposable-image worker tests separately verify payload, layout and recovery. Mapper regressions include generic FAT identification for FAT12/FAT16 and an 800,000-cell FAT32 map exceeding 64 MiB, plus rejection beyond the GUI's 1,048,576-cell maximum. GUI lifecycle tests verify that image mutation and Recover use the protected privileged journal path.

## Destructive-path evidence

A write-capable change is expected to demonstrate more than process success. Where the filesystem contract permits it, tests manufacture a known fragmented image, invoke the production worker, reopen the result and verify payload identity plus the required allocation layout.

Recovery tests inject failure around durable transaction boundaries and accept only three classes of result: no authoritative source write occurred, the filesystem is already valid, or durable state remains sufficient for Recover.

Growth Defrag tests verify the exact 10% post-file reserve rather than treating "some free space" as equivalent.

Minix qualification uses an independently manufactured fragmented v3 image to exercise the production native worker end to end. It verifies canonical Defragment, exact 10% Growth Defrag idempotence, durable Recover after a source-open failure, and fail-closed rejection of a recovery stage whose persisted SHA-256 no longer matches the journal. The native unit suite separately reopens staged images, verifies logical payload identity across relocated zones and rejects a Growth layout as a packed Defragment layout.

The XFS metadata white-box suite includes the field-shaped allocation-tree pressure case in which the final free-space map needs 40 bnobt/cntbt blocks while only 13 tree/AGFL blocks are initially available. The regression verifies safe reserve growth from source-free/final-free blocks, preservation of protected Growth Defrag runs and regeneration of XFS AG-owner reverse mappings. This is structural disposable-test evidence; the user's physical 2 GB XFS device remains separate live-media evidence.

GUI contract tests also treat typography as release behaviour rather than decoration. They verify that the main application and Test Media reference only the MB Corpo A/S families, that the package still installs the A Condensed, S Regular and S Bold font files, and that generic/system font fallbacks or a forced host monospace log cannot be silently reintroduced.

Common-integration regressions additionally bind CMake, the local installer and the submodule to the exact released Common 1.19.10 revision. They verify that C++ JSON real conversion uses Common's locale-independent parser, local allocation wrappers use Common checked size arithmetic, native recovery/relayout readers use Common's key=value parser, shared lexical path construction uses Common path contracts, Test Media obtains typography/metrics and the semantic palette roles through the native design API, and the GTK token generator plus font packager derive their shared design/provenance data from the pinned Common source rather than duplicated constants. The generated Python palette must equal Common's complete Day/Night maps, the About dialog may not carry private palette literals, and the Python compatibility I/O layer is regression-locked read-only while native writers own Common-backed exact I/O and durability.

Branding validation likewise treats the Defragmenter icon as an exact release asset. The architecture test verifies the approved 96×96 PNG's Git object identity, dimensions, complete chunk boundaries and CRCs and checks that packaging installs it consistently for the desktop icon theme, Mint app-install catalogue and About/window private path.

The mutation path is not accepted as its own sole oracle where a separate structural or payload check can be used.

## Block-layer crash replay

`defragger/tests/destructive/run_dm_log_writes_replay.sh` is an opt-in
sacrificial-media harness for Linux `dm-log-writes` plus the upstream
`replay-log` utility. It copies a prepared filesystem image through the
logging target, marks that exact state as the baseline, runs the production
Defragmenter worker and then reconstructs/replays the operation while invoking
a caller-supplied read-only checker at every FLUSH or FUA boundary.

This evidence is deliberately separate from the normal hosted quality gate:
it requires root, two disposable block devices and a kernel exposing the
`log-writes` target. It validates block-layer write ordering of the source
filesystem. It does not pretend to replay persistence of the external recovery
journal; the transaction/fault-injection suites remain the evidence for that
cross-filesystem recovery contract.

## Manual and environment-dependent evidence

Synthetic images and hosted runners cannot prove every storage-controller, kernel, privilege-manager or real-media interaction. Live testing on sacrificial media is therefore separate evidence for environment-dependent behaviour.

Manual evidence must be described at the level actually observed. A fixture, simulator or mocked failure is not physical-media proof.

## Release criterion

The exact revision intended for release must pass the required Project quality gate. Release assets must be derived from that revision, the audit must name the current version and exact audited source/governance baselines, and documentation must not advertise known-failing or merely planned write support as complete.

A release gate also verifies the exact pinned Common dependency and rejects audited production or workflow drift beyond the recorded baselines. The audited production set explicitly includes the C++ application-service sources under `defragger/native/`.

## Regression rule

Every reproducible defect should gain the narrowest useful permanent regression. Changes to a writer should update the evidence for the affected safety boundary, including a fail-closed case and interruption/recovery coverage when the transaction boundary changes.

Tests are part of the product contract, not disposable scaffolding.

## Limits

The evidence is not a mathematical proof and does not establish correctness for untested feature combinations, compromised privileged environments, or hardware/firmware that falsely acknowledges persistence. Those limits are why unsupported states fail closed and destructive qualification uses verified backups or sacrificial media.
