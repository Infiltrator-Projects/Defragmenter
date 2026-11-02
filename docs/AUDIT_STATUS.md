# Safety audit status

**Status of the recorded safety audit: complete.** This statement describes the **specific audited baselines below**, not unconditional approval of every subsequent `main` commit or a claim of physical-media qualification.

Status: **complete**

Audited source commit: 3447d1cdd9ccef283b880ff5d55b4d20d41230d2
Audited release-governance commit: 3934c470b7ff3c8411515b37651dc9346c4d99ca

These exact lines are an **executable release-gate contract** used by `defragger/ci/release.sh` and `defragger/tests/test_release_gate.py`. Do not remove, re-label or advance them without the corresponding independently reviewed source/governance audit. The code gate separately checks ancestry and the exact tracked source and governance diff.

## Reviewed incremental performance correction — 2026-11-02

Reviewed source candidate: `3447d1cdd9ccef283b880ff5d55b4d20d41230d2`, directly after approved Defragmenter 1.8.0-282 commit `e7d11e15efc983a08d2b0baf0f726555975eb76d`.

**Review scope:** GitHub's exact source diff changes only `defragger/native/filesystem_module_host.c` and `defragger/tests/test_architecture.py`. No release workflow, package format, filesystem driver source, test-media layout, device opener or module-loading policy changed. No governance-code delta exists beyond the preceding audited governance commit `3934c470b7ff3c8411515b37651dc9346c4d99ca`, which remains the recorded governance baseline.

**Performance bottleneck:** The previous module-host `target_write` called `ld_device_matches_identity` before *every* raw block write. For removable block devices the function can traverse sysfs/udev and `/dev/disk` links to rediscover hardware identity. On Amiga OFS 512-byte writer callbacks, this can dominate all data-transfer time. The new module host binds `bound_identity` and the descriptor's dev/inode/major:minor to the original, mounted-overlap-checked, O_EXCL-opened target. Each write checks the descriptor's current `fstat` identity, original capacity and original immutable identity string. Full hardware identity discovery occurs no less often than every 128 successful writes and before every durability `sync_target` barrier, failing closed on a mismatch. This is a deliberate bounded safety/performance tradeoff: it preserves continuous *open object* checks and periodic *external hardware identity* checks, but is **not identical** to the former full external-discovery-before-every-block policy.

**Safety checks preserved:** `ld_device_try_open` still rejects mounted or conflicting devices and opens writable block devices O_EXCL with O_NOFOLLOW; `target_open` still verifies expected binding/size and obtains the mandatory non-blocking advisory lock. The write host checks `writable`, exact target capacity, kernel opened-object type, st_dev, inode and rdev, and byte bounds before `pwrite`. Sync revalidates full hardware identity before `fsync`. Filesystem-specific format signatures and exact geometry validation are unchanged, and a stale/invalid binding refuses further writes. A true hot-swap within a 128-write window is a residual risk that was previously checked more frequently; hardware qualification remains necessary.

