# Changelog

This changelog records user-visible, compatibility, architecture and validation changes for Defragmenter. Detailed commit-by-commit history remains in Git.

## 1.8.0-199

- Correct SFS2 metadata checksum handling to use the format-specific checksum convention on both validation and rewrite paths; fix the independent SFS2 fixtures so they no longer reproduce the old SFS0 checksum error.
- Enforce the documented 107-character SFS/SFS2 object-name limit and reject malformed control/colon name bytes before catalogue data is trusted for relocation.
- Require classic Amiga OFS/FFS root bitmap-valid state before allocation is trusted, and fail closed on DOS\\6/DOS\\7 long-name media until their altered on-disk namespace structures are independently implemented.
- Harden the bounded PFS3 reader/writer contract with root-extension roving/delete-directory/filename-limit validation and directory-entry extension-tail/comment bounds checks.
- Add regressions for SFS2 checksum separation, overlength SFS names, invalid Amiga bitmap state, DOS\\6 rejection, malformed PFS3 extension geometry and overlapping PFS3 directory-extension tails.
- Qualify production source commit `963c8500b87a90a82f619e9abecdbbafc5f48cfb`: the warnings-as-errors build and all 44 hosted CTest tests pass at candidate `4bdce31c7b6e9ff995b9320303d7d8f17ebd3ed0`, and the separate ASan/UBSan lane passes; the pre-release gate stops only on the deliberately stale audit-baseline marker advanced by this release.

## 1.8.0-198

- Preserve privileged-helper protocol order through one FIFO GTK-main-loop dispatch queue so successful completion cannot overtake and discard the preceding allocation-map JSON.
- Add a deterministic physical-device NTFS analysis regression that defers GUI scheduling and proves mapper output is delivered before the `finished` message.
- Qualify the fix at commit `aa7b2675e6f945b6e41a6f2a5b65c1cff7fc7c9b`: all 44 hosted CTest tests pass and the separate ASan/UBSan lane passes in Project quality gate run 36098340286; the pre-release gate stops only on the deliberately stale audit-baseline marker updated by this release.
- Keep the NTFS filesystem engine unchanged; the regression was in the privileged GUI transport between the already-correct mapper output and its consumer.

## 1.8.0-197

- Centralise Unicode scalar-to-UTF-8 byte encoding through Infiltratr Common 1.19.27 for the native JSON parser and FAT/exFAT filename paths, removing their independent byte encoders.
- Keep filesystem-specific Unicode policy local: FAT still sanitises path separators/NUL and replaces malformed UTF-16, while exFAT now maps unpaired UTF-16 surrogates to U+FFFD before the shared strict encoder.
- Advance the exact Common dependency to released 1.19.27 at commit `3ef3710df6563df305b6d8e2dc9d1a41c61843ba` across the gitlink, CMake, installer, tests and maintained documentation.
- Qualify the production-source baseline at `e9a341b08b93d624f98df9a9ec0d7bc4842d9d7b`: warnings-as-errors builds and all 44 hosted CTest tests pass, and the separate hosted ASan/UBSan lane passes; the pre-release gate stops only on the deliberately stale audit-baseline marker that this release commit advances.

## 1.8.0-196

- Align GTK button height with the 30 px desktop suite control contract while retaining Common's existing compact radius.
- Re-run the complete 44-test hosted quality suite and ASan/UBSan lane; all functional and sanitizer checks pass.
- Advance the exact audited production-source baseline to the reviewed presentation-only commit without changing filesystem engines, write safety, dependencies or Common APIs.

## 1.8.0-195

- Correct the Common 1.19.24 Git submodule at the actual project dependency path and remove the stray root entry that prevented recursive checkout.
- Restore executable modes on the local installer builder and release-gate script.
- Verify the committed dependency path and revision against the CMake requirement, preventing a partial pin update from appearing complete.
- Record the incremental dependency/packaging review and its exact 44-test and sanitizer evidence; final release publication remains conditional on the complete gate.

## 1.8.0-194

