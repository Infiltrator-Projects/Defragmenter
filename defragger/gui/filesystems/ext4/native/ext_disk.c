// SPDX-License-Identifier: GPL-3.0-or-later
#include "ext_disk.h"

#include "ld_io.h"
#include "ld_runtime.h"

#include "infiltratr/endian.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXT_SUPER_OFFSET UINT64_C(1024)
#define EXT_SUPER_SIZE 1024U
#define EXT_SUPER_CSUM_OFFSET 0x3fcU
#define EXT_EXTENT_MAGIC UINT16_C(0xf30a)
#define EXT_EXTENT_HEADER_SIZE 12U
#define EXT_EXTENT_ENTRY_SIZE 12U
#define EXT_MAX_EXTENT_DEPTH 5U
#define EXT_NDIR_BLOCKS 12U
#define EXT_N_BLOCKS 15U
#define EXT_INODE_BLOCK_OFFSET 40U
#define EXT_INODE_BLOCK_BYTES 60U
#define EXT_INODE_CSUM_LO_OFFSET 124U
#define EXT_INODE_EXTRA_ISIZE_OFFSET 128U
#define EXT_INODE_CSUM_HI_OFFSET 130U
#define EXT_CRC32C_TYPE 1U

struct ExtFs {
    int fd;
    bool writable;
    uint8_t super[EXT_SUPER_SIZE];
    uint32_t block_size;
    uint64_t blocks_count;
    uint64_t free_blocks;
    uint64_t original_free_blocks;
    uint64_t first_data_block;
    uint32_t inodes_count;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t first_inode;
    uint32_t group_count;
    uint32_t desc_size;
    uint32_t desc_per_block;
    uint32_t desc_blocks;
    uint32_t first_meta_bg;
    uint16_t reserved_gdt_blocks;
    uint16_t state;
    uint32_t compat;
    uint32_t incompat;
    uint32_t ro_compat;
    uint32_t backup_bgs[2];
    uint64_t mmp_block;
    uint8_t uuid[16];
    uint8_t checksum_type;
    uint32_t csum_seed;
    uint8_t *group_descs;

    uint8_t *block_bitmap;
    uint32_t block_bitmap_group;
    bool block_bitmap_valid;
    bool block_bitmap_dirty;
    bool block_bitmap_synthetic;

    uint8_t *inode_bitmap;
    uint32_t inode_bitmap_group;
    bool inode_bitmap_valid;
};

static void set_error(char **error, const char *message)
{
    if (error != NULL && *error == NULL)
        *error = ld_xstrdup(message);
}

static void set_error_errno(char **error, const char *prefix)
{
    if (error == NULL || *error != NULL)
        return;
    size_t needed = strlen(prefix) + strlen(strerror(errno)) + 3U;
    char *text = ld_xmalloc(needed);
    (void)snprintf(text, needed, "%s: %s", prefix, strerror(errno));
    *error = text;
}

static uint16_t le16(const uint8_t *p)
{
    return infiltratr_load_le16(p);
}

static uint32_t le32(const uint8_t *p)
{
    return infiltratr_load_le32(p);
}

static uint64_t le64(const uint8_t *p)
{
    return infiltratr_load_le64(p);
}

static void put_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8U);
}

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8U);
    p[2] = (uint8_t)(value >> 16U);
    p[3] = (uint8_t)(value >> 24U);
}

static uint32_t crc32c(uint32_t crc, const uint8_t *data, size_t length)
{
    while (length-- != 0U) {
        crc ^= *data++;
        for (unsigned bit = 0U; bit < 8U; ++bit)
            crc = (crc >> 1U) ^
                  (UINT32_C(0x82f63b78) & (uint32_t)-(int32_t)(crc & 1U));
    }
    return crc;
}

static uint16_t crc16(uint16_t crc, const uint8_t *data, size_t length)
{
    while (length-- != 0U) {
        crc ^= *data++;
        for (unsigned bit = 0U; bit < 8U; ++bit)
            crc = (uint16_t)((crc >> 1U) ^
                  (UINT16_C(0xa001) & (uint16_t)-(int16_t)(crc & 1U)));
    }
    return crc;
}

static uint64_t group_first_block(const ExtFs *fs, uint32_t group)
{
    return fs->first_data_block +
           (uint64_t)group * (uint64_t)fs->blocks_per_group;
}

static uint64_t group_last_block_exclusive(const ExtFs *fs, uint32_t group)
{
    uint64_t end = group_first_block(fs, group) + fs->blocks_per_group;
    return end < fs->blocks_count ? end : fs->blocks_count;
}

static bool power_of(unsigned value, unsigned base)
{
    if (value < 1U)
        return false;
    while ((value % base) == 0U)
        value /= base;
    return value == 1U;
}

static bool group_has_super(const ExtFs *fs, uint32_t group)
{
    if (group == 0U)
        return true;
    if ((fs->compat & EXT_FEATURE_COMPAT_SPARSE_SUPER2) != 0U)
        return group == fs->backup_bgs[0] || group == fs->backup_bgs[1];
    if (group <= 1U ||
        (fs->ro_compat & EXT_FEATURE_RO_COMPAT_SPARSE_SUPER) == 0U)
        return true;
    if ((group & 1U) == 0U)
        return false;
    return power_of(group, 3U) || power_of(group, 5U) ||
           power_of(group, 7U);
}

static uint64_t descriptor_block_location(const ExtFs *fs,
                                          uint32_t descriptor_block)
{
    if ((fs->incompat & EXT_FEATURE_INCOMPAT_META_BG) == 0U ||
        descriptor_block < fs->first_meta_bg)
        return fs->first_data_block + (uint64_t)descriptor_block + 1U;

    uint64_t bg64 = (uint64_t)fs->desc_per_block * descriptor_block;
    if (bg64 >= fs->group_count)
        return UINT64_MAX;
    uint32_t bg = (uint32_t)bg64;
    return group_first_block(fs, bg) + (group_has_super(fs, bg) ? 1U : 0U);
}

static uint8_t *group_desc(ExtFs *fs, uint32_t group)
{
    return fs->group_descs + (size_t)group * fs->desc_size;
}

static const uint8_t *group_desc_const(const ExtFs *fs, uint32_t group)
{
    return fs->group_descs + (size_t)group * fs->desc_size;
}

