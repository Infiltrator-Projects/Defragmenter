// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media_raw.h"
#include "ext_disk.h"
#include "infiltratr/endian.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UNIX_BLOCK 4096U
static void bit_set(uint8_t *map, uint64_t bit) { map[bit / 8U] |= (uint8_t)(1U << (bit % 8U)); }
static bool bit_get(const uint8_t *map, uint64_t bit)
{
    return (map[bit / 8U] & (uint8_t)(1U << (bit % 8U))) != 0U;
}
static uint32_t file_block(const LdtmRawFile *file, uint64_t index)
{
    return (uint32_t)(index < file->count[0] ? file->start[0] + index
                                             : file->start[1] + index - file->count[0]);
}
static uint32_t pointer_tree(int fd, const LdtmRawFile *file, uint64_t begin, uint64_t count,
                             unsigned depth, uint32_t *cursor, uint8_t *used)
{
    uint8_t raw[UNIX_BLOCK] = {0};
    const uint32_t block = (*cursor)++;
    bit_set(used, block);
    const uint64_t stride = depth == 1U ? 1U : 1024U;
    for (uint64_t n = 0U; n < count; n += stride) {
        uint32_t value;
        if (depth == 1U)
            value = file_block(file, begin + n);
        else {
            value = pointer_tree(fd, file, begin + n, count - n < stride ? count - n : stride,
                                 depth - 1U, cursor, used);
            if (value == 0U)
                return 0U;
        }
        infiltratr_store_le32(raw + (n / stride) * 4U, value);
    }
    return ldtm_raw_write(fd, (uint64_t)block * UNIX_BLOCK, raw, sizeof(raw)) == 0 ? block : 0U;
}
static int inode_pointers(int fd, const LdtmRawFile *file, unsigned direct, uint8_t *pointers,
                          uint32_t *cursor, uint8_t *used)
{
    const uint64_t total = file->count[0] + file->count[1];
    for (uint64_t n = 0U; n < total && n < direct; ++n)
        infiltratr_store_le32(pointers + n * 4U, file_block(file, n));
    uint64_t begin = direct;
    for (unsigned depth = 1U; begin < total && depth <= 2U; ++depth) {
        const uint64_t limit = depth == 1U ? 1024U : UINT64_C(1024) * 1024U;
        const uint64_t count = total - begin < limit ? total - begin : limit;
        const uint32_t tree = pointer_tree(fd, file, begin, count, depth, cursor, used);
        if (tree == 0U)
            return -1;
        infiltratr_store_le32(pointers + (direct + depth - 1U) * 4U, tree);
        begin += count;
    }
    return begin >= total ? 0 : -1;
}
typedef struct {
    LdtmRawCheck *check;
    int fd;
    unsigned file;
} ExtCheck;
static int check_ext_payload(ExtFs *fs, uint32_t ino, int64_t logical, uint64_t *physical,
                             bool mutable_mapping, void *context, char **error)
{
    (void)fs;
    (void)ino;
    (void)mutable_mapping;
    (void)error;
    ExtCheck *state = context;
    if (logical < 0)
        return 0;
    const uint64_t offset = (uint64_t)logical * UNIX_BLOCK;
    const uint64_t bytes = ldtm_raw_file_bytes(state->check->spec, state->file);
    if (offset >= bytes)
        return -1;
    const uint64_t take = bytes - offset < UNIX_BLOCK ? bytes - offset : UNIX_BLOCK;
    return *physical != 0U ? ldtm_raw_check_data(state->check, state->fd, state->file,
                                                 *physical * UNIX_BLOCK, offset, take)
                           : -1;
}

