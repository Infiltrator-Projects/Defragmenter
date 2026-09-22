// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LINUX_DEFRAGGER_EXT_DISK_H
#define LINUX_DEFRAGGER_EXT_DISK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * First-party EXT2/3/4 on-disk access.
 *
 * This interface is deliberately narrower than libext2fs.  Defragmenter
 * preserves an inode's logical allocation shape and relocates existing payload
 * blocks only; it therefore needs exact decoding, physical-reference rewriting,
 * allocation bitmap updates and checksum maintenance, not a general filesystem
 * construction API.
 */

#define EXT_SUPER_MAGIC UINT16_C(0xef53)
#define EXT_ROOT_INO UINT32_C(2)
#define EXT_JOURNAL_INO UINT32_C(8)
#define EXT_GOOD_OLD_FIRST_INO UINT32_C(11)

#define EXT_VALID_FS UINT16_C(0x0001)

#define EXT_FEATURE_COMPAT_HAS_JOURNAL UINT32_C(0x0004)
#define EXT_FEATURE_COMPAT_RESIZE_INODE UINT32_C(0x0010)
#define EXT_FEATURE_COMPAT_SPARSE_SUPER2 UINT32_C(0x0200)
#define EXT_FEATURE_COMPAT_FAST_COMMIT UINT32_C(0x0400)
#define EXT_FEATURE_COMPAT_ORPHAN_FILE UINT32_C(0x1000)

#define EXT_FEATURE_RO_COMPAT_SPARSE_SUPER UINT32_C(0x0001)
#define EXT_FEATURE_RO_COMPAT_LARGE_FILE UINT32_C(0x0002)
#define EXT_FEATURE_RO_COMPAT_GDT_CSUM UINT32_C(0x0010)
#define EXT_FEATURE_RO_COMPAT_HAS_SNAPSHOT UINT32_C(0x0080)
#define EXT_FEATURE_RO_COMPAT_QUOTA UINT32_C(0x0100)
#define EXT_FEATURE_RO_COMPAT_BIGALLOC UINT32_C(0x0200)
#define EXT_FEATURE_RO_COMPAT_METADATA_CSUM UINT32_C(0x0400)
#define EXT_FEATURE_RO_COMPAT_READONLY UINT32_C(0x1000)
#define EXT_FEATURE_RO_COMPAT_SHARED_BLOCKS UINT32_C(0x4000)
#define EXT_FEATURE_RO_COMPAT_VERITY UINT32_C(0x8000)
#define EXT_FEATURE_RO_COMPAT_ORPHAN_PRESENT UINT32_C(0x10000)

#define EXT_FEATURE_INCOMPAT_COMPRESSION UINT32_C(0x0001)
#define EXT_FEATURE_INCOMPAT_FILETYPE UINT32_C(0x0002)
#define EXT_FEATURE_INCOMPAT_RECOVER UINT32_C(0x0004)
#define EXT_FEATURE_INCOMPAT_JOURNAL_DEV UINT32_C(0x0008)
#define EXT_FEATURE_INCOMPAT_META_BG UINT32_C(0x0010)
#define EXT_FEATURE_INCOMPAT_EXTENTS UINT32_C(0x0040)
#define EXT_FEATURE_INCOMPAT_64BIT UINT32_C(0x0080)
#define EXT_FEATURE_INCOMPAT_MMP UINT32_C(0x0100)
#define EXT_FEATURE_INCOMPAT_FLEX_BG UINT32_C(0x0200)
#define EXT_FEATURE_INCOMPAT_EA_INODE UINT32_C(0x0400)
#define EXT_FEATURE_INCOMPAT_DIRDATA UINT32_C(0x1000)
#define EXT_FEATURE_INCOMPAT_CSUM_SEED UINT32_C(0x2000)
#define EXT_FEATURE_INCOMPAT_LARGEDIR UINT32_C(0x4000)
#define EXT_FEATURE_INCOMPAT_INLINE_DATA UINT32_C(0x8000)
#define EXT_FEATURE_INCOMPAT_ENCRYPT UINT32_C(0x10000)
#define EXT_FEATURE_INCOMPAT_CASEFOLD UINT32_C(0x20000)