static uint64_t desc_u64(const ExtFs *fs, const uint8_t *desc,
                         size_t low_offset, size_t high_offset)
{
    uint64_t value = le32(desc + low_offset);
    if ((fs->incompat & EXT_FEATURE_INCOMPAT_64BIT) != 0U &&
        fs->desc_size >= high_offset + 4U)
        value |= (uint64_t)le32(desc + high_offset) << 32U;
    return value;
}

static uint64_t block_bitmap_block(const ExtFs *fs, uint32_t group)
{
    return desc_u64(fs, group_desc_const(fs, group), 0U, 32U);
}

static uint64_t inode_bitmap_block(const ExtFs *fs, uint32_t group)
{
    return desc_u64(fs, group_desc_const(fs, group), 4U, 36U);
}

static uint64_t inode_table_block(const ExtFs *fs, uint32_t group)
{
    return desc_u64(fs, group_desc_const(fs, group), 8U, 40U);
}

static uint64_t desc_free_blocks(const ExtFs *fs, uint32_t group)
{
    const uint8_t *desc = group_desc_const(fs, group);
    uint64_t value = le16(desc + 12U);
    if ((fs->incompat & EXT_FEATURE_INCOMPAT_64BIT) != 0U &&
        fs->desc_size >= 46U)
        value |= (uint64_t)le16(desc + 44U) << 16U;
    return value;
}

static void set_desc_free_blocks(ExtFs *fs, uint32_t group, uint64_t value)
{
    uint8_t *desc = group_desc(fs, group);
    put_le16(desc + 12U, (uint16_t)value);
    if ((fs->incompat & EXT_FEATURE_INCOMPAT_64BIT) != 0U &&
        fs->desc_size >= 46U)
        put_le16(desc + 44U, (uint16_t)(value >> 16U));
}

static uint16_t desc_flags(const ExtFs *fs, uint32_t group)
{
    return le16(group_desc_const(fs, group) + 18U);
}

static void set_desc_flags(ExtFs *fs, uint32_t group, uint16_t flags)
{
    put_le16(group_desc(fs, group) + 18U, flags);
}

static bool valid_fs_block(const ExtFs *fs, uint64_t block)
{
    return block >= fs->first_data_block && block < fs->blocks_count;
}

static int read_block(const ExtFs *fs, uint64_t block, void *buffer,
                      char **error)
{
    if (!valid_fs_block(fs, block)) {
        set_error(error, "EXT metadata points outside the filesystem");
        return -1;
    }
    ssize_t got = ld_pread_full(fs->fd, buffer, fs->block_size,
                                block * (uint64_t)fs->block_size);
    if (got < 0 || (size_t)got != fs->block_size) {
        set_error_errno(error, "cannot read EXT filesystem block");
        return -1;
    }
    return 0;
}

static int write_block(ExtFs *fs, uint64_t block, const void *buffer,
                       char **error)
{
    if (!fs->writable || !valid_fs_block(fs, block)) {
        set_error(error, "invalid EXT metadata block write");
        return -1;
    }
    ssize_t wrote = ld_pwrite_full(fs->fd, buffer, fs->block_size,
                                   block * (uint64_t)fs->block_size);
    if (wrote < 0 || (size_t)wrote != fs->block_size) {
        set_error_errno(error, "cannot write EXT filesystem block");
        return -1;
    }
    return 0;
}

static int verify_super_checksum(const ExtFs *fs, char **error)
{
    if ((fs->ro_compat & EXT_FEATURE_RO_COMPAT_METADATA_CSUM) == 0U)
        return 0;
    if (fs->checksum_type != EXT_CRC32C_TYPE) {
        set_error(error, "unsupported EXT metadata checksum type");
        return -1;
    }
    uint32_t expected = le32(fs->super + EXT_SUPER_CSUM_OFFSET);
    uint32_t actual = crc32c(UINT32_MAX, fs->super, EXT_SUPER_CSUM_OFFSET);
    if (expected != actual) {
        set_error(error, "EXT superblock checksum verification failed");
        return -1;
    }
    return 0;
}

static uint16_t group_checksum(const ExtFs *fs, uint32_t group)
{
    const uint8_t *desc = group_desc_const(fs, group);
    uint8_t group_le[4];
    put_le32(group_le, group);

    if ((fs->ro_compat & EXT_FEATURE_RO_COMPAT_METADATA_CSUM) != 0U) {
        uint8_t *copy = ld_xmalloc(fs->desc_size);
        memcpy(copy, desc, fs->desc_size);
        copy[30] = 0U;
        copy[31] = 0U;
        uint32_t crc = crc32c(fs->csum_seed, group_le, sizeof(group_le));
        crc = crc32c(crc, copy, fs->desc_size);
        free(copy);
        return (uint16_t)crc;
    }

    uint16_t crc = UINT16_MAX;
    crc = crc16(crc, fs->uuid, sizeof(fs->uuid));
    crc = crc16(crc, group_le, sizeof(group_le));
    crc = crc16(crc, desc, 30U);
    if (fs->desc_size > 32U)
        crc = crc16(crc, desc + 32U, fs->desc_size - 32U);
    return crc;
}

static bool group_checksummed(const ExtFs *fs)
{
    return (fs->ro_compat &
            (EXT_FEATURE_RO_COMPAT_GDT_CSUM |
             EXT_FEATURE_RO_COMPAT_METADATA_CSUM)) != 0U;
}

static int verify_group_checksum(const ExtFs *fs, uint32_t group,
                                 char **error)
{
    if (!group_checksummed(fs))
        return 0;
    if (le16(group_desc_const(fs, group) + 30U) !=
        group_checksum(fs, group)) {
        set_error(error, "EXT group descriptor checksum verification failed");
        return -1;
    }
    return 0;
}

static void set_group_checksum(ExtFs *fs, uint32_t group)
{
    if (group_checksummed(fs))
        put_le16(group_desc(fs, group) + 30U, group_checksum(fs, group));
}

static uint32_t bitmap_checksum(const ExtFs *fs, const uint8_t *bitmap,
                                size_t bytes)
{
    return crc32c(fs->csum_seed, bitmap, bytes);
}