- Replaced the C++ allocation mapper's private uint64 overflow multiplication logic with the exact pinned Common `infiltratr_u64_multiply_checked()` contract while retaining Defragmenter's exception/context policy.
- Removed the unreachable legacy APFS map adapter and its stale summary-only `spaceman allocation map not yet decoded` path; APFS remains routed through the qualified first-party native exact-map contract.
- Removed the mapper string helper made obsolete by that dead APFS path and added a direct architecture regression that rejects reintroduction of either the private arithmetic implementation or legacy APFS adapter.
- Re-ran the complete hosted 44-test suite, warnings-as-errors build, architecture/release invariants and ASan/UBSan qualification against the updated production baseline.

## 1.8.0-193

- Replaced the production EXT2/EXT3/EXT4 libext2fs dependency with a bounded first-party native on-disk engine owning superblock/group-descriptor validation, allocation bitmaps, inode scanning/checksums, extent and legacy-indirect traversal, allocation accounting and physical-reference mutation; e2fsprogs/libext2fs remains test-only independent fixture/oracle evidence.
- Completed UFS1/UFS2 qualification and enabled the bounded clean/non-journalled/snapshot-free native Defragment, exact 10% Growth Defrag and Recover contract with genuine makefs integration coverage.
- Replaced ZFS summary-only detection with bounded exact native analysis across labels/uberblocks, packed-XDR topology, MOS/dnodes, supported checksum/compression, metaslab space maps and regular-file physical fragmentation; ZFS mutation remains deliberately outside product scope.
- Advanced the exact Infiltratr Common dependency to released 1.19.23 at commit `a9cf2957cffeefe6001830916b8a32c2ef58a551` across the submodule, CMake, installer, tests and current architecture/design/validation documentation.
- Fixed release-qualification defects exposed by warnings-as-errors and sanitizers: EXT checked-arithmetic integration and opaque handle ownership, UFS1 zero rotational-offset division, and bounded ZFS string copying.
- Restored the ZFS multi-uberblock selection fixture so candidate counting and same-TXG timestamp tie-breaking are both exercised rather than weakening the assertions.
- Clarified UFS, ZFS, APFS and Btrfs native ownership/fail-closed boundaries in source comments without duplicating filesystem specifications or narrating syntax.
- Standardised Defragmenter About on the shared native GTK presentation contract and realigned XFS/NTFS/Common ownership regressions with the current source decomposition.

## 1.8.0-192

- Tightened allocation-map detail typing at the GTK/native-manifest boundary so strict Pyright validation no longer treats validated detail dictionaries as nullable.
- Restored executable Git modes on the aggregate test harness, packaging builder and directly runnable regression scripts after the source migration, preventing hosted CTest and release packaging from failing with permission-denied errors.
- Removed the accidental home-runner dependency from release qualification: the permanent full project quality gate now runs on GitHub-hosted Ubuntu alongside the hosted sanitizer lane, while the self-hosted local-quality workflow remains optional manual evidence.
- Restored explicit durable cleanup for incomplete NTFS staging images after the transaction-component split, keeping pre-journal failure cleanup worker-local while persistent journal-owned cleanup stays in `ntfs_transaction.c`.
- Completed the NTFS journal split by moving durable stage/plan/WAL/SHM cleanup into the transaction component and restoring worker-local numeric option parsing, fixing the warnings-as-errors sanitizer build after the refactor.
- Began semantic decomposition of oversized filesystem sources by moving NTFS persistent-journal representation, parsing and durable publication into a dedicated transaction component; placement, commit ordering and Recover policy remain in the worker.
- Clarified the transaction architecture after the target-binding extraction: the native core shares only canonical target/object/capacity mechanics, while filesystem-specific volume identity, phase meaning, placement and recovery semantics remain owned by each filesystem engine.
- Extended the shared one-open transaction target binding across every write-path family except XFS, whose local probe deliberately retains an extra mounted-related-device assertion; filesystem-specific volume identity and recovery policy remain local.
- Began the shared transaction-mechanics consolidation without centralising filesystem semantics: all native worker-level transaction target snapshots now use one filesystem-neutral identity formatter backed by the verified LdDevice contract, descriptor-based exFAT staging uses the same identity representation, and identity comparison delegates to that shared formatter.
- Removed the final installed Python filesystem-capability contract: GTK now enables operations directly from the immutable native C++ registry manifest, and package installation carries only the GTK/core Python presentation modules.
- Made the installed UFS worker itself fail closed on Defragment, Growth Defrag and Recover while UFS remains an unfinished roadmap item; developmental mutation code can no longer be reached simply by bypassing the read-only GUI/native registry capability declaration.
- Corrected the Debian package description so Btrfs, classic HFS, APFS and Minix are no longer incorrectly described as analysis-only.
- Added permanent architecture and genuine-makefs regressions for the UFS production qualification boundary.

