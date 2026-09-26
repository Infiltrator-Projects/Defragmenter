# Changelog

This changelog records user-visible, compatibility, architecture and validation changes for Defragmenter. Detailed commit-by-commit history remains in Git.

## 1.8.0-214

- Reduce large-volume analysis and staging memory pressure by replacing remaining byte-per-allocation-unit state with packed bitmaps across HFS, Amiga OFS/FFS/SFS/PFS3 and FAT map paths. PFS3 staging now packs its final-free map; FAT stores its four map classifications as independent bitsets rather than one byte per cluster.
- Move destructive Test Media whole-disk identity/system-use decisions onto the shared native block-device layer backed by sysfs, mountinfo, swap state and udev metadata instead of parsing lsblk/findmnt output at the safety boundary.
- Tighten native mapper protocol handling so optional fields default only when absent; present-but-malformed numeric, Boolean or string values fail the protocol instead of silently becoming plausible defaults.
- Replace Test Media's substring-based mapper-result extraction with a bounded structural JSON scanner, cap captured child output at 8 MiB and terminate a child that exceeds that contract.
- Standardise checked dynamic-array growth in remaining native analysis paths and route filesystem transaction sync points through the shared EINTR-safe durable-sync primitive.
- Fix the Test Media Amiga payload builder after the packed AFFS allocation-map conversion: all free-map reads/writes now use the packed bitmap API, eliminating the sanitizer-detected heap overrun.
- Activate the forensic scalability/protocol/safety architecture regression that had accidentally been placed after the script entry point, and update stale regression expectations to the new native safety/sync contracts.
- Qualify source commit `07e731c99c53c59d2f12fad779eef166332699a2`: all 44 hosted CTest tests pass, the hosted ASan/UBSan lane passes, and the self-hosted Linux build/test subset passes; ordinary gates stop only at the deliberate stale-audit lock advanced by this release.

## 1.8.0-213

- Move physical-volume enumeration and first-party identification probes off the GTK main thread. Discovery now produces a detached snapshot and applies only the newest completed generation on the GUI thread, preventing slow block devices or native probes from freezing startup/Refresh.
- Preserve the safe unmount-and-continue mutation path across asynchronous rediscovery: Defragmenter waits for the refreshed device snapshot before deciding whether the selected target is still present and unmounted.
- Make window resizing a presentation-only operation. Resizing rerasterises the already-authoritative physical allocation sample and never launches another filesystem analysis merely because the window dimensions changed.
- Keep Defragment, Growth Defrag and Recover controls disabled until a successful first-party native Analyse has positively verified the physical filesystem identity; the planner continues to enforce the same rule independently.
- Add GUI/service regressions for detached discovery/application, resize-without-rescan and identity-gated mutation controls.

## 1.8.0-212

- Make first-party filesystem code the authority that unlocks physical mutation. Linux filesystem metadata and Test Media GPT labels now provide routing candidates only; Defragment, Growth Defrag and Recover remain disabled until a successful native Analyse proves a compatible filesystem identity.
- Replace the APFS-only/raw-Amiga discovery special cases with one mapper-backed first-party probe path. Readable unknown/Test Media targets can be positively identified during discovery; root-only targets remain visible as mutation-disabled candidates until privileged analysis verifies them.
- Preserve a verified identity across rediscovery only when stable filesystem/partition identity still matches, and reject analyser results that conflict with the selected candidate family.
- Strengthen allocation-map validation in both the native C++ mapper and GTK presentation layer: every cell must account exactly for its physical span and fragmentation/directory overlays may never exceed used allocation.
- Remove the duplicated current-version statement from the repository README and align all documented local builds with the project-wide N-1 CPU policy.

## 1.8.0-211