static int verify_block_bitmap_checksum(const ExtFs *fs, uint32_t group,
                                        const uint8_t *bitmap, char **error)
{
    if ((fs->ro_compat & EXT_FEATURE_RO_COMPAT_METADATA_CSUM) == 0U)
        return 0;
    uint32_t expected = le16(group_desc_const(fs, group) + 24U);
    if (fs->desc_size >= 58U)
        expected |= (uint32_t)le16(group_desc_const(fs, group) + 56U) << 16U;
    uint32_t actual = bitmap_checksum(
        fs, bitmap, ((size_t)fs->blocks_per_group + 7U) / 8U);
    if (fs->desc_size < 58U)
        actual &= UINT32_C(0xffff);
    if (actual != expected) {
        set_error(error, "EXT block bitmap checksum verification failed");
        return -1;
    }
    return 0;
}

static int verify_inode_bitmap_checksum(const ExtFs *fs, uint32_t group,
                                        const uint8_t *bitmap, char **error)
{
    if ((fs->ro_compat & EXT_FEATURE_RO_COMPAT_METADATA_CSUM) == 0U)
        return 0;
    uint32_t expected = le16(group_desc_const(fs, group) + 26U);
    if (fs->desc_size >= 60U)
        expected |= (uint32_t)le16(group_desc_const(fs, group) + 58U) << 16U;
    uint32_t actual = bitmap_checksum(
        fs, bitmap, ((size_t)fs->inodes_per_group + 7U) / 8U);
    if (fs->desc_size < 60U)
        actual &= UINT32_C(0xffff);
    if (actual != expected) {
        set_error(error, "EXT inode bitmap checksum verification failed");
        return -1;
    }
    return 0;
}

static void set_block_bitmap_checksum(ExtFs *fs, uint32_t group,
                                      const uint8_t *bitmap)
{
    if ((fs->ro_compat & EXT_FEATURE_RO_COMPAT_METADATA_CSUM) == 0U)
        return;
    uint32_t crc = bitmap_checksum(
        fs, bitmap, ((size_t)fs->blocks_per_group + 7U) / 8U);
    uint8_t *desc = group_desc(fs, group);
    put_le16(desc + 24U, (uint16_t)crc);
    if (fs->desc_size >= 58U)
        put_le16(desc + 56U, (uint16_t)(crc >> 16U));
}

static bool bit_test(const uint8_t *bitmap, uint64_t bit)
{
    return (bitmap[bit >> 3U] & (uint8_t)(1U << (bit & 7U))) != 0U;
}

static void bit_set(uint8_t *bitmap, uint64_t bit, bool value)
{
    uint8_t mask = (uint8_t)(1U << (bit & 7U));
    if (value)
        bitmap[bit >> 3U] |= mask;
    else
        bitmap[bit >> 3U] &= (uint8_t)~mask;
}

static void mark_if_local(const ExtFs *fs, uint32_t group, uint8_t *bitmap,
                          uint64_t block)
{
    uint64_t first = group_first_block(fs, group);
    uint64_t end = group_last_block_exclusive(fs, group);
    if (block >= first && block < end)
        bit_set(bitmap, block - first, true);
}

static void mark_range_if_local(const ExtFs *fs, uint32_t group,
                                uint8_t *bitmap, uint64_t start,
                                uint64_t count)
{
    for (uint64_t index = 0U; index < count; ++index)
        mark_if_local(fs, group, bitmap, start + index);
}

static void synthesize_uninit_block_bitmap(const ExtFs *fs, uint32_t group,
                                           uint8_t *bitmap)
{
    memset(bitmap, 0, fs->block_size);

    uint64_t first = group_first_block(fs, group);
    if (group_has_super(fs, group)) {
        mark_if_local(fs, group, bitmap, first);
        if ((fs->incompat & EXT_FEATURE_INCOMPAT_META_BG) == 0U) {
            mark_range_if_local(fs, group, bitmap, first + 1U,
                                (uint64_t)fs->desc_blocks +
                                fs->reserved_gdt_blocks);
        }
    }

    if ((fs->incompat & EXT_FEATURE_INCOMPAT_META_BG) != 0U) {
        uint32_t meta_size = fs->desc_per_block;
        uint32_t position = group % meta_size;
        if (position == 0U || position == 1U || position + 1U == meta_size) {
            uint32_t descriptor_block = group / meta_size;
            if (descriptor_block >= fs->first_meta_bg)
                mark_if_local(fs, group, bitmap,
                              first + (group_has_super(fs, group) ? 1U : 0U));
        }
    }

    uint64_t inode_table_blocks =
        ((uint64_t)fs->inodes_per_group * fs->inode_size +
         fs->block_size - 1U) / fs->block_size;
    for (uint32_t other = 0U; other < fs->group_count; ++other) {
        mark_if_local(fs, group, bitmap, block_bitmap_block(fs, other));
        mark_if_local(fs, group, bitmap, inode_bitmap_block(fs, other));
        mark_range_if_local(fs, group, bitmap, inode_table_block(fs, other),
                            inode_table_blocks);
    }

    if (fs->mmp_block != 0U)
        mark_if_local(fs, group, bitmap, fs->mmp_block);
}

static int flush_block_bitmap_cache(ExtFs *fs, char **error)
{
    if (!fs->block_bitmap_valid || !fs->block_bitmap_dirty)
        return 0;
    uint32_t group = fs->block_bitmap_group;
    uint16_t flags = desc_flags(fs, group);
    flags &= (uint16_t)~EXT_BG_BLOCK_UNINIT;
    set_desc_flags(fs, group, flags);
    set_block_bitmap_checksum(fs, group, fs->block_bitmap);
    set_group_checksum(fs, group);
    if (write_block(fs, block_bitmap_block(fs, group),
                    fs->block_bitmap, error) != 0)
        return -1;
    fs->block_bitmap_dirty = false;
    fs->block_bitmap_synthetic = false;
    return 0;
}