int ldtm_raw_ext(const LdtmFilesystemSpec *spec, const char *path, bool create)
{
    uint64_t capacity = 0U;
    const int fd = ldtm_raw_open(path, create, &capacity);
    if (fd < 0)
        return -1;
    int result = -1;
    uint8_t *used = NULL;
    if (!create) {
        ExtFs *fs = NULL;
        char *error = NULL;
        if (ext_fs_open(path, false, &fs, &error) != 0) {
            free(error);
            goto done;
        }
        LdtmRawCheck check;
        ldtm_raw_check_init(&check, spec);
        bool valid = ext_fs_block_size(fs) == UNIX_BLOCK;
        for (unsigned file = 0U; valid && file < LDTM_RAW_FILES; ++file) {
            ExtInode inode = {0};
            if (ext_fs_read_inode(fs, 11U + file, &inode, &error) != 0) {
                valid = false;
                break;
            }
            ExtCheck context = {&check, fd, file};
            valid =
                (inode.mode & 0170000U) == 0100000U &&
                ldtm_raw_check_file(&check, file, inode.size) == 0 &&
                ext_fs_iterate_payload(fs, &inode, false, check_ext_payload, &context, &error) == 0;
            ext_inode_destroy(&inode);
        }
        result = valid ? ldtm_raw_check_finish(&check) : -1;
        ext_fs_close(fs);
        free(error);
        goto done;
    }
    if (capacity != (uint64_t)spec->size_mib * LDTM_MIB || capacity % UNIX_BLOCK != 0U)
        goto done;
    const uint32_t blocks = (uint32_t)(capacity / UNIX_BLOCK), groups = (blocks + 32767U) / 32768U;
    if (groups != 16U)
        goto done;
    used = calloc((blocks + 7U) / 8U, 1U);
    if (used == NULL)
        goto done;
    LdtmRawPlan plan;
    if (ldtm_raw_plan(spec, capacity, UNIX_BLOCK, 4096U, &plan) != 0)
        goto done;
    /* Keep the two payload halves in separate block groups. Every group has
     * its own superblock, descriptor, bitmaps and inode table. */
    const uint64_t second_delta = 32768U + 4096U - plan.files[0].start[1];
    for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file)
        if (plan.files[file].count[1] != 0U)
            plan.files[file].start[1] += second_delta;
    plan.end += second_delta;
    uint8_t super[1024] = {0}, descriptors[UNIX_BLOCK] = {0}, inodes[16384] = {0},
            root[UNIX_BLOCK] = {0};
    const bool extents = spec->creator == LDTM_CREATOR_EXT4;
    const bool journal = spec->creator == LDTM_CREATOR_EXT3 || extents;
    for (uint32_t group = 0U; group < groups; ++group)
        for (uint32_t n = 0U; n < 8U; ++n)
            bit_set(used, (uint64_t)group * 32768U + n);
    bit_set(used, 8U);
    uint8_t *root_inode = inodes + 128U;
    infiltratr_store_le16(root_inode, 0040755U);
    infiltratr_store_le32(root_inode + 4U, UNIX_BLOCK);
    infiltratr_store_le16(root_inode + 26U, 2U);
    infiltratr_store_le32(root_inode + 28U, 8U);
    infiltratr_store_le32(root_inode + 40U, 8U);
    uint32_t cursor = 16U;
    for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file) {
        const LdtmRawFile *item = &plan.files[file];
        uint8_t *inode = inodes + (10U + file) * 128U;
        infiltratr_store_le16(inode, 0100644U);
        infiltratr_store_le16(inode + 26U, 1U);
        infiltratr_store_le32(inode + 4U, (uint32_t)item->bytes);
        const uint32_t before = cursor;
        if (extents) {
            infiltratr_store_le32(inode + 32U, UINT32_C(0x00080000));
            infiltratr_store_le16(inode + 40U, 0xf30aU);
            infiltratr_store_le16(inode + 44U, 4U);
            unsigned entries = 0U;
            for (unsigned part = 0U; part < 2U; ++part) {
                if (item->count[part] == 0U)
                    continue;
                uint8_t *extent = inode + 52U + entries * 12U;
                infiltratr_store_le32(extent, part == 0U ? 0U : (uint32_t)item->count[0]);
                infiltratr_store_le16(extent + 4U, (uint16_t)item->count[part]);
                infiltratr_store_le32(extent + 8U, (uint32_t)item->start[part]);
                ++entries;
            }
            infiltratr_store_le16(inode + 42U, (uint16_t)entries);
        } else if (inode_pointers(fd, item, 12U, inode + 40U, &cursor, used) != 0)
            goto done;
        infiltratr_store_le32(inode + 28U,
                              (uint32_t)((item->count[0] + item->count[1] + cursor - before) * 8U));
        for (unsigned part = 0U; part < 2U; ++part)
            for (uint64_t n = 0U; n < item->count[part]; ++n)
                bit_set(used, item->start[part] + n);
    }
    if (journal) {
        const LdtmRawFile log = {
            .bytes = 4U * LDTM_MIB, .start = {1024U, 0U}, .count = {1024U, 0U}};
        uint8_t *inode = inodes + 7U * 128U;
        infiltratr_store_le16(inode, 0100600U);
        infiltratr_store_le16(inode + 26U, 1U);
        infiltratr_store_le32(inode + 4U, (uint32_t)log.bytes);
        const uint32_t before = cursor;
        if (inode_pointers(fd, &log, 12U, inode + 40U, &cursor, used) != 0)
            goto done;
        infiltratr_store_le32(inode + 28U, (1024U + cursor - before) * 8U);
        for (uint32_t n = 1024U; n < 2048U; ++n)
            bit_set(used, n);
        uint8_t jbd[UNIX_BLOCK] = {0};
        infiltratr_store_be32(jbd, UINT32_C(0xc03b3998));
        infiltratr_store_be32(jbd + 4U, 4U);
        infiltratr_store_be32(jbd + 12U, UNIX_BLOCK);
        infiltratr_store_be32(jbd + 16U, 1024U);
        infiltratr_store_be32(jbd + 20U, 1U);
        infiltratr_store_be32(jbd + 24U, 1U);
        infiltratr_store_be32(jbd + 64U, 1U);
        memcpy(jbd + 48U, "LDTM-EXT-FIXTURE", 16U);
        if (ldtm_raw_write(fd, 1024U * UNIX_BLOCK, jbd, sizeof(jbd)) != 0)
            goto done;
    }
    size_t offset = 0U;
    for (unsigned entry = 0U; entry < LDTM_RAW_FILES + 2U; ++entry) {
        char name[16];
        if (entry == 0U)
            memcpy(name, ".", 2U);
        else if (entry == 1U)
            memcpy(name, "..", 3U);
        else
            ldtm_raw_name(entry - 2U, name);
        const size_t length = strlen(name), record = entry == LDTM_RAW_FILES + 1U
                                                         ? UNIX_BLOCK - offset
                                                         : (8U + length + 3U) & ~(size_t)3U;
        infiltratr_store_le32(root + offset, entry < 2U ? 2U : 11U + entry - 2U);
        infiltratr_store_le16(root + offset + 4U, (uint16_t)record);
        root[offset + 6U] = (uint8_t)length;
        root[offset + 7U] = entry < 2U ? 2U : 1U;
        memcpy(root + offset + 8U, name, length);
        offset += record;
    }
    uint64_t free_blocks = 0U;
    for (uint32_t group = 0U; group < groups; ++group) {
        const uint32_t base = group * 32768U;
        uint8_t bb[UNIX_BLOCK] = {0}, ib[UNIX_BLOCK];
        memset(ib, 0xff, sizeof(ib));
        memset(ib, 0, 16U);
        if (group == 0U)
            for (unsigned n = 0U; n < 26U; ++n)
                bit_set(ib, n);
        uint32_t free_count = 0U;
        for (uint32_t n = 0U; n < 32768U; ++n) {
            if (bit_get(used, base + n))
                bit_set(bb, n);
            else
                ++free_count;
        }
        free_blocks += free_count;
        uint8_t *desc = descriptors + group * 32U;
        infiltratr_store_le32(desc, base + 2U);
        infiltratr_store_le32(desc + 4U, base + 3U);
        infiltratr_store_le32(desc + 8U, base + 4U);
        infiltratr_store_le16(desc + 12U, (uint16_t)free_count);
        infiltratr_store_le16(desc + 14U, (uint16_t)(group == 0U ? 102U : 128U));
        infiltratr_store_le16(desc + 16U, group == 0U ? 1U : 0U);
        if (ldtm_raw_write(fd, (uint64_t)(base + 2U) * UNIX_BLOCK, bb, sizeof(bb)) != 0 ||
            ldtm_raw_write(fd, (uint64_t)(base + 3U) * UNIX_BLOCK, ib, sizeof(ib)) != 0)
            goto done;
        uint8_t empty[16384] = {0};
        if (ldtm_raw_write(fd, (uint64_t)(base + 4U) * UNIX_BLOCK, group == 0U ? inodes : empty,
                           sizeof(empty)) != 0)
            goto done;
    }
    infiltratr_store_le32(super, groups * 128U);
    infiltratr_store_le32(super + 4U, blocks);
    infiltratr_store_le32(super + 12U, (uint32_t)free_blocks);
    infiltratr_store_le32(super + 16U, groups * 128U - 26U);
    infiltratr_store_le32(super + 24U, 2U);
    infiltratr_store_le32(super + 28U, 2U);
    infiltratr_store_le32(super + 32U, 32768U);
    infiltratr_store_le32(super + 36U, 32768U);
    infiltratr_store_le32(super + 40U, 128U);
    infiltratr_store_le16(super + 56U, 0xef53U);
    infiltratr_store_le16(super + 58U, 1U);
    infiltratr_store_le16(super + 60U, 1U);
    infiltratr_store_le32(super + 76U, 1U);
    infiltratr_store_le32(super + 84U, 11U);
    infiltratr_store_le16(super + 88U, 128U);
    infiltratr_store_le32(super + 92U, journal ? 4U : 0U);
    infiltratr_store_le32(super + 96U, extents ? 0x42U : 2U);
    infiltratr_store_le32(super + 100U, 2U);
    memcpy(super + 104U, "LDTM-EXT-FIXTURE", 16U);
    memcpy(super + 120U, spec->label, strlen(spec->label));
    if (journal)
        infiltratr_store_le32(super + 224U, 8U);
    for (uint32_t group = 0U; group < groups; ++group) {
        infiltratr_store_le16(super + 90U, (uint16_t)group);
        if (ldtm_raw_write(fd, (uint64_t)group * 32768U * UNIX_BLOCK + (group == 0U ? 1024U : 0U),
                           super, sizeof(super)) != 0 ||
            ldtm_raw_write(fd, ((uint64_t)group * 32768U + 1U) * UNIX_BLOCK, descriptors,
                           sizeof(descriptors)) != 0)
            goto done;
    }
    if (ldtm_raw_write(fd, 8U * UNIX_BLOCK, root, sizeof(root)) != 0 ||
        ldtm_raw_write_payload(fd, &plan) != 0)
        goto done;
    result = 0;
done:
    free(used);
    if (close(fd) != 0)
        result = -1;
    return result;
}