**Regression:** `tests/test_architecture.py` now specifically enforces the bounded refresh threshold, fstat/inode/device checks, immutable identity equality, successful-write accounting, full identity check before sync, and retention of the exclusive-open lock. The pre-existing mounted-image, hard-link, locked 2 GiB OFS fixture, complete filesystem, native-media and ASan/UBSan tests remain intact. Candidate [source quality run 41400237099](https://github.com/Infiltrator-Projects/Defragmenter/actions/runs/41400237099) qualifies this exact source; publication of the subsequent VERSION-only release requires its own successfully completed full gate, without exception.

**Assurance limits:** No real SD-card latency benchmark or physical media write/erase test has been performed by this source review. The original physical OFS formatter failure and observed slowness must be checked on the user-confirmed sacrificial media after the qualified release is installed. A source change and a green synthetic test are not proof of that physical outcome.

## Reviewed source and governance delta — 2026-11-02

Review target (both code and governance): `3934c470b7ff3c8411515b37651dc9346c4d99ca`.
Previous source baseline: `7bccc8fd5e49d1246a5de0b77fda4bc9a073984f`.
Previous governance baseline: `c1216fc232225bf082b2ff0fdaaeca50b360c5f7`.
The exact changes from those baselines to this target were reviewed, including all executable, test, install, pinned dependency and workflow changes—not just a VERSION edit. The current source contains twelve intervening commits from the previous source baseline. The review covers static source-path and control-flow examination plus the evidence below; it does **not** claim independent destructive hardware qualification.

### Reviewed change classes and safety arguments

1. **21-provider destructive preflight:** `test_media_core.c` now checks every registered creator, including the independently owned UFS2 ABI interfaces, before Test Media erases/repartitions the selected disk. Absence, wrong ABI or a missing provider is a pre-write refusal. The partition-identity and affirmative destructive-confirmation safeguards remain in `test_media_worker.c`.
2. **EXT identity and dispatch:** `ext_module_loader.c` validates the exported EXT2/3/4 format ID against the selected module before running a provider. `native/runtime.cpp` exposes separate EXT2/3/4 entries but retains the same qualified native worker. The independent EXT repository workflow adds both wrong-module negative testing and real-file verification from three separate repositories. The EXT4 module test pin changed to a known commit with its required explicit legacy multiformat *test-only* build option; production module ownership and consumer API are unchanged.
3. **OFS Test Media physical-partition admission:** `test_media_worker.c` checks for desktop remounts after GPT settlement, refuses a still-busy mounted partition, and retries **only** when opening the *unmodified* selected test partition exclusively failed with EBUSY; all other module, format, write and flush failures fail closed. Retry is bounded to six attempts; no automatic retry after a partial format. `filesystem_module_host.c` preserves errno on failed exclusive lock and keeps stable-device, mount-conflict, inode and capacity checks. `test_media_ofs_module.c` exposes whether the target open or the OFS V3 formatter failed. `test_test_media.c` includes an independently locked 2 GiB OFS image proving exclusive-open refusal and a fresh successful format after unlock.
4. **Release governance:** Only the pin and tests in `.github/workflows/ext-integration.yml` and the corresponding pinned EXT build steps in `.github/workflows/quality-gate.yml` changed since the previous governance baseline. The immutable-tag handling, source diff gate, release packaging, checksums, SHA-bound publisher and branch/linearity checks remain unchanged; `ci/release.sh` and `tests/test_release_gate.py` have no governance diff.
5. **OFS dependency identity:** `shared/ofs-module-revision` is now pinned to `01f5bdadadda9f847be2d69d9e17480cd370df0b`, the published 0.1.4 OFS engine used for the physical Test Media reproduction. This eliminates the previous qualification mismatch where Defragmenter used a pre-performance-fix OFS version. The upstream `Filesystem-OFS` 0.1.4 release's own module, EFI, Boot and kernel qualification succeeded ([OFS run 41364210473](https://github.com/Infiltrator-Projects/Filesystem-OFS/actions/runs/41364210473)); no filesystem data-format or journal ABI change was introduced by this Defragmenter pin.
6. **Tests and packaging:** The change adds negative cases and independent-identity tests without deleting the canonical fixtures, weakening assertions, changing package ownership or changing external-program prohibitions. The reviewed source delta does not relax filesystem write eligibility or bypass the .so trust boundary.

### Test evidence and limits

- Exact pre-pin Defragmenter source `959c838193388dcee73ef4bf2a8161ba0d3c407f`: [quality run 41368579311](https://github.com/Infiltrator-Projects/Defragmenter/actions/runs/41368579311) passed **42/42 core** tests, **16/16 native Test Media** tests and self-hosted **AddressSanitizer/UndefinedBehaviorSanitizer**; 14 independent filesystem-integration workflows passed. Its release-invariant stage correctly refused publication because these reviewed baseline markers had not yet been advanced.
- Exact pinned release-source `3934c470b7ff3c8411515b37651dc9346c4d99ca` changes only `shared/ofs-module-revision` beyond that pre-pin commit. Its fresh consumer qualification and the next versioned release are separately required to pass the unchanged self-hosted gate. Recording this review does **not** bypass or pre-approve a failed future build.
- Upstream OFS's reproducible sparse 2 GiB DOS0 formatter case passed its own quality workflow [run 41367611155](https://github.com/Infiltrator-Projects/Filesystem-OFS/actions/runs/41367611155); that does not prove the original SD-card automount race is the sole cause.
- Physical `/dev/mmcblk0p11` after these *Defragmenter* changes remains **untested**. Device-automount contention is the targeted, plausible cause of the prior OFS format failure, not a proven diagnosis. Only a controlled run of the corrected application using the installed OFS 0.1.4 can establish the final hardware result.
- Source review was performed using repository diffs, relevant implementation paths and recorded independent CI/sanitizer evidence. It is **not** a claim of an external human audit, live device or power-loss testing.

The independently enforced release gate still requires an exact clean source/governance diff from these reviewed commits, protected linear history, a separately qualified candidate, artifact identity and release publication checks. Any subsequent tracked executable, test, dependency, installer or governance modification requires another reviewed baseline update.

## Safety invariants

- Every new write requires qualified native filesystem analysis, exact format/geometry, stable device identity and capacity, mounted-overlap refusal and a supported plan.
- Unsupported features, inconsistent metadata, unknown target identity or a required missing/untrusted/ABI-incompatible `.so` cause **pre-write refusal**. No private-engine fallback.
- Recovery material and verified staging are persisted before authoritative source mutation can require rollback; transaction phases are checked and Stop is honoured at safe/recoverable boundaries.
- Recover can be admitted from a valid pre-existing user-bound journal even when fresh source analysis is impossible. The journal, target identity, staged bytes and phase are validated by the actual native worker.
- Final success requires reopening the result and checking canonical placement, exact Growth reserve where available, structural integrity and complete logical payload identity.
- Filesystem-neutral Linux topology checks distinguish partition parents, holders and whole/stacked-device slaves; hard-link aliases cannot bypass image mount checks.

These invariants apply across the bounded FAT12/16/32, exFAT, NTFS, EXT2/3/4, XFS v5, Amiga OFS/FFS/SFS/PFS3, HFS/HFS+, Btrfs, APFS, Minix and UFS variants wherever write operations are admitted. Special format states not explicitly qualified fail closed. **ZFS/OpenZFS and swap remain analysis-only.**

## Filesystem module and Test Media boundaries

The intended single-owner design is documented in [Filesystem Engine Ownership](FILESYSTEM_ENGINE_OWNERSHIP.md). Migrated SO-backed production workers must execute the actual external module with trusted path/version/ABI checks; test environments use isolated SO roots. FAT and OFS Test Media formatting/population are module-owned. Migration may not quietly retain a second production formatter, writer or journal engine.

The 21 Test Media slots must keep their independent first-party C creator/raw verifier, deterministic 200 MiB plus boundary-file corpus, schema 3 disk-identity binding, full-byte verification and corruption refusal. A successfully formatted image or a saved results file alone is not evidence of correctness.

## Release and physical assurance

Development uses direct-to-protected `main`. A `defragger/VERSION` change requests a release; ordinary documentation updates do not. Both permanent self-hosted quality/sanitizer lanes, exact source/governance baseline comparisons and protected linear history must qualify the same intended commit. Publication uses only its once-built SHA-bound `.deb`, `.run` and manifest; version identity and digests are write-once. Public APT propagation is observed separately.

Synthetic, sanitizer and dm-log-writes tests are important evidence but are **not** physical-media power-loss proof. No completed source audit, published release or passing test alone claims universal on-disk write safety.

## Historical audit trail

The original 47 KB chronological audit diary, reviewed intermediate SHAs and release-by-release notes remain available at [v1.8.0-281/docs/AUDIT_STATUS.md](https://github.com/Infiltrator-Projects/Defragmenter/blob/v1.8.0-281/docs/AUDIT_STATUS.md), with the exact immutable history in Git. This maintained document contains the active audit identities, current known qualification gap and durable safety contract, not repeated superseded baseline announcements.
