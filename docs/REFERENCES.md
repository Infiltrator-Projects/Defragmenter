<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Technical references and evidence basis

## Purpose

This catalogue identifies external specifications and platform references used
to interpret filesystem structures and operating-system behaviour. It is not a
claim that every referenced implementation is copied or that all documents are
complete specifications. Defragmenter remains responsible for its own
validation, fail-closed feature gates and tests.

Where a stable public specification is unavailable or incomplete, the project
uses multiple corroborating sources and disposable-image evidence rather than
treating one third-party implementation as infallible.

## Filesystem formats

- **FAT12/FAT16/FAT32** - Microsoft, *FAT32 File System Specification,
  Version 1.03* (2000), together with UEFI FAT requirements where applicable.
  BPB/FAT geometry is validated independently before allocation chains are
  trusted.
- **exFAT** - Microsoft, *exFAT File System Specification*:
  <https://learn.microsoft.com/windows/win32/fileio/exfat-specification>.
  Relevant rules include boot-region checksums, allocation bitmap semantics,
  FAT-chained versus NoFatChain streams and entry-set checksums.
- **NTFS** - Microsoft Windows filesystem documentation plus the Linux-NTFS
  project's public NTFS structure documentation are corroborating references.
  Mapping-pair decoding and Update Sequence Array handling are additionally
  validated against generated and real fixtures.
- **ext2/ext3/ext4** - Linux kernel documentation, *The ext4 Filesystem*:
  <https://docs.kernel.org/filesystems/ext4/>. The native writer also uses the
  system `libext2fs` API in-process; no external mutation command is part of
  the production path.
- **XFS** - Linux kernel XFS documentation and the xfsprogs reference source:
  <https://docs.kernel.org/filesystems/xfs/index.html> and
  <https://git.kernel.org/pub/scm/fs/xfs/xfsprogs-dev.git/>. Mutation is
  restricted to the explicitly validated XFS v5 feature subset.
- **Btrfs** - Btrfs development documentation, *On-disk Format*:
  <https://btrfs.readthedocs.io/en/latest/dev/On-disk-format.html>. That
  document explicitly warns that parts are incomplete/outdated, so it is
  treated as corroborating rather than sole authority. Defragmenter currently
  uses Btrfs only for read-only raw analysis.
- **HFS+ / HFSX** - Apple Technical Note TN1150, *HFS Plus Volume Format*:
  <https://developer.apple.com/library/archive/technotes/tn/tn1150.html>.
- **APFS** - Apple, *Apple File System Reference*:
  <https://developer.apple.com/support/apple-file-system/Apple-File-System-Reference.pdf>.
  Defragmenter currently exposes summary read-only analysis and does not claim
  a write contract.
- **UFS/FFS** - BSD Fast File System literature and current FreeBSD filesystem
  sources are corroborating references. UFS write support is not enabled.
- **Amiga OFS/FFS/SFS** - public format descriptions, first-party deterministic
  fixtures and cross-checks against known images are used together. The original
  SmartFilesystem source is used as the primary implementation reference for
  SFS0 structures; Aaru's independent SFS reader is used as corroborating
  evidence for the SFS2 structure-version-4 differences (48-bit file sizes and
  32-bit extent counts). Because stable normative public specifications are less
  uniform than for FAT/ext4, mutation support is deliberately limited to the
  structures exercised by the audited parsers and fixtures.

## Linux/POSIX platform semantics

The safety model depends on documented Linux/POSIX interfaces rather than on
pathnames as persistent identities:

- Linux man-pages project: `open(2)`, `openat(2)`, `openat2(2)`, `pread(2)`,
  `pwrite(2)`, `fsync(2)`, `flock(2)`, `stat(2)`,
  `proc_pid_mountinfo(5)` and `ioctl(2)`:
  <https://man7.org/linux/man-pages/>.
- Linux kernel block-device/sysfs interfaces:
  <https://docs.kernel.org/>.
- Linux device-mapper `dm-log-writes` target for completed-write/FLUSH/FUA
  replay during crash-consistency qualification:
  <https://docs.kernel.org/admin-guide/device-mapper/log-writes.html>.
- Upstream `log-writes` userspace replay tool:
  <https://github.com/josefbacik/log-writes>.
- SQLite atomic-commit model:
  <https://sqlite.org/atomiccommit.html>.

These references define observable software contracts. They do not remove the
residual risk of faulty storage firmware or a compromised privileged operating
environment.

## Toolchain and dynamic analysis

- AddressSanitizer documentation:
  <https://clang.llvm.org/docs/AddressSanitizer.html>.
- UndefinedBehaviorSanitizer documentation:
  <https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html>.
- LLVM libFuzzer documentation:
  <https://llvm.org/docs/LibFuzzer.html>.
- e2fsprogs `e2fuzz`/filesystem corruption harness, used as corroborating
  testing practice for malformed on-disk structures:
  <https://github.com/tytso/e2fsprogs/tree/master/misc>.

## Referencing practice in source

Source comments should cite an external reference when the reason for a
non-obvious operation is a published format rule (for example, checksum
exclusions or sector-tail fixups). Comments should not paraphrase obvious code.
Project-specific decisions - transaction phases, recovery binding, resource
ceilings and fail-closed policy - should point back to
[DESIGN.md](DESIGN.md), [VALIDATION.md](VALIDATION.md) or the relevant test
rather than presenting implementation convenience as a filesystem requirement.
