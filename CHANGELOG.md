# Changelog

This changelog records user-visible, compatibility, architecture and validation changes for Defragmenter. Detailed commit-by-commit history remains in Git.

## Unreleased

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
