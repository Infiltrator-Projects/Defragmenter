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
- local-installer construction plus Debian packaging-contract/version checks from the exact tested source; the final `.deb` and release `.run` artifacts are rebuilt and revalidated by the release workflow from that same qualified commit.

Automated checks cover ordinary behaviour, important boundaries, malformed/error cases and release/package contracts appropriate to the affected subsystem.

The native supervisor fixtures exercise the production supervision code through a test-only child resolver; they do not themselves write a filesystem. Real disposable-image worker tests separately verify payload, layout and recovery. Mapper regressions include generic FAT identification for FAT12/FAT16 and an 800,000-cell FAT32 map exceeding 64 MiB, plus rejection beyond the GUI's 1,048,576-cell maximum. GUI lifecycle tests verify that image mutation and Recover use the protected privileged journal path.

## Destructive-path evidence

A write-capable change is expected to demonstrate more than process success. Where the filesystem contract permits it, tests manufacture a known fragmented image, invoke the production worker, reopen the result and verify payload identity plus the required allocation layout.

Recovery tests inject failure around durable transaction boundaries and accept only three classes of result: no authoritative source write occurred, the filesystem is already valid, or durable state remains sufficient for Recover.

Growth Defrag tests verify the exact 10% post-file reserve rather than treating "some free space" as equivalent.

EXT2/3/4 qualification deliberately keeps e2fsprogs/libext2fs on the
opposite side of the production boundary. The test-only `make_ext4_image.c`
fixture uses libext2fs to manufacture a checksummed fragmented EXT4 image,
including an external extent-tree block. Production analysis, metadata
classification, Defragment and Growth Defrag then run exclusively through
Defragmenter's first-party `ext_disk.c` parser/mutator. The end-to-end
regression reopens the result through that production engine, verifies UUID and
fragmentation state, and verifies the exact 10% Growth layout. Architecture and
release regressions additionally forbid libext2fs headers, types, calls, runtime
linkage and package dependencies from the production EXT path while permitting
the independent test fixture/oracle.

Minix qualification uses an independently manufactured fragmented v3 image to exercise the production native worker end to end. It verifies canonical Defragment, exact 10% Growth Defrag idempotence, durable Recover after a source-open failure, and fail-closed rejection of a recovery stage whose persisted SHA-256 no longer matches the journal. The native unit suite separately reopens staged images, verifies logical payload identity across relocated zones and rejects a Growth layout as a packed Defragment layout.

SFS2 qualification extends the existing SFS0 evidence with deterministic structure-version-4 fixtures exercised through production Defragment, exact 10% Growth Defrag and Recover. A separate sparse image is larger than 4 GiB and contains an extent longer than 65,535 blocks, forcing the native analyser to decode SFS2's 48-bit file-size and 32-bit extent-count fields; this prevents an SFS0-width implementation from passing the SFS2 gate accidentally.

PFS3 qualification uses independently constructed on-disk structures rather than a mounted host filesystem: a PFS root block, reserved bitmap, bitmap-index/allocation-bitmap hierarchy, anode index/blocks and directory blocks describe deliberately fragmented regular files. The native suite now covers recursive user-directory traversal with parent/anode validation, cycle/duplicate refusal, root and nested payload ownership, Defragment and exact 10% Growth Defrag. The transaction suite exercises durable Recover and unsupported superindex state remains fail-closed. The separate first-party Test Media creator builds a 2 GiB PFS3 image with deterministic fragmented payloads and byte-for-byte verification. Validated hard links, soft-link payloads and rollover files are covered by the qualified writer contract. PFS2, SUPERINDEX/LARGEFILE and remaining unsupported special-entry or larger reserved-index geometries remain fail-closed.

Classic HFS qualification starts from independently manufactured MDB, allocation bitmap, Extents Overflow B-tree and Catalog B-tree structures. The native regression covers both ordinary inline forks and a five-block fork split between three inline extents and two Extents Overflow records. Defragment and exact 10% Growth Defrag must preserve the complete logical payload byte-for-byte, produce physically contiguous allocation, preserve safe overflow-record topology, leave the post-fork reserve free, and support durable Recover. The writer remains fail-closed for malformed/incomplete extent chains, embedded HFS+/HFSX, inconsistent or write-locked volumes.

Btrfs qualification uses independently constructed CRC32C-valid single-device images containing a system chunk, one unprofiled mixed data/metadata chunk, level-0 root/extent/filesystem/device/checksum trees and skinny metadata references. Regressions cover both NODATASUM and ordinary checksum-tree-protected regular files split across physical extents. The writer verifies each source CRC32C sector against payload bytes, remaps checksum keys through the exact relocation plan, rebuilds checksum items, advances metadata generations/checksums and independently reopens/verifies the staged result. Defragment, exact 10% Growth Defrag and durable Recover are covered. Multi-device/striped/profiled layouts, snapshots/subvolumes, qgroups, active log/balance/device-replace state, shared/sparse/encoded external extents, deeper mutable roots and unsupported feature bits remain outside the writer contract.

