# Changelog

This is a **maintained milestone summary**, not an assertion that every intermediate alpha build was individually release-qualified. The exact source, release notes, asset identities and original detailed chronology are preserved in [Git history](https://github.com/Infiltrator-Projects/Defragmenter/commits/main) and [published tags/releases](https://github.com/Infiltrator-Projects/Defragmenter/releases). The full pre-cleanup changelog and 17 historical release notes are retained at [v1.8.0-281](https://github.com/Infiltrator-Projects/Defragmenter/tree/v1.8.0-281).

## Unreleased — documentation and manual maintenance

- Reconciled the ZFS capability requirement with the currently enforced analysis-only runtime limit.
- Documented both contiguous and disjoint-free-run generic plan-only algorithms, the OFS pilot boundary and rounded whole-allocation-unit Growth reserves.
- Expanded the installed Defragmenter PDF guide from three to eleven instructional pages while retaining its full GPL appendix and exact package layout; strengthened the manual packaging regression.

## Earlier unreleased documentation consolidation

- Consolidated current ownership, safety, validation and release contracts.
- Removed repetitive historical audit/update narration and superseded release-note duplicates from the active tree. No executable implementation, installer or test changed in this documentation pass.

## 1.8.0-281 — independently hosted filesystem engines

- Expanded integration around authoritative standalone filesystem modules. exFAT and Amiga OFS/FFS defragmentation moved behind verified module dispatch; EXT module identity and isolated-host qualification were strengthened in subsequent main commits.
- Test Media uses independently supplied module interfaces for migrated formats rather than embedded competing format writers.
- Preserved the exact-source and release-governance audit gates. Later source changes require renewed audit qualification; a passing subsystem integration check alone does not qualify the entire release.

## 1.8.0-280 / 278 / 275 — module migration and installed layout

- FAT32 Test Media formatting and population moved to FAT32.so, joining FAT12/16; its independent raw payload verifier remains.
- OFS Defragment, Growth Defrag, Recover and Test Media moved to the OFS.so contracts, with fail-closed module identity/ABI admission.
- Installed exactly two user launchables and moved private workers into `/System/Commands/Defragmenter`, outside the filesystem-module directory.

## 1.8.0-274–267 — native interface, recovery and release discipline

- Refined native target identity, interruption/recovery admission, allocation-map rendering and filesystem qualification.
- Replaced the retired Python GUI/application graph with the native C/C++ desktop and control plane; retained Python only for independent fixtures and black-box tests.
- Strengthened protected-main qualification, restartable single-build publication, exact-source audit baselines, isolated worker testing and public APT observation.

## 1.8.0-266–261 — independent verification and packaging

- Added native UFS2 sacrificial-media creation, package-owned helper qualification and complete byte verification.
- Tightened release-input inventories, Common pinning, source-based `.run` construction and project-wide architecture/safety regressions.
- Corrected Linux block topology, FAT pathname reuse and main-run cancellation/artefact-retention behaviour.

## 1.8.0-251–243 — filesystem and GUI hardening

- Strengthened Btrfs/UFS geometry, Linux journal-path safety, NTFS catalogue/placement performance, EXT descriptor read checks and ZFS analysis bounds.
- Refined Test Media diagnostics, GTK typography, progress/cancellation and small-screen interaction while retaining explicit destructive action controls.

## Earlier development

Earlier alpha implementation, corrections and internal release-by-release notes are preserved in the [original v1.8.0-281 changelog](https://github.com/Infiltrator-Projects/Defragmenter/blob/v1.8.0-281/CHANGELOG.md). That historical file is evidence of previous work; current support and safety claims must come from the implementation, [Validation](docs/VALIDATION.md) and the current audited baseline.