static int load_block_bitmap(ExtFs *fs, uint32_t group, char **error)
{
    if (fs->block_bitmap_valid && fs->block_bitmap_group == group)
        return 0;
    if (flush_block_bitmap_cache(fs, error) != 0)
        return -1;

    fs->block_bitmap_group = group;
    fs->block_bitmap_valid = true;
    fs->block_bitmap_dirty = false;
    fs->block_bitmap_synthetic =
        (desc_flags(fs, group) & EXT_BG_BLOCK_UNINIT) != 0U;

    if (fs->block_bitmap_synthetic) {
        synthesize_uninit_block_bitmap(fs, group, fs->block_bitmap);
        return 0;
    }
    if (read_block(fs, block_bitmap_block(fs, group),
                   fs->block_bitmap, error) != 0)
        return -1;
    return verify_block_bitmap_checksum(fs, group, fs->block_bitmap, error);
}

static int load_inode_bitmap(ExtFs *fs, uint32_t group, char **error)
{
    if (fs->inode_bitmap_valid && fs->inode_bitmap_group == group)
        return 0;
    fs->inode_bitmap_group = group;
    fs->inode_bitmap_valid = true;
    if ((desc_flags(fs, group) & EXT_BG_INODE_UNINIT) != 0U) {
        memset(fs->inode_bitmap, 0, fs->block_size);
        return 0;
    }
    if (read_block(fs, inode_bitmap_block(fs, group),
                   fs->inode_bitmap, error) != 0)
        return -1;
    return verify_inode_bitmap_checksum(fs, group, fs->inode_bitmap, error);
}

static int read_group_descriptors(ExtFs *fs, char **error)
{
    size_t total = (size_t)fs->group_count * fs->desc_size;
    fs->group_descs = ld_xmalloc(total);
    uint8_t *block = ld_xmalloc(fs->block_size);
    uint32_t loaded_descriptor_block = UINT32_MAX;

    for (uint32_t group = 0U; group < fs->group_count; ++group) {
        uint32_t descriptor_block = group / fs->desc_per_block;
        if (descriptor_block != loaded_descriptor_block) {
            uint64_t location =
                descriptor_block_location(fs, descriptor_block);
            if (location == UINT64_MAX ||
                read_block(fs, location, block, error) != 0) {
                free(block);
                return -1;
            }
            loaded_descriptor_block = descriptor_block;
        }
        size_t offset =
            (size_t)(group % fs->desc_per_block) * fs->desc_size;
        if (offset + fs->desc_size > fs->block_size) {
            free(block);
            set_error(error, "invalid EXT group descriptor geometry");
            return -1;
        }
        memcpy(group_desc(fs, group), block + offset, fs->desc_size);
    }
    free(block);
    return 0;
}

static int write_primary_descriptors(ExtFs *fs, char **error)
{
    uint8_t *block = ld_xmalloc(fs->block_size);
    for (uint32_t descriptor_block = 0U;
         descriptor_block < fs->desc_blocks; ++descriptor_block) {
        uint64_t location =
            descriptor_block_location(fs, descriptor_block);
        if (location == UINT64_MAX ||
            read_block(fs, location, block, error) != 0) {
            free(block);
            return -1;
        }
        uint32_t first_group = descriptor_block * fs->desc_per_block;
        for (uint32_t slot = 0U; slot < fs->desc_per_block; ++slot) {
            uint32_t group = first_group + slot;
            if (group >= fs->group_count)
                break;
            memcpy(block + (size_t)slot * fs->desc_size,
                   group_desc(fs, group), fs->desc_size);
        }
        if (write_block(fs, location, block, error) != 0) {
            free(block);
            return -1;
        }
    }
    free(block);
    return 0;
}

static int write_old_layout_descriptor_backups(ExtFs *fs, char **error)
{
    if ((fs->incompat & EXT_FEATURE_INCOMPAT_META_BG) != 0U)
        return 0;

    uint8_t *source = ld_xmalloc(fs->block_size);
    for (uint32_t group = 1U; group < fs->group_count; ++group) {
        if (!group_has_super(fs, group))
            continue;
        uint64_t first = group_first_block(fs, group);
        for (uint32_t descriptor_block = 0U;
             descriptor_block < fs->desc_blocks; ++descriptor_block) {
            uint64_t primary =
                descriptor_block_location(fs, descriptor_block);
            if (read_block(fs, primary, source, error) != 0 ||
                write_block(fs, first + 1U + descriptor_block,
                            source, error) != 0) {
                free(source);
                return -1;
            }
        }
    }
    free(source);
    return 0;
}

static uint64_t inode_offset(const ExtFs *fs, uint32_t ino)
{
    uint32_t group = (ino - 1U) / fs->inodes_per_group;
    uint32_t index = (ino - 1U) % fs->inodes_per_group;
    return inode_table_block(fs, group) * (uint64_t)fs->block_size +
           (uint64_t)index * fs->inode_size;
}

static bool inode_has_checksum_hi(const ExtFs *fs, const uint8_t *raw)
{
    return fs->inode_size > 128U &&
           fs->inode_size > EXT_INODE_CSUM_HI_OFFSET + 1U &&
           le16(raw + EXT_INODE_EXTRA_ISIZE_OFFSET) >= 4U;
}

static uint32_t inode_checksum(ExtFs *fs, uint32_t ino, uint8_t *raw)
{
    uint8_t ino_le[4];
    put_le32(ino_le, ino);
    uint16_t old_lo = le16(raw + EXT_INODE_CSUM_LO_OFFSET);
    uint16_t old_hi = 0U;
    bool has_hi = inode_has_checksum_hi(fs, raw);
    put_le16(raw + EXT_INODE_CSUM_LO_OFFSET, 0U);
    if (has_hi) {
        old_hi = le16(raw + EXT_INODE_CSUM_HI_OFFSET);
        put_le16(raw + EXT_INODE_CSUM_HI_OFFSET, 0U);
    }

    uint32_t crc = crc32c(fs->csum_seed, ino_le, sizeof(ino_le));
    crc = crc32c(crc, raw + 100U, 4U);
    crc = crc32c(crc, raw, fs->inode_size);

    put_le16(raw + EXT_INODE_CSUM_LO_OFFSET, old_lo);
    if (has_hi)
        put_le16(raw + EXT_INODE_CSUM_HI_OFFSET, old_hi);
    return crc;
}