APFS qualification uses two independently expressed fixture paths: the Python on-disk constructor used by the native worker regression and the first-party all-C Test Media creator/verifier. Both manufacture the bounded Fletcher-valid contract with checkpoint-map resolution of the ephemeral spaceman, one direct chunk-info block/allocation bitmap, container and volume object maps, one unencrypted snapshot-free volume, flat catalog/extent-reference roots and fragmented regular-file data. The writer may retain checksum-valid older descriptor-ring NX/checkpoint-map history whose XID is strictly older than the selected active checkpoint and whose container geometry/UUID remain compatible; the regression proves that history remains byte-for-byte untouched through Defragment and qualifies Growth Defrag with the same history present. Shared extents remain fail-closed. Multiple volumes, encryption/sealing, snapshots, sparse/cloned/shared/encoded state, deeper mutable trees, indirect CIB/CAB layouts, internal-pool allocation and pending spaceman free queues remain outside the writer contract.

The XFS metadata white-box suite includes the field-shaped allocation-tree pressure case in which the final free-space map needs 40 bnobt/cntbt blocks while only 13 tree/AGFL blocks are initially available. The regression verifies safe reserve growth from source-free/final-free blocks, preservation of protected Growth Defrag runs and regeneration of XFS AG-owner reverse mappings. This is structural disposable-test evidence; the user's physical 2 GB XFS device remains separate live-media evidence.

GUI contract tests also treat typography as release behaviour rather than decoration. They verify that the main application and Test Media reference only the MB Corpo A/S families, that the package still installs the A Condensed, S Regular and S Bold font files, and that generic/system font fallbacks or a forced host monospace log cannot be silently reintroduced.

Common-integration regressions additionally bind CMake, the local installer and the submodule to the exact released Common 1.19.35 revision. They verify that C++ JSON real conversion uses Common's locale-independent parser, local allocation wrappers use Common checked size arithmetic, native recovery/relayout readers use Common's key=value parser, shared lexical path construction uses Common path contracts, Test Media obtains typography/metrics and the semantic palette roles through the native design API, and the GTK token generator plus font packager derive their shared design/provenance data from the pinned Common source rather than duplicated constants. Legacy/source compatibility palette fixtures must equal Common's complete Day/Night maps and the About surface may not carry private palette literals. The installed application, filesystem I/O, mapping, mutation and GTK desktop are wholly native C/C++; retained Python files are source/test compatibility material only.

Branding validation likewise treats the Defragmenter icon as an exact release asset. The architecture test verifies the approved 96×96 PNG's Git object identity, dimensions, complete chunk boundaries and CRCs and checks that packaging installs it consistently for the desktop icon theme, Mint app-install catalogue and About/window private path.

The mutation path is not accepted as its own sole oracle where a separate structural or payload check can be used.


ZFS qualification uses both independently constructed synthetic on-disk
fixtures and the isolated Test Media pool path. The native regression covers
four-label/uberblock selection, little- and big-endian decoding, MOS root block
pointers, packed-XDR topology, Fletcher4 verification, LZJB/LZ4 decoding,
MOS/dnode traversal, per-metaslab space-map replay and deliberately fragmented
regular-file block trees. Feature-flag-pool tests prove that supported
MOS-required features can retain exact analysis, unknown read-critical features
fail closed, and active `log_spacemap` state cannot be mislabeled exact while
unflushed allocation changes remain outside the per-metaslab maps. The
production registry deliberately exposes no ZFS Defragment/Growth Defrag/Recover
operations; ADR-008 records why raw ZFS mutation is outside the product
contract.

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

Allocation-map validation is also fail-closed: every returned cell must account exactly for its physical span across free/used/unknown/outside/bad primary states, while fragmentation and directory overlays may not exceed used allocation. Both the native mapper boundary and GTK presentation layer enforce the same invariant.

## Limits

The evidence is not a mathematical proof and does not establish correctness for untested feature combinations, compromised privileged environments, or hardware/firmware that falsely acknowledges persistence. Those limits are why unsupported states fail closed and destructive qualification uses verified backups or sacrificial media.


NTFS qualification additionally covers an ordinary named nonresident alternate data stream split across two physical runs. Defragment and Growth Defrag relocate the ADS through the same journalled record/attribute-offset plan as unnamed data, preserve its payload byte-for-byte and report ADS fragmentation at the owning-file level. Attribute-list/split, compressed, encrypted and sparse streams remain fail-closed or fixed as explicitly qualified.

UFS qualification includes sparse regular-file relocation: holes remain logical holes while allocated fragment/block references are packed, fragment-sized tails remain valid, and both Defragment and exact 10% Growth Defrag independently reopen and verify the result.