- Remove the stale source-tree GUI release literal. In-tree launches now read the canonical VERSION file while installed builds continue to consume CMake's generated build metadata.
- Make Open Image identification first-party end to end: the GUI asks the native mapper to probe the image, and the mapper preserves the exact detected filesystem identity (including FAT width and EXT aliases) alongside its backend ID instead of relying on host blkid support.
- Harden destructive Test Media execution. Root-side child processes resolve commands only through trusted absolute system locations, PATH lookup is no longer part of the privileged execution contract, and destructive targets without a stable serial or WWN are rejected rather than treating model/capacity lookalikes as the same disk.
- Enable explicit Linux compiler/linker hardening when supported: strong stack protection, PIE, RELRO and immediate binding. Local and package builds also follow the N-1 CPU policy by default so compilation leaves one logical processor available to the desktop.
- Expand malformed-media qualification with deterministic bounded fuzz-smoke payloads of varied lengths and content across every installed native filesystem identifier.
- Add permanent architecture and end-to-end regressions for first-party image identification, exact probe identity, trusted Test Media execution, source-version ownership, hardening flags and N-1 build policy.
- Extend the release audit boundary to include the destructive Test Media implementation itself, so any future Test Media source drift requires a fresh completed safety audit before publication.

## 1.8.0-210

- Harden destructive Test Media target selection and state handling. System-backed disks are rejected more broadly, preparation is bound to a stable disk fingerprint rather than only a reusable /dev pathname, root-owned qualification state lives under /var/lib, and repartitioning waits for the expected device topology instead of trusting a fixed delay.
- Strengthen the Test Media integrity contract with deterministic boundary-sized payloads around 512-byte and 4096-byte allocation edges, retained directory-test payload verification, fail-closed preparation/verification aggregation, and the corrected APFS/UFS qualification paths.
- Preserve the allocation map's exact monotonic physical ordering while reducing GUI work: resize-driven remaps now use hysteresis, rasterisation paints physical spans instead of repeatedly searching source cells per pixel, and APFS Test Media discovery avoids a redundant AFFS probe.
- Apply the shared parallel-work policy consistently so parallel-capable native work leaves one online logical CPU available to the desktop while storage-specific limits remain independent I/O constraints.
- Broaden malformed-media boundary coverage across native filesystem identifiers and keep the authoritative visual-order regression pinned.
- Qualify production source commit `c047cfa152b238c1b5684cd15fffaadef93e77a8`: Project quality gate run 36216799567 completed the strict C/C++ build, complete native/filesystem/GUI/release test suite and hosted ASan/UBSan lane successfully; self-hosted run 36216799410 completed its dependency check, strict build and native/filesystem/GUI tests successfully. Both candidate runs stopped only at the deliberately stale audited-source control advanced by this release.

## 1.8.0-209

- Fix the Overview layout so the bottom of the screen remains present on laptop work areas. Remove the duplicate miniature "Current allocation" map, compact the activity area into a single horizontal strip and make the authoritative disk map vertically elastic with a 64-pixel minimum instead of forcing a 250-pixel minimum that caused avoidable scrolling.
- Preserve the authoritative map's real physical pixel ordering while allowing GTK to shrink or grow its height with the available work area; map-resolution refresh continues to follow the actual allocated drawing size.
- Complete the SFS0/SFS2 and PFS3 map summary contracts. Their native exact-allocation mappers now emit regular-file/directory counts, fragmented-file count, zero fragmented-directory count for the qualified subset, and a calculated fragmentation percentage rather than leaving the GUI at "Not calculated".
- Add GUI and architecture regressions for SFS/PFS3 fragmentation summaries and for the non-duplicated, shrinkable Overview layout.
- Clarify Linux swap semantics in the UI. Swap uses page slots rather than file extents, so file fragmentation is not applicable whether the swap area is active or inactive; the presentation now says this explicitly instead of leaving the relationship to "mounted" state ambiguous.
- Qualify production source commit `6c066eca248747f7150f84f1e7744caf36199ae2`: all 44 hosted CTest tests passed, hosted ASan/UBSan passed, and the self-hosted functional/architecture subsets passed. The only ordinary-gate stop was the deliberately stale audited-source marker advanced by this release.

## 1.8.0-208