## 1.8.0-191

- Standardised Defragmenter artwork on the canonical `#00ADEF` non-automotive Infiltrator icon family while retaining the single-byte-source launcher/taskbar/About packaging contract.
- Hardened root-anchored trusted-directory traversal with Linux `openat2()` using `RESOLVE_BENEATH`, `RESOLVE_NO_SYMLINKS` and `RESOLVE_NO_MAGICLINKS`, while preserving the established `openat(O_NOFOLLOW)` fallback for older kernels.
- Removed the obsolete second GTK About implementation from the base window view; the LINK-standard About presenter is now the single concrete About/licence/icon implementation.
- Added a deterministic fail-closed malformed-media matrix across all 15 installed native filesystem identifiers, including classic HFS, rejecting hangs, signal termination and accidental identification of empty/truncated/seeded garbage; the matrix is a first-class CTest so it also runs under the hosted ASan/UBSan qualification lane.
- Added an opt-in `dm-log-writes`/`replay-log` sacrificial-media harness for validating source-filesystem structure at each FLUSH or FUA durability boundary without conflating that evidence with external recovery-journal persistence.
- Advanced the exact released Infiltratr Common pin to 1.19.22 while retaining the existing filesystem-specific mutation/recovery ownership boundary.

## 1.8.0-190

- Kept the approved 1.8.0-188 Defragmenter artwork exactly: the flat graphite/cyan block-compaction icon remains the canonical application identity for About, window, Cinnamon taskbar, hicolor, Mint app-install and private runtime use.
- Reverted the unpublished 1.8.0-189 icon experiment before release; no alternate disk/cylinder artwork is shipped by Defragmenter.
- No filesystem engine, safety, placement, recovery or Common integration behaviour changed in this release.

## 1.8.0-188

- Replaced the restored legacy glossy Defragmenter artwork with a flat 96×96 project-owned icon deliberately aligned to the System Monitor/Infiltrator desktop family: dark graphite tile, #00ADEF cyan linework and a simple block-compaction defragmentation glyph.
- Kept the 1.8.0-186 GTK/GLib/X11 identity repair and the 1.8.0-187 complete PNG structural validation, so the new artwork is used consistently by Cinnamon taskbar matching, the main window, About, hicolor, Mint app-install metadata and the private runtime copy.
- Moved the hicolor installation contract to the matching 96×96 directory and added regressions that reject stale 128×128/256×256 icon paths while pinning the exact new artwork Git blob.

## 1.8.0-187

- Fixed the actual remaining icon failure: the 256×256 PNG introduced in 1.8.0-179 was structurally truncated. Its IHDR looked valid, but its IDAT chunk declared 51,276 bytes while the entire file was only 12,849 bytes, so GdkPixbuf correctly rejected it and GTK fell back to the missing-image glyph.
- Restored the last known structurally valid project-owned black/cyan Defragmenter artwork (128×128, Git blob `2da0939b9e68cbea9fee31358a2a9d130ad21e15`) and returned the hicolor install path to the matching 128×128 directory while retaining the private runtime and Mint app-install copies.
- Strengthened branding validation to parse the complete PNG container, verify every chunk stays within EOF, validate each chunk CRC, require IEND at the physical end of file and pin the exact restored artwork blob. Header-only PNG checks can no longer certify a truncated icon.
- Retained the 1.8.0-186 GTK/GLib/X11 identity repair so Cinnamon taskbar matching and the About/window pixbuf path now operate on valid artwork rather than a corrupt asset.

## 1.8.0-186

- Fixed the Cinnamon taskbar identity at its source: the GTK process now publishes the installed `io.github.linuxdefragger` identity as its GLib program name and X11 program class, while the desktop entry declares the matching `StartupWMClass`.
- Routed process-wide, main-window and About-window icons through the same directly loaded project-owned pixbuf, retaining the canonical icon-name fallback only when direct artwork loading is unavailable.
- Kept the About dialog's GTK3 named-logo override cleared before assigning the project pixbuf, and added a canonical-name fallback for damaged/source-tree environments.
- Added permanent regressions for the Cinnamon WM-class/desktop-entry match, direct pixbuf window icons, About-window icon application and release packaging identity.