#define EXT_BG_INODE_UNINIT UINT16_C(0x0001)
#define EXT_BG_BLOCK_UNINIT UINT16_C(0x0002)

#define EXT_INODE_EXTENTS_FL UINT32_C(0x00080000)
#define EXT_INODE_INLINE_DATA_FL UINT32_C(0x10000000)
#define EXT_INODE_ENCRYPT_FL UINT32_C(0x00000800)
#define EXT_INODE_VERITY_FL UINT32_C(0x00100000)

typedef struct ExtFs ExtFs;

typedef struct {
    uint32_t number;
    uint16_t mode;
    uint16_t links;
    uint32_t flags;
    uint32_t generation;
    uint64_t size;
    uint8_t *raw;
    size_t raw_size;
} ExtInode;

typedef int (*ExtInodeVisitor)(ExtFs *fs, ExtInode *inode,
                               void *context, char **error);

/*
 * The visitor may replace *physical when mutable_mapping is true.  Logical
 * block numbers are file-relative payload blocks and arrive in ascending order.
 */
typedef int (*ExtPayloadVisitor)(ExtFs *fs, uint32_t ino, int64_t logical,
                                 uint64_t *physical, bool mutable_mapping,
                                 void *context, char **error);
typedef int (*ExtPhysicalVisitor)(ExtFs *fs, uint64_t physical,
                                  void *context, char **error);

int ext_fs_open(const char *path, bool writable, ExtFs **out, char **error);
void ext_fs_close(ExtFs *fs);
int ext_fs_flush(ExtFs *fs, char **error);

uint32_t ext_fs_block_size(const ExtFs *fs);
uint64_t ext_fs_blocks_count(const ExtFs *fs);
uint64_t ext_fs_free_blocks(const ExtFs *fs);
uint64_t ext_fs_first_data_block(const ExtFs *fs);
uint32_t ext_fs_first_inode(const ExtFs *fs);
uint32_t ext_fs_inodes_count(const ExtFs *fs);
uint32_t ext_fs_inodes_per_group(const ExtFs *fs);
uint32_t ext_fs_blocks_per_group(const ExtFs *fs);
uint32_t ext_fs_group_count(const ExtFs *fs);
uint32_t ext_fs_desc_size(const ExtFs *fs);
uint16_t ext_fs_state(const ExtFs *fs);
uint32_t ext_fs_compat(const ExtFs *fs);
uint32_t ext_fs_incompat(const ExtFs *fs);
uint32_t ext_fs_ro_compat(const ExtFs *fs);
const uint8_t *ext_fs_uuid(const ExtFs *fs);

int ext_fs_validate_metadata(ExtFs *fs, bool verify_inodes, char **error);
int ext_fs_foreach_inode(ExtFs *fs, ExtInodeVisitor visitor,
                         void *context, char **error);
int ext_fs_read_inode(ExtFs *fs, uint32_t ino, ExtInode *inode, char **error);
void ext_inode_destroy(ExtInode *inode);
int ext_fs_write_inode(ExtFs *fs, ExtInode *inode, char **error);

int ext_fs_iterate_payload(ExtFs *fs, ExtInode *inode, bool writable,
                           ExtPayloadVisitor visitor, void *context,
                           char **error);

int ext_fs_block_allocated(ExtFs *fs, uint64_t block, bool *allocated,
                           char **error);
int ext_fs_set_block_allocated(ExtFs *fs, uint64_t block, bool allocated,
                               char **error);

/* Exact fixed/system metadata classification used by the native map worker. */
int ext_fs_foreach_metadata_block(ExtFs *fs, ExtPhysicalVisitor visitor,
                                  void *context, char **error);

#endif