- Fix the APFS failure shown on stale Test Media where an `LD_APFS` GPT partition label was being treated as proof that the partition actually contained an APFS container.
- Stop letting the Test Media slot label override real host filesystem metadata. A stale `LD_APFS` slot that still contains HFS+/other bytes now keeps its real discovered identity instead of being routed to the APFS mapper.
- When Linux leaves the filesystem type blank, require a successful first-party `apfs-native identify` result before exposing an `LD_APFS` slot as APFS. A label-only, unformatted or partially rebuilt slot is no longer advertised as a valid APFS filesystem.
- Keep deterministic OFS/FFS/SFS/PFS3 Test Media label fallback where host blkid support is genuinely absent; the stricter identity rule is applied to APFS because a positive native container probe is available.
- Extend Test Media verification so APFS is checked with the first-party raw APFS verifier instead of attempting to mount it through the host kernel. Missing/corrupt APFS fixture bytes now produce an explicit Test Media verification failure.
- Add regressions covering stale HFS+ bytes under an `LD_APFS` label, positive native APFS identification, and rejection of an unformatted labelled APFS slot.
- Qualify production source commit `21e2936bddab06c0e491f9b0f1167cdf45cd6cc5`: all 44 hosted CTest tests passed, the hosted ASan/UBSan lane passed, and the self-hosted functional/architecture subsets passed. The ordinary candidate gates stopped only on the deliberately stale audited-source marker advanced by this release.

## 1.8.0-207

- Fix the selected-volume dropdown being clipped and effectively unusable under the decorative hero artwork.
- Move the real volume selector, Refresh, Open image and Unmount controls out of the fixed-height hero overlay into a dedicated full-width control panel directly below the hero.
- Keep the hero purely decorative/informational so its raster height can no longer constrain or overlap primary GTK controls or combo-box popup interaction.
- Give the volume combo an explicit useful minimum width while allowing it to expand across the available window width.
- Add a GUI regression and UI-vision invariant requiring primary volume controls to remain outside the hero overlay.
- Qualify production source commit `8913ba5da086fff6971e5bfa91e0bb72349e7009`: all 44 hosted CTest tests passed, the hosted ASan/UBSan lane passed, and the local functional/architecture subsets passed. The ordinary candidate gates stopped only on the deliberately stale audited-source marker advanced by this release.

## 1.8.0-206

- Correct the 1.8.0-205 raster-art implementation error that caused a miniature copy of the full UI concept to appear in the selected-drive badge and left the intended hero/sidebar artwork absent.
- Stop loading and cropping `docs/ui/defragmenter-ui-vision.jpg` at runtime. The concept screenshot is now documentation-only, matching the intended design workflow rather than being treated as a sprite sheet.
- Add three clean standalone JPEG runtime assets under `gui/ui/art/`: `drive-ssd.jpg`, `hero-landscape.jpg` and `sidebar-workbench.jpg`. Each is loaded independently, aspect-preserving, with no prototype controls embedded in the asset.
- Install the dedicated artwork beside the GTK UI modules and remove the full concept screenshot from the application payload.
- Resolve the Defragmenter brand icon from the exact packaged PNG bytes before falling back to the desktop icon theme, preventing an unrelated theme glyph from appearing in the sidebar brand surface.
- Increase the radial summary gauge and headline weight to move the live dashboard closer to the approved target without changing any data semantics.
- Add regressions that reject runtime references to the concept screenshot and verify the exact three standalone raster assets and their package installation contract.
- Qualify production source commit `8efc6dc86b333d309ca6d465450449f9d53f8efa`: Project quality gate run 36129632563 passed all 44 hosted CTest tests and the ASan/UBSan lane; self-hosted run 36129631957 passed its build, functional test subsets and architecture checks. Both ordinary-candidate runs stopped only on the deliberately stale audited-source marker advanced by this release.

## 1.8.0-205