## 1.8.0-185

- Replaced the release pipeline's event-only `workflow_run` handoff with a direct reusable-workflow dependency of the two-lane Project quality gate, so a successful retry also reruns its dependent publication job.
- Chained APT refresh directly from successful immutable release publication and retained manual `workflow_dispatch` only as a retry path; the APT workflow now verifies the requested release tag resolves to the exact released SHA before publication.
- Added permanent release-contract regressions that reject reintroduction of `workflow_run` handoffs for either release or APT publication.

## 1.8.0-184

- Fixed the About dialog's missing-image placeholder by clearing GTK3's default `logo-icon-name` before assigning the approved Defragmenter pixbuf; GTK3 gives the named-icon property precedence over `logo`.
- Kept the existing deterministic private/hicolor/source artwork resolver and the single approved 256×256 icon asset; no substitute artwork or duplicate icon payload was introduced.
- Added a regression that requires the GTK3 precedence override to occur before `dialog.set_logo(logo)`, preventing the same placeholder regression from returning.

## 1.8.0-183

- Completed a second forensic Common 1.19.10 reuse pass without moving filesystem semantics or recovery policy out of Defragmenter.
- Replaced private key=value splitting in FAT, AFFS, SFS, XFS, EXT, exFAT, NTFS and HFS+ recovery/relayout readers with Common's allocation-free configuration-line parser.
- Replaced private lexical path joining in Test Media and exFAT catalogue construction with Common's POSIX path-join contract, and made Defragmenter's allocating suffix adapter delegate byte-exact concatenation to Common.
- Added architecture regressions that reject reintroduction of private journal splitters and path-join duplicates where Common owns the exact mechanism.
- Retained local ASCII case-folding, mount/device topology, filesystem structures, transaction fields, target safety, placement and recovery decisions where Common either deliberately exposes no matching public contract or the behaviour is domain-specific.

## 1.8.0-182

- Advanced the exact Infiltratr Common dependency from 1.19.8 to released 1.19.10 at commit `33e69c0a462b56d388881d89c4eb49f72fa0b0fe` across the submodule, CMake build and local installer.
- Regenerated the complete Common Day/Night adapter and mapped the new titlebar, connection, heading, summary, kicker, detail-label, note, status-border and hover-accent roles into the main GTK application and Test Media.
- Removed the About dialog's private Night colour constants so About follows the same Common-resolved Day/Night palette as the rest of Defragmenter.
- Removed the unused Python transaction/journal durability layer and unused Python raw-write/sync path; production mutation continues through the native writers using Common exact I/O and durable file publication/removal.
- Strengthened architecture, GUI and release regressions so the exact 1.19.10 pin, full generated palette, semantic-role consumption and read-only Python compatibility boundary cannot silently drift.

## 1.8.0-181

- Replaced the single hard-coded runtime icon path with one deterministic resolver shared by the About dialog, process-wide GTK defaults and each Defragmenter window.
- The resolver now tries the packaged private artwork, the installed 256×256 hicolor artwork and the source-tree artwork before falling back to the icon theme, preventing the missing-image placeholder seen when one installation path is unavailable.
- Debian package construction now fails if the private application icon, hicolor desktop icon or Mint app-install icon is missing or differs byte-for-byte from the approved artwork; the package regression exercises that contract.

## 1.8.0-180

- Advanced the exact Infiltratr Common dependency from 1.19.6 to released 1.19.8.
- Replaced C++ JSON real conversion with Common's deterministic locale-independent finite-decimal parser and hardened local string allocation with Common checked size arithmetic.
- Extended the generated GTK design adapter to consume Common typography roles and neutral structural metrics; the allocation-map placeholder now uses the same generated typography identity.
- Moved Test Media typography and shared radius identity onto Common's native design API without changing Defragmenter-specific presentation or filesystem behaviour.
- Removed duplicated MB Corpo source/hash constants from Defragmenter packaging; font provenance and verification are now read from the exact pinned Common design contract.
- Strengthened architecture, GUI and release regressions so these Common ownership boundaries and the exact 1.19.8 pin cannot silently drift.