static int verify_inode_checksum(ExtFs *fs, uint32_t ino, uint8_t *raw,
                                 char **error)
{
    if ((fs->ro_compat & EXT_FEATURE_RO_COMPAT_METADATA_CSUM) == 0U)
        return 0;
    uint32_t expected = le16(raw + EXT_INODE_CSUM_LO_OFFSET);
    uint32_t actual = inode_checksum(fs, ino, raw);
    if (inode_has_checksum_hi(fs, raw))
        expected |= (uint32_t)le16(raw + EXT_INODE_CSUM_HI_OFFSET) << 16U;
    else
        actual &= UINT32_C(0xffff);
    if (expected == actual)
        return 0;

    bool all_zero = true;
    size_t check = fs->inode_size < 128U ? fs->inode_size : 128U;
    for (size_t index = 0U; index < check; ++index)
        if (raw[index] != 0U) {
            all_zero = false;
            break;
        }
    if (all_zero)
        return 0;
    set_error(error, "EXT inode checksum verification failed");
    return -1;
}

static void set_inode_checksum(ExtFs *fs, ExtInode *inode)
{
    if ((fs->ro_compat & EXT_FEATURE_RO_COMPAT_METADATA_CSUM) == 0U)
        return;
    uint32_t crc = inode_checksum(fs, inode->number, inode->raw);
    put_le16(inode->raw + EXT_INODE_CSUM_LO_OFFSET, (uint16_t)crc);
    if (inode_has_checksum_hi(fs, inode->raw))
        put_le16(inode->raw + EXT_INODE_CSUM_HI_OFFSET,
                 (uint16_t)(crc >> 16U));
}

int ext_fs_open(const char *path, bool writable, ExtFs **out, char **error)
{
    if (out == NULL) {
        set_error(error, "invalid EXT open request");
        return -1;
    }
    *out = NULL;
    int fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) {
        set_error_errno(error, "cannot open EXT target");
        return -1;
    }

    ExtFs *fs = ld_xcalloc(1U, sizeof(*fs));
    fs->fd = fd;
    fs->writable = writable;
    ssize_t got = ld_pread_full(fd, fs->super, sizeof(fs->super),
                                EXT_SUPER_OFFSET);
    if (got < 0 || (size_t)got != sizeof(fs->super)) {
        set_error_errno(error, "cannot read EXT superblock");
        ext_fs_close(fs);
        return -1;
    }
    if (le16(fs->super + 56U) != EXT_SUPER_MAGIC) {
        set_error(error, "not an EXT2/EXT3/EXT4 filesystem");
        ext_fs_close(fs);
        return -1;
    }

    uint32_t log_block = le32(fs->super + 24U);
    if (log_block > 6U) {
        set_error(error, "unsupported EXT block size");
        ext_fs_close(fs);
        return -1;
    }
    fs->block_size = 1024U << log_block;
    fs->inodes_count = le32(fs->super + 0U);
    fs->blocks_count = le32(fs->super + 4U);
    fs->free_blocks = le32(fs->super + 12U);
    fs->first_data_block = le32(fs->super + 20U);
    fs->blocks_per_group = le32(fs->super + 32U);
    fs->inodes_per_group = le32(fs->super + 40U);
    fs->state = le16(fs->super + 58U);
    fs->first_inode = le32(fs->super + 84U);
    if (fs->first_inode == 0U)
        fs->first_inode = EXT_GOOD_OLD_FIRST_INO;
    fs->inode_size = le16(fs->super + 88U);
    if (fs->inode_size == 0U)
        fs->inode_size = 128U;
    fs->compat = le32(fs->super + 92U);
    fs->incompat = le32(fs->super + 96U);
    fs->ro_compat = le32(fs->super + 100U);
    memcpy(fs->uuid, fs->super + 104U, sizeof(fs->uuid));
    fs->reserved_gdt_blocks = le16(fs->super + 206U);
    fs->first_meta_bg = le32(fs->super + 260U);
    fs->mmp_block = le64(fs->super + 360U);
    fs->checksum_type = fs->super[373U];
    fs->backup_bgs[0] = le32(fs->super + 588U);
    fs->backup_bgs[1] = le32(fs->super + 592U);

    if ((fs->incompat & EXT_FEATURE_INCOMPAT_64BIT) != 0U) {
        fs->blocks_count |= (uint64_t)le32(fs->super + 336U) << 32U;
        fs->free_blocks |= (uint64_t)le32(fs->super + 344U) << 32U;
    }

    if (fs->blocks_per_group == 0U || fs->inodes_per_group == 0U ||
        fs->inode_size < 128U || fs->inode_size > fs->block_size ||
        fs->blocks_count <= fs->first_data_block ||
        fs->free_blocks > fs->blocks_count) {
        set_error(error, "invalid EXT superblock geometry");
        ext_fs_close(fs);
        return -1;
    }

    fs->desc_size =
        (fs->incompat & EXT_FEATURE_INCOMPAT_64BIT) != 0U
            ? le16(fs->super + 254U) : 32U;
    if (fs->desc_size < 32U ||
        ((fs->incompat & EXT_FEATURE_INCOMPAT_64BIT) != 0U &&
         fs->desc_size < 64U) ||
        fs->desc_size > fs->block_size ||
        (fs->block_size % fs->desc_size) != 0U) {
        set_error(error, "unsupported EXT group descriptor size");
        ext_fs_close(fs);
        return -1;
    }

    uint64_t data_blocks = fs->blocks_count - fs->first_data_block;
    uint64_t group_count =
        (data_blocks + fs->blocks_per_group - 1U) / fs->blocks_per_group;
    if (group_count == 0U || group_count > UINT32_MAX) {
        set_error(error, "EXT group count is outside supported limits");
        ext_fs_close(fs);
        return -1;
    }
    fs->group_count = (uint32_t)group_count;
    fs->desc_per_block = fs->block_size / fs->desc_size;
    fs->desc_blocks =
        (fs->group_count + fs->desc_per_block - 1U) / fs->desc_per_block;
    fs->original_free_blocks = fs->free_blocks;

    if ((fs->ro_compat & EXT_FEATURE_RO_COMPAT_METADATA_CSUM) != 0U) {
        if ((fs->incompat & EXT_FEATURE_INCOMPAT_CSUM_SEED) != 0U)
            fs->csum_seed = le32(fs->super + 624U);
        else
            fs->csum_seed = crc32c(UINT32_MAX, fs->uuid, sizeof(fs->uuid));
    }

    if (verify_super_checksum(fs, error) != 0 ||
        read_group_descriptors(fs, error) != 0) {
        ext_fs_close(fs);
        return -1;
    }

    fs->block_bitmap = ld_xmalloc(fs->block_size);
    fs->inode_bitmap = ld_xmalloc(fs->block_size);
    *out = fs;
    return 0;
}