- Remove the large-volume NTFS analysis bottleneck. The $MFT catalogue scanner now reads records in 32 MiB sequential batches instead of issuing one tiny metadata read per file-record segment, retaining the old per-record path only as a damaged-I/O fallback.
- Parse independent NTFS MFT records across **N−1 CPU workers**, leaving one logical processor available to the desktop, and reuse per-worker fixup buffers instead of allocating raw/fixed record buffers for every record.
- Replace the NTFS catalogue's repeated linear owner lookup and stream/object aggregation passes with an open-addressed record index and one indexed aggregation pass, eliminating the pathological growth that made a large historical MFT appear hung while a small NTFS volume completed normally.
- Keep NTFS parsing and mutation semantics unchanged: malformed-record tolerance, attribute-list fail-closed behaviour, movable-stream qualification, hibernation detection, fragmentation accounting, Growth Defrag reserve checks and writer/recovery safety remain authoritative.
- Turn the left rail into genuine navigation rather than duplicate command buttons. Overview keeps convenient quick actions; Analyse, Defragment, Growth Defrag, Recover, Test Media and Settings now open dedicated pages with task-specific information and one relevant primary action.
- Replace the procedural hero landscape, checker sphere and selected-drive drawing with raster artwork taken directly from the approved UI concept. The approved concept image is installed with the application under `/usr/lib/linux-defragger/art/`; gauges and the physical allocation map remain live data visualisations rather than decorative raster substitutes.
- Add permanent architecture/regression guards for 32 MiB NTFS batching, N−1 worker policy, hashed catalogue ownership, raster concept artwork, dedicated navigation pages and the existing exact physical disk-map ordering.
- Qualify production source commit `1d6d609219d360fbd76fc3689d10f7a911940f37` in Project quality gate run 36124937941 and the self-hosted qualification run 36124937769: warnings-as-errors builds, all 44 hosted CTest tests, the local qualification subsets and the separate ASan/UBSan lane pass; both ordinary candidate gates stop only on the deliberately stale audited-source marker advanced by this release.

## 1.8.0-204

- Fix a selected-volume identity regression introduced by the graphical hero: changing the volume combo box now immediately refreshes the hero title, detail line, footer identity and mounted/offline badge from the revalidated `Volume` object.
- Remove the stale-state path where the combo could show one partition while the hero still showed the previously selected partition and its mount state.
- Centralise selected-volume presentation in one `show_selected_volume()` path shared by device-list refreshes and manual selection changes, so all identity surfaces consume the same authoritative display string.
- Parse the explicit trailing `, unmounted` state before `, mounted` so the substring `mounted` inside `unmounted` can never produce a false mounted badge.
- Add a GUI regression that requires manual selection changes to call the selected-volume synchroniser using the revalidated volume identity.
- Qualify production source commit `eb0ef2222e110bba8a4b8b89c41af68e10e09d3d` in Project quality gate run 36120025982: warnings-as-errors builds and all 44 hosted CTest tests pass and the separate ASan/UBSan lane passes; the ordinary candidate gate stops only on the deliberately stale audited-source marker advanced by this release.

## 1.8.0-203

- Correct the allocation-map semantics after the 1.8.0-202 Hilbert experiment: display pixels now preserve exact physical disk order instead of moving allocation units to aesthetically convenient coordinates.
- Map the first physical allocation unit to the top-left pixel and advance monotonically left-to-right, then top-to-bottom, through the real on-disk unit address space. A fully compacted leading allocation region therefore remains one compact leading region on screen instead of being split into unrelated shapes.
- Build the raster at the actual drawing-surface resolution and paint it 1:1 without interpolation, so category boundaries are not shifted or blurred. Tooltip hit-testing uses the same physical-position mapping and returns the exact source cell rendered at that pixel.
- Replace the obsolete grid-geometry helper with pure physical-position mapping helpers and add a regression proving a 0–99 unit disk maps exactly to the 100 display pixels in monotonic order, including the packed-used/free boundary.
- Make the physical-position rule explicit in the UI vision and regression suite: Hilbert, Morton/Z-order and future space-filling curves are forbidden for the authoritative allocation map.
- Qualify production source commit `d11f3085718064121fa8f98ef25aff87617933e4` in Project quality gate run 36118695325: the warnings-as-errors build and all 44 hosted CTest tests pass, and the separate ASan/UBSan lane passes; the ordinary candidate gate stops only on the deliberately stale audited-source marker advanced by this release.

## 1.8.0-202

