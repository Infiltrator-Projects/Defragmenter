/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef INFILTRATOR_OFS_V3_H
#define INFILTRATOR_OFS_V3_H
#include <stdint.h>
#include <stddef.h>

#define IFS_OFS_ABI_V3 3u
#define IFS_OFS_READ_ONLY 1u
#define IFS_OFS_ALLOW_DIRTY 2u /* Explicit inspection/recovery; normal writes stay blocked. */
#define IFS_OFS_FILE (-3)
#define IFS_OFS_DIRECTORY 2
#define IFS_OFS_ROOT 1
#define IFS_OFS_SOFTLINK 3
#define IFS_OFS_LINKFILE (-4)
#define IFS_OFS_LINKDIR 4
#define IFS_OFS_TIME_CHANGED 1u
#define IFS_OFS_TIME_VOLUME 2u
#define IFS_OFS_TIME_CREATED 4u
#define IFS_OFS_OK 0
#define IFS_OFS_END 1
#define IFS_OFS_NOT_FOUND 2
#define IFS_OFS_INVALID (-1)
#define IFS_OFS_IO_ERROR (-2)
#define IFS_OFS_CORRUPT (-3)
#define IFS_OFS_NO_SPACE (-4)
#define IFS_OFS_EXISTS (-5)
#define IFS_OFS_ACCESS (-6)
#define IFS_OFS_NO_MEMORY (-7)
#define IFS_OFS_CAP_FULL_RW UINT64_C(1)
#define IFS_OFS_CAP_FORMAT UINT64_C(2)
#define IFS_OFS_CAP_METADATA UINT64_C(4)
#define IFS_OFS_CAP_LINKS UINT64_C(8)
#define IFS_OFS_CAP_VERIFY UINT64_C(16)
#define IFS_OFS_CAP_RECOVER_BITMAP UINT64_C(32)
#define IFS_OFS_CAP_ALL_BLOCK_SIZES UINT64_C(64)
#define IFS_OFS_CAP_UNSIGNED_KEYS UINT64_C(128)

/* Native Amiga DateStamp: days from 1978-01-01, minutes since midnight,
 * ticks at 50 Hz since the beginning of that minute. No timezone conversion. */
typedef struct {
    uint32_t days, minutes, ticks;
} IfsOfsDate;
typedef struct {
    uint32_t structure_bytes, flags;
    void *context;
    uint64_t volume_bytes;
    int (*read_at)(void *, uint64_t, void *, uint32_t);
    int (*write_at)(void *, uint64_t, const void *, uint32_t);
    int (*flush)(void *);
    /* Writable mounts verify ownership before enabling allocation: bitmap/
     * system indexes plus four volume-sized bitsets and traversal storage.
     * ALLOW_DIRTY mounts require explicit successful recovery before writes.
     * The read-only Boot view uses bounded internal buffers without a heap. */
    void *(*allocate)(void *, size_t);
    void (*release)(void *, void *);
    int (*clock)(void *, IfsOfsDate *);
} IfsOfsIoV3;
typedef struct {
    uint32_t structure_bytes;
    uint32_t block_bytes;     /* Zero detects a unique validated geometry. */
    uint32_t reserved_blocks; /* Zero selects the standard two boot blocks. */
    uint32_t root_block;      /* Zero uses the stored boot root or canonical midpoint. */
} IfsOfsMountV3;
typedef struct {
    uint32_t structure_bytes, block_bytes, reserved_blocks, variant;
    const char *label; /* UTF-8, representable in Amiga Latin-1. */
} IfsOfsFormatV3;
typedef struct {
    uint32_t structure_bytes, block, original, bytes;
    int32_t type;
    uint32_t protection;
    uint16_t uid, gid;
    IfsOfsDate changed, volume_changed, created;
    char name[321], comment[241]; /* UTF-8; classic names are 30 Latin-1 bytes. */
} IfsOfsEntryV3;
typedef struct {
    uint32_t structure_bytes, block_bytes, blocks, root, variant;
    uint64_t total_bytes, free_bytes;
    uint32_t bitmap_valid;
} IfsOfsSpaceV3;
typedef struct {
    uint32_t structure_bytes, errors;
    uint64_t referenced_blocks, free_blocks, bitmap_mismatches;
    uint32_t first_bad_block, reserved;
} IfsOfsCheckV3;

typedef struct {
    uint32_t abi_version, structure_bytes;
    const char *format_id;
    uint64_t capabilities;
    int (*mount)(const IfsOfsIoV3 *, const IfsOfsMountV3 *);
    int (*unmount)(void);
    int (*format)(const IfsOfsIoV3 *, const IfsOfsFormatV3 *);
    int (*stat)(const char *, IfsOfsEntryV3 *);
    int (*directory_entry)(const char *, uint32_t, IfsOfsEntryV3 *);
    int (*create)(const char *);
    int (*mkdir)(const char *);
    int (*read)(const char *, uint32_t, void *, uint32_t);
    int (*write)(const char *, uint32_t, const void *, uint32_t);
    int (*truncate)(const char *, uint32_t);
    int (*rename)(const char *, const char *);
    int (*unlink)(const char *);
    int (*set_protection)(const char *, uint32_t);
    int (*set_owner)(const char *, uint16_t, uint16_t);
    int (*set_comment)(const char *, const char *);
    int (*set_dates)(const char *, const IfsOfsDate *, uint32_t);
    int (*set_label)(const char *);
    int (*space)(IfsOfsSpaceV3 *);
    /* Explicit native file or user-directory hard link. Directory aliases
     * may form cycles. Removing an original promotes a rooted alias; an
     * operation which would orphan its only directory tree is refused. */
    int (*hardlink)(const char *, const char *);
    int (*symlink)(const char *, const char *); /* Native Amiga target syntax. */
    int (*readlink)(const char *, char *, uint32_t);
    int (*verify)(IfsOfsCheckV3 *);
    int (*recover_bitmap)(IfsOfsCheckV3 *);
    int (*lstat)(const char *, IfsOfsEntryV3 *);
    int (*flush)(void);
} IfsOfsModuleV3;
const IfsOfsModuleV3 *infiltrator_filesystem_module_v3(void);
#endif