void ext_fs_close(ExtFs *fs)
{
    if (fs == NULL)
        return;
    if (fs->fd >= 0)
        (void)close(fs->fd);
    free(fs->group_descs);
    free(fs->block_bitmap);
    free(fs->inode_bitmap);
    free(fs);
}

uint32_t ext_fs_block_size(const ExtFs *fs) { return fs->block_size; }
uint64_t ext_fs_blocks_count(const ExtFs *fs) { return fs->blocks_count; }
uint64_t ext_fs_free_blocks(const ExtFs *fs) { return fs->free_blocks; }
uint64_t ext_fs_first_data_block(const ExtFs *fs) { return fs->first_data_block; }
uint32_t ext_fs_first_inode(const ExtFs *fs) { return fs->first_inode; }
uint32_t ext_fs_inodes_count(const ExtFs *fs) { return fs->inodes_count; }
uint32_t ext_fs_inodes_per_group(const ExtFs *fs) { return fs->inodes_per_group; }
uint32_t ext_fs_blocks_per_group(const ExtFs *fs) { return fs->blocks_per_group; }
uint32_t ext_fs_group_count(const ExtFs *fs) { return fs->group_count; }
uint32_t ext_fs_desc_size(const ExtFs *fs) { return fs->desc_size; }
uint16_t ext_fs_state(const ExtFs *fs) { return fs->state; }
uint32_t ext_fs_compat(const ExtFs *fs) { return fs->compat; }
uint32_t ext_fs_incompat(const ExtFs *fs) { return fs->incompat; }
uint32_t ext_fs_ro_compat(const ExtFs *fs) { return fs->ro_compat; }
const uint8_t *ext_fs_uuid(const ExtFs *fs) { return fs->uuid; }

int ext_fs_block_allocated(ExtFs *fs, uint64_t block, bool *allocated,
                           char **error)
{
    if (allocated == NULL || !valid_fs_block(fs, block)) {
        set_error(error, "EXT allocation query is outside the filesystem");
        return -1;
    }
    uint64_t relative = block - fs->first_data_block;
    uint32_t group = (uint32_t)(relative / fs->blocks_per_group);
    uint64_t bit = relative % fs->blocks_per_group;
    if (group >= fs->group_count || load_block_bitmap(fs, group, error) != 0)
        return -1;
    *allocated = bit_test(fs->block_bitmap, bit);
    return 0;
}

int ext_fs_set_block_allocated(ExtFs *fs, uint64_t block, bool allocated,
                               char **error)
{
    if (!fs->writable) {
        set_error(error, "EXT allocation update requires a writable filesystem");
        return -1;
    }
    uint64_t relative = block - fs->first_data_block;
    if (!valid_fs_block(fs, block)) {
        set_error(error, "EXT allocation update is outside the filesystem");
        return -1;
    }
    uint32_t group = (uint32_t)(relative / fs->blocks_per_group);
    uint64_t bit = relative % fs->blocks_per_group;
    if (load_block_bitmap(fs, group, error) != 0)
        return -1;
    bool current = bit_test(fs->block_bitmap, bit);
    if (current == allocated)
        return 0;

    uint64_t group_free = desc_free_blocks(fs, group);
    if (allocated) {
        if (group_free == 0U || fs->free_blocks == 0U) {
            set_error(error, "EXT free-block accounting underflow");
            return -1;
        }
        group_free--;
        fs->free_blocks--;
    } else {
        group_free++;
        fs->free_blocks++;
    }
    set_desc_free_blocks(fs, group, group_free);
    bit_set(fs->block_bitmap, bit, allocated);
    fs->block_bitmap_dirty = true;
    return 0;
}

int ext_fs_read_inode(ExtFs *fs, uint32_t ino, ExtInode *inode, char **error)
{
    if (inode == NULL || ino == 0U || ino > fs->inodes_count) {
        set_error(error, "invalid EXT inode number");
        return -1;
    }
    memset(inode, 0, sizeof(*inode));
    inode->raw = ld_xmalloc(fs->inode_size);
    inode->raw_size = fs->inode_size;
    ssize_t got = ld_pread_full(fs->fd, inode->raw, fs->inode_size,
                                inode_offset(fs, ino));
    if (got < 0 || (size_t)got != fs->inode_size) {
        ext_inode_destroy(inode);
        set_error_errno(error, "cannot read EXT inode");
        return -1;
    }
    if (verify_inode_checksum(fs, ino, inode->raw, error) != 0) {
        ext_inode_destroy(inode);
        return -1;
    }
    inode->number = ino;
    inode->mode = le16(inode->raw + 0U);
    inode->links = le16(inode->raw + 26U);
    inode->flags = le32(inode->raw + 32U);
    inode->generation = le32(inode->raw + 100U);
    inode->size = le32(inode->raw + 4U);
    if ((inode->mode & UINT16_C(0170000)) == UINT16_C(0100000))
        inode->size |= (uint64_t)le32(inode->raw + 108U) << 32U;
    return 0;
}

void ext_inode_destroy(ExtInode *inode)
{
    if (inode == NULL)
        return;
    free(inode->raw);
    memset(inode, 0, sizeof(*inode));
}

int ext_fs_write_inode(ExtFs *fs, ExtInode *inode, char **error)
{
    if (!fs->writable || inode == NULL || inode->raw == NULL ||
        inode->raw_size != fs->inode_size) {
        set_error(error, "invalid EXT inode write request");
        return -1;
    }
    set_inode_checksum(fs, inode);
    ssize_t wrote = ld_pwrite_full(fs->fd, inode->raw, fs->inode_size,
                                   inode_offset(fs, inode->number));
    if (wrote < 0 || (size_t)wrote != fs->inode_size) {
        set_error_errno(error, "cannot write EXT inode");
        return -1;
    }
    return 0;
}

