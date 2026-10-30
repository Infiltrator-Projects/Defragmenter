# Defragmenter: filesystem implementation authority

**Target: one filesystem, one complete `.so`, every consumer.** The [authoritative architecture](https://github.com/Infiltrator-Projects/Filesystems/blob/main/docs/UNIFIED_FILESYSTEM_MODULE_ARCHITECTURE.md) requires each filesystem repository to distribute a single trusted file such as `/System/Filesystems/FAT12.so`. BOOT, the kernel and applications, including Defragmenter, must use the same format engine. The `.so` is **the filesystem driver**, not an optional second implementation. Platform translation belongs to generic loaders.

## What Defragmenter owns

The current transition is explicit: Boot loads the same SO through its generic
ELF/UEFI bridge; Linux still uses the separate native KO. A future generic kernel
SO bridge is not a prerequisite for Boot or Defragmenter and is not implemented.

Defragmenter owns its user interface, filesystem-neutral device identity/safety policy, mount/refusal checks, operation scheduling, progress/Stop handling, privilege isolation, bounded host raw I/O, relocation *orchestration* and independent post-operation verification. It **does not own separate FAT12, FAT16, EXT or Amiga filesystem parsing, allocation, placement, journaling, formatting or repair engines** once their corresponding authoritative `.so` is qualified and integrated.

The worker loads the correct independently installed filesystem module by verified identity and versioned ABI. Format-specific algorithms, relocation steps, cluster allocation and recovery semantics run in that module. The host supplies narrowly scoped capabilities; module privileges are gated by target identity, permissions, operation and cancellation. Do not silently use a private engine when a required module is missing, incompatible or fails to load.

## Migration and proof

OFS analysis, Defrag, Growth Defrag and new-journal Recover use OFS.so's
process-hosted Defrag V2 interface. The filesystem-neutral host supplies bounded
I/O, stable target identity, mounted-overlap refusal, locks, private durable
files, SHA-256 and Stop; it owns no OFS parser or placement policy. Missing or
incompatible OFS.so is an error. FFS keeps its separate transitional engine.
Previously issued AFFS journals retain their exact recovery reader; this is
never a fallback for a new OFS operation.

Test Media calls OFS.so V3 for OFS formatting and file/directory population.
Its deterministic dataset remains application-owned, and its retained AFFS
read-only payload verifier is an independent oracle. The privileged prepare
worker rechecks the required SO before any repartitioning.

FAT12/16/32 Test Media calls the matching SO's V4 formatter and V3 file
APIs. Interleaved appends manufacture the existing 200 MiB fragmented corpus
and eight boundary files without writing FAT or directory records locally.
The independent raw reader follows the module's actual data-area offset;
filesystem structures need not begin on an absolute cluster-size boundary.
Readiness and privileged preparation reject missing/incompatible modules before
erasing signatures or partitioning. FAT32's local FAT and directory writer has been retired; Test Media keeps
only the independent read-only payload verifier.

Existing internal filesystem engines and test-media writers can remain **temporary reference/compatibility implementations** until the matching `.so` covers their verified functions. Removing them before equivalence tests risks data loss. A user-visible successful defrag is **not** proof that the `.so` was called: instrument and test actual `dlopen`/ABI dispatch, reject wrong format, compare independent before/after allocation maps and filesystem integrity, check Stop/rollback and exercise files across multiple clusters, names and failure modes.

Defragmenter Test Media uses the authoritative module's format/create/populate APIs when qualified but keeps independently implemented disposable-image verification oracles. The final release must not ship a second competing filesystem-specific engine or claim a fallback is equivalent without independent tests. Historical releases are described as they were actually built.