## 1.8.0-179

- Restored the exact 256×256 Defragmenter artwork previously supplied for the desktop and About presentation.
- Installed the artwork in the matching 256×256 hicolor directory while retaining the same asset for Mint app-install metadata and the private About/window copy.
- Added a regression that pins the approved Git blob and PNG dimensions so a substitute icon cannot silently replace the application artwork again.

## 1.8.0-178

- Enforced the three-face MB Corpo typography contract across the main Defragmenter UI and the separate Test Media utility.
- Removed generic host-font fallbacks and the Test Media log's forced system monospace setting; normal text now stays on MB Corpo S and application titles stay on MB Corpo A.
- Removed runtime font-family probing from both interfaces because supported packages already install and register the verified MB font bundle.
- Added a permanent GUI regression that rejects generic/system font escape hatches and verifies all three packaged MB font files remain part of the release contract.

## 1.8.0-177

- Fixed XFS Defragment/Growth Defrag when a valid relayout needs more bnobt/cntbt/rmapbt blocks than the allocation group already has in its tree/AGFL reserve.
- Allocation-tree growth now borrows only blocks that are already free in the source and remain free in the final plan, while excluding file targets and exact Growth Defrag reserve runs.
- Regenerated XFS AG-owner reverse mappings from the final allocation-tree block set instead of retaining stale source tree ownership when tree blocks move.
- Strengthened final XFS verification for allocation-tree/AGFL exclusion, AGF tree counters and AG-owner rmap ownership.
- Added a permanent white-box regression reproducing the observed 40-tree-block requirement with only 13 initially reserved blocks.

## 1.8.0-176

- Closed the mounted-image hard-link bypass in direct mount-source and loop-backing identity checks.
- Preserved generic Linux FAT identification until native geometry probing selects FAT12, FAT16 or FAT32, restoring image analysis without guessing FAT32.
- Routed all GUI image mutations and Recover through the privileged helper so they can use the protected root-owned recovery namespace.
- Sized bounded native map capture for the GUI's full cell range, including maps larger than 64 MiB; rejected requests beyond 1,048,576 cells.
- Made Stop interrupt silent analysis, retained writer stops queued during startup, isolated child stdin and made follow-on requests safe as soon as completion arrives.
- Added native live-child supervision tests for Stop, queued Stop, GUI control loss, broken output and child reaping, plus mounted-alias, generic-FAT, large-map and GUI privilege regressions. Corrected the documented release-protection contract.

## 1.8.0-175 and preceding C++ migration

- Fixed a 1.8.0-173 regression where the C++ allocation mapper incorrectly required the generic schema from the FAT12/FAT16/FAT32 worker even though FAT intentionally retains its established cluster-map contract; FAT analysis now has an explicit validated adapter and a real FAT12 end-to-end regression fixture.
- Moved the production allocation mapper, operation dispatcher and privileged helper session into the selective C++17 application-service layer while retaining the established native C filesystem engines.
- Hardened privileged shutdown: the helper now uses `posix_spawn` with a dedicated process group, survives a closed protocol pipe long enough to request cooperative Stop, and waits for the privileged writer to exit.
- Strengthened the C++ mapper's fail-closed schema, identity, geometry and accuracy validation and qualified translated maps against real EXT, NTFS, exFAT, XFS, Amiga OFS/FFS and HFS+ fixtures.
- Corrected exact-bound process-output accounting, hardened mountinfo escape parsing, and declared the C++ runtime dependency in Debian package metadata.
- Extended release auditing so `defragger/native/` is part of the immutable production-source baseline.

- Aligned the documentation roles with Calendar and System Monitor: architecture now owns system contracts, design is concise rationale, validation records evidence boundaries, and Audit Status records only the current safety case.
- Removed duplicated product/manual material from the source-tree README and retained historical audit development in Git/release history instead of the current-state audit.

## Recording policy

Record additions, removals, behavioural fixes, compatibility changes, dependency changes that affect consumers, and material validation/release changes. Pure refactoring needs an entry only when it changes maintenance or portability expectations.

## Historical releases

Existing Git tags and GitHub Releases remain the authoritative identity for exact historical source and release assets. Do not reconstruct detailed historical claims here without evidence from those immutable records.