int ext_fs_foreach_inode(ExtFs *fs, ExtInodeVisitor visitor,
                         void *context, char **error)
{
    for (uint32_t group = 0U; group < fs->group_count; ++group) {
        if (load_inode_bitmap(fs, group, error) != 0)
            return -1;
        uint32_t first = group * fs->inodes_per_group + 1U;
        uint32_t count = fs->inodes_per_group;
        if ((uint64_t)first + count - 1U > fs->inodes_count)
            count = fs->inodes_count - first + 1U;
        for (uint32_t index = 0U; index < count; ++index) {
            if (!bit_test(fs->inode_bitmap, index))
                continue;
            ExtInode inode;
            if (ext_fs_read_inode(fs, first + index, &inode, error) != 0)
                return -1;
            int result = visitor(fs, &inode, context, error);
            ext_inode_destroy(&inode);
            if (result != 0)
                return result;
        }
    }
    return 0;
}

static int extent_node_checksum(ExtFs *fs, const ExtInode *inode,
                                uint8_t *block, uint16_t max_entries,
                                bool set, char **error)
{
    if ((fs->ro_compat & EXT_FEATURE_RO_COMPAT_METADATA_CSUM) == 0U)
        return 0;
    size_t tail = EXT_EXTENT_HEADER_SIZE +
                  (size_t)max_entries * EXT_EXTENT_ENTRY_SIZE;
    if (tail + 4U > fs->block_size) {
        set_error(error, "invalid EXT extent checksum tail");
        return -1;
    }
    uint8_t ino_le[4];
    put_le32(ino_le, inode->number);
    uint32_t crc = crc32c(fs->csum_seed, ino_le, sizeof(ino_le));
    crc = crc32c(crc, inode->raw + 100U, 4U);
    crc = crc32c(crc, block, tail);
    if (set) {
        put_le32(block + tail, crc);
        return 0;
    }
    if (le32(block + tail) != crc) {
        set_error(error, "EXT extent block checksum verification failed");
        return -1;
    }
    return 0;
}

typedef struct {
    ExtFs *fs;
    ExtInode *inode;
    bool writable;
    ExtPayloadVisitor visitor;
    void *context;
    int64_t previous_logical;
    bool have_previous;
} ExtWalk;

static int visit_payload(ExtWalk *walk, int64_t logical,
                         uint64_t *physical, char **error)
{
    if (*physical == 0U || !valid_fs_block(walk->fs, *physical)) {
        set_error(error, "EXT inode payload points outside the filesystem");
        return -1;
    }
    if (walk->have_previous && logical <= walk->previous_logical) {
        set_error(error, "EXT inode payload mapping is not strictly ordered");
        return -1;
    }
    walk->previous_logical = logical;
    walk->have_previous = true;
    uint64_t before = *physical;
    if (walk->visitor(walk->fs, walk->inode->number, logical, physical,
                      walk->writable, walk->context, error) != 0)
        return -1;
    if (!valid_fs_block(walk->fs, *physical)) {
        *physical = before;
        set_error(error, "EXT remap target points outside the filesystem");
        return -1;
    }
    return 0;
}

static int walk_extent_node(ExtWalk *walk, uint8_t *node, size_t node_size,
                            uint64_t node_block, unsigned expected_depth,
                            char **error)
{
    if (node_size < EXT_EXTENT_HEADER_SIZE ||
        le16(node + 0U) != EXT_EXTENT_MAGIC) {
        set_error(error, "invalid EXT extent header");
        return -1;
    }
    uint16_t entries = le16(node + 2U);
    uint16_t maximum = le16(node + 4U);
    uint16_t depth = le16(node + 6U);
    size_t calculated_max =
        (node_size - EXT_EXTENT_HEADER_SIZE) / EXT_EXTENT_ENTRY_SIZE;
    if (depth != expected_depth || depth > EXT_MAX_EXTENT_DEPTH ||
        entries > maximum || maximum == 0U ||
        maximum > calculated_max ||
        EXT_EXTENT_HEADER_SIZE + (size_t)entries * EXT_EXTENT_ENTRY_SIZE >
            node_size) {
        set_error(error, "invalid EXT extent-tree geometry");
        return -1;
    }

    if (node_block != 0U &&
        extent_node_checksum(walk->fs, walk->inode, node, maximum,
                             false, error) != 0)
        return -1;

    bool changed = false;
    for (uint16_t index = 0U; index < entries; ++index) {
        uint8_t *entry =
            node + EXT_EXTENT_HEADER_SIZE +
            (size_t)index * EXT_EXTENT_ENTRY_SIZE;
        if (depth == 0U) {
            uint64_t logical_start = le32(entry + 0U);
            uint16_t encoded_length = le16(entry + 4U);
            uint64_t length = encoded_length > UINT16_C(32768)
                ? (uint64_t)encoded_length - UINT16_C(32768)
                : encoded_length;
            if (length == 0U) {
                set_error(error, "zero-length EXT extent");
                return -1;
            }
            uint64_t physical_start =
                ((uint64_t)le16(entry + 6U) << 32U) | le32(entry + 8U);
            uint64_t new_start = physical_start;
            for (uint64_t offset = 0U; offset < length; ++offset) {
                uint64_t physical = physical_start + offset;
                if (visit_payload(walk,
                        (int64_t)(logical_start + offset),
                        &physical, error) != 0)
                    return -1;
                if (offset == 0U)
                    new_start = physical;
                else if (physical != new_start + offset) {
                    set_error(error,
                        "EXT remap would require reshaping an extent tree");
                    return -1;
                }
            }
            if (walk->writable && new_start != physical_start) {
                put_le16(entry + 6U, (uint16_t)(new_start >> 32U));
                put_le32(entry + 8U, (uint32_t)new_start);
                changed = true;
            }
        } else {
            uint64_t child =
                ((uint64_t)le16(entry + 8U) << 32U) | le32(entry + 4U);
            if (!valid_fs_block(walk->fs, child)) {
                set_error(error, "EXT extent index points outside filesystem");
                return -1;
            }
            uint8_t *child_block = ld_xmalloc(walk->fs->block_size);
            if (read_block(walk->fs, child, child_block, error) != 0) {
                free(child_block);
                return -1;
            }
            if (walk_extent_node(walk, child_block, walk->fs->block_size,
                                 child, depth - 1U, error) != 0) {
                free(child_block);
                return -1;
            }
            free(child_block);
        }
    }

    if (walk->writable && changed) {
        if (node_block == 0U)
            return ext_fs_write_inode(walk->fs, walk->inode, error);
        if (extent_node_checksum(walk->fs, walk->inode, node, maximum,
                                 true, error) != 0 ||
            write_block(walk->fs, node_block, node, error) != 0)
            return -1;
    }
    return 0;
}