- Recompose the GTK dashboard much closer to the approved concept: pin the navigation rail to the actual left edge with a Paned layout, move version/status into a decorative selected-volume hero, add Workbench-inspired checker artwork, richer colour-coded navigation and operation cards, a compact current-allocation preview, a live activity surface and a denser footer/status strip.
- Replace the still-blocky Morton allocation image with a Hilbert-curve raster stretched through bilinear filtering so source locality remains deterministic while the map reads as a continuous picture rather than vertical bars or a visible grid; tooltip reverse-mapping remains exact to the originating analysed cell.
- Keep displayed storage metrics factual: radial gauges continue to derive only from analyser-returned fragmentation, free/used allocation and file counts; the decorative hero and checker artwork are data-neutral.
- Make visible dashboard operations responsive rather than inert. Analyse now reports a missing-selection error instead of silently returning; Analyse, Defragment, Growth Defrag and Recover cards remain clickable while idle and delegate validation to the existing controllers so unsupported, mounted, missing-journal and recovery-only cases produce explicit feedback.
- Drive the graphical Activity panel from real runner lifecycle and progress events without changing the native operation engine, mounted-target refusal, transaction journal, recovery or final verification contracts.
- Qualify production source commit `fd0c41471b92fd88a099395a76d9942d8c387086` in Project quality gate run 36115039259: warnings-as-errors builds and all 44 hosted CTest tests pass and the separate ASan/UBSan lane passes; the ordinary candidate gate stops only on the deliberately stale audited-source marker advanced by this release.

## 1.8.0-201

- Replace the first-pass row-major allocation raster that produced diagonal scan-line stripes with a locality-preserving Morton/Z-order pixel image, retaining exact source-cell tooltips while making contiguous disk regions read as a graphical picture rather than a grid.
- Add radial graphical summary gauges, a stronger selected-volume hero, deeper card/gradient hierarchy, a wider 1480×900 work-area-bounded canvas and responsive scrolling so the live GTK interface moves materially closer to the approved GUI-first concept.
- Make the graphical navigation functional: Overview returns to the dashboard top, Test Media launches the installed companion, and Settings opens the persistent System/Day/Night appearance control.
- Keep supported Defragment/Growth Defrag/Recover actions usable when a physical volume is mounted: the GUI now asks permission to unmount it safely, performs and verifies the unmount, refreshes the selected identity and only then enters the existing mutation planner. The native planner and filesystem writers still refuse mounted targets if that UI sequence is bypassed.
- Keep unsupported mutation actions fail-closed and user-visible rather than silently unmounting analysis-only filesystems.
- Qualify production source commit `ff054b617e57482c2ebb6dcae929e79fed191b16` in Project quality gate run 36112649677: the warnings-as-errors build and all 44 hosted CTest tests pass, and the separate ASan/UBSan lane passes. The ordinary candidate gate stops only on the deliberately stale audited-source marker advanced by this release.

## 1.8.0-200

- Begin the graphical Defragmenter dashboard redesign from the approved GUI-first concept: add a left visual navigation rail, selected-volume hero panel, stronger summary hierarchy, large operation cards, compact activity surface and a wider 1240×780 desktop canvas.
- Replace the visible chessboard allocation grid with a continuous pixel-raster renderer while preserving the same authoritative allocation categories and exact tooltip mapping back to source allocation ranges.
- Move the technical operation log behind a collapsed expander so graphical status and progress remain primary while low-level detail stays available on demand.
- Extend the existing Common Day/Night palette and MB Corpo typography into the new dashboard surfaces without introducing a second theme or changing filesystem semantics.
- Store the approved UI north-star image at `docs/ui/defragmenter-ui-vision.jpg` and document the implementation boundary in `docs/UI_VISION.md`; no earlier mockup is retained in that design directory.
- Qualify candidate `477f05032c2ed175950f4f1cba7b29332f0b4b06` in Project quality gate run 36109520528: warnings-as-errors builds and all 44 hosted CTest tests pass, and the independent ASan/UBSan lane passes. The ordinary candidate gate stops only on the deliberately stale audited-source marker advanced by this release.

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