static int walk_indirect(ExtWalk *walk, uint64_t block, unsigned level,
                         uint64_t logical_base, char **error)
{
    if (block == 0U)
        return 0;
    uint8_t *buffer = ld_xmalloc(walk->fs->block_size);
    if (read_block(walk->fs, block, buffer, error) != 0) {
        free(buffer);
        return -1;
    }
    uint64_t pointers = walk->fs->block_size / 4U;
    uint64_t span = 1U;
    for (unsigned depth = 1U; depth < level; ++depth) {
        if (span > UINT64_MAX / pointers) {
            free(buffer);
            set_error(error, "EXT indirect-tree span overflow");
            return -1;
        }
        span *= pointers;
    }
    bool changed = false;
    for (uint64_t index = 0U; index < pointers; ++index) {
        uint64_t physical = le32(buffer + index * 4U);
        if (physical == 0U)
            continue;
        uint64_t logical = logical_base + index * span;
        if (level == 1U) {
            uint64_t replacement = physical;
            if (visit_payload(walk, (int64_t)logical,
                              &replacement, error) != 0) {
                free(buffer);
                return -1;
            }
            if (walk->writable && replacement != physical) {
                if (replacement > UINT32_MAX) {
                    free(buffer);
                    set_error(error,
                        "legacy EXT indirect pointers cannot address remap target");
                    return -1;
                }
                put_le32(buffer + index * 4U, (uint32_t)replacement);
                changed = true;
            }
        } else if (walk_indirect(walk, physical, level - 1U,
                                 logical, error) != 0) {
            free(buffer);
            return -1;
        }
    }
    if (walk->writable && changed &&
        write_block(walk->fs, block, buffer, error) != 0) {
        free(buffer);
        return -1;
    }
    free(buffer);
    return 0;
}

int ext_fs_iterate_payload(ExtFs *fs, ExtInode *inode, bool writable,
                           ExtPayloadVisitor visitor, void *context,
                           char **error)
{
    if (inode == NULL || visitor == NULL ||
        (writable && !fs->writable)) {
        set_error(error, "invalid EXT payload iteration request");
        return -1;
    }
    if ((inode->flags & EXT_INODE_INLINE_DATA_FL) != 0U) {
        set_error(error, "EXT inline-data inode is outside the bounded mapper");
        return -1;
    }

    ExtWalk walk = {
        .fs = fs,
        .inode = inode,
        .writable = writable,
        .visitor = visitor,
        .context = context,
    };

    if ((inode->flags & EXT_INODE_EXTENTS_FL) != 0U)
        return walk_extent_node(&walk,
            inode->raw + EXT_INODE_BLOCK_OFFSET,
            EXT_INODE_BLOCK_BYTES, 0U,
            le16(inode->raw + EXT_INODE_BLOCK_OFFSET + 6U), error);

    uint64_t logical = 0U;
    bool inode_changed = false;
    for (unsigned index = 0U; index < EXT_NDIR_BLOCKS; ++index, ++logical) {
        uint8_t *field = inode->raw + EXT_INODE_BLOCK_OFFSET + index * 4U;
        uint64_t physical = le32(field);
        if (physical == 0U)
            continue;
        uint64_t replacement = physical;
        if (visit_payload(&walk, (int64_t)logical,
                          &replacement, error) != 0)
            return -1;
        if (writable && replacement != physical) {
            if (replacement > UINT32_MAX) {
                set_error(error,
                    "legacy EXT direct pointer cannot address remap target");
                return -1;
            }
            put_le32(field, (uint32_t)replacement);
            inode_changed = true;
        }
    }

    uint64_t per = fs->block_size / 4U;
    uint64_t indirects[3] = {
        le32(inode->raw + EXT_INODE_BLOCK_OFFSET + 12U * 4U),
        le32(inode->raw + EXT_INODE_BLOCK_OFFSET + 13U * 4U),
        le32(inode->raw + EXT_INODE_BLOCK_OFFSET + 14U * 4U),
    };
    if (walk_indirect(&walk, indirects[0], 1U, 12U, error) != 0 ||
        walk_indirect(&walk, indirects[1], 2U, 12U + per, error) != 0 ||
        walk_indirect(&walk, indirects[2], 3U,
                      12U + per + per * per, error) != 0)
        return -1;

    if (writable && inode_changed)
        return ext_fs_write_inode(fs, inode, error);
    return 0;
}

static int validate_inode_callback(ExtFs *fs, const ExtInode *inode,
                                   void *context, char **error)
{
    (void)fs;
    (void)inode;
    (void)context;
    (void)error;
    return 0;
}

int ext_fs_validate_metadata(ExtFs *fs, bool verify_inodes, char **error)
{
    if (verify_super_checksum(fs, error) != 0)
        return -1;
    for (uint32_t group = 0U; group < fs->group_count; ++group) {
        if (verify_group_checksum(fs, group, error) != 0 ||
            load_block_bitmap(fs, group, error) != 0 ||
            load_inode_bitmap(fs, group, error) != 0)
            return -1;
    }
    if (verify_inodes)
        return ext_fs_foreach_inode(fs, validate_inode_callback, NULL, error);
    return 0;
}

int ext_fs_flush(ExtFs *fs, char **error)
{
    if (!fs->writable) {
        set_error(error, "EXT flush requires a writable filesystem");
        return -1;
    }
    if (flush_block_bitmap_cache(fs, error) != 0)
        return -1;
    if (fs->free_blocks != fs->original_free_blocks) {
        set_error(error,
            "EXT relayout changed allocation cardinality unexpectedly");
        return -1;
    }
    for (uint32_t group = 0U; group < fs->group_count; ++group)
        set_group_checksum(fs, group);
    if (write_primary_descriptors(fs, error) != 0 ||
        write_old_layout_descriptor_backups(fs, error) != 0)
        return -1;
    if (fsync(fs->fd) != 0) {
        set_error_errno(error, "cannot sync EXT metadata");
        return -1;
    }
    return 0;
}
