// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"
#include "pfs3_native.h"

#include <infiltratr/endian.h>
#include <infiltratr/posix_io.h>

#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PFS_TM_SECTOR_SIZE 512U
#define PFS_TM_RESBLOCK_SIZE 1024U
#define PFS_TM_CAP_BYTES (UINT64_C(2) * LDTM_GIB)
#define PFS_TM_FIRST_RESERVED 2U
#define PFS_TM_LAST_RESERVED 2049U
#define PFS_TM_RESCLUSTER 2U
#define PFS_TM_EXTENSION 4U
#define PFS_TM_BITMAP_INDEX0 6U
#define PFS_TM_ROOT_ANODE 5U
#define PFS_TM_FILE_ANODE 6U
#define PFS_TM_FRAGMENTS 100U
#define PFS_TM_CHUNK_SECTORS 4096U
#define PFS_TM_CHUNK_KIB 2048U
#define PFS_TM_DATA_START 4096U
#define PFS_TM_DATA_STRIDE 6144U
#define PFS_TM_DATA_SECTORS (PFS_TM_FRAGMENTS * PFS_TM_CHUNK_SECTORS)
#define PFS_TM_BITMAP_BITS (((PFS_TM_RESBLOCK_SIZE / 4U) - 3U) * 32U)
#define PFS_TM_INDEX_POINTERS ((PFS_TM_RESBLOCK_SIZE - 12U) / 4U)
#define PFS_TM_MAX_BITMAP_INDEXES 5U
#define PFS_TM_OPTIONS UINT32_C(0x677)

typedef struct {
    uint32_t total_sectors;
    uint32_t bitmap_blocks;
    uint32_t bitmap_indexes;
    uint32_t anode_index;
    uint32_t anode_block0;
    uint32_t anode_block1;
    uint32_t root_dir;
    uint32_t bitmap_first;
    uint32_t metadata_slots;
} PfsTmGeometry;

static uint32_t fragment_start(uint32_t index)
{
    return PFS_TM_DATA_START + index * PFS_TM_DATA_STRIDE;
}

static int profile_matches(const LdtmFragmentProfile *profile)
{
    if (profile == NULL ||
        profile->files != LDTM_TARGET_FILE_COUNT ||
        profile->chunks != 30U ||
        profile->chunk_kib != PFS_TM_CHUNK_KIB ||
        profile->directory_initial != 0U ||
        profile->directory_second != 0U)
        return 0;
    static const uint32_t expected[LDTM_TARGET_FILE_COUNT] =
        {2U, 3U, 5U, 8U, 13U, 17U, 22U, 30U};
    for (size_t file = 0U; file < LDTM_TARGET_FILE_COUNT; ++file)
        if (ldtm_profile_file_chunks(profile, file) != expected[file])
            return 0;
    return ldtm_profile_payload_bytes(profile) == UINT64_C(200) * LDTM_MIB;
}

static int fragment_owner(const LdtmFragmentProfile *profile,
                          uint32_t fragment,
                          uint32_t *file_index,
                          uint32_t *within_file)
{
    uint32_t first = 0U;
    for (uint32_t file = 0U; file < profile->files; ++file) {
        const uint32_t count = ldtm_profile_file_chunks(profile, file);
        if (fragment < first + count) {
            if (file_index != NULL) *file_index = file;
            if (within_file != NULL) *within_file = fragment - first;
            return 0;
        }
        first += count;
    }
    return -1;
}

static uint32_t first_fragment_for_file(const LdtmFragmentProfile *profile,
                                        uint32_t file_index)
{
    uint32_t first = 0U;
    for (uint32_t file = 0U; file < file_index; ++file)
        first += ldtm_profile_file_chunks(profile, file);
    return first;
}

static uint32_t extent_anode(uint32_t index)
{
    const uint32_t first_block_capacity =
        (PFS_TM_RESBLOCK_SIZE - 16U) / 12U - PFS_TM_FILE_ANODE;
    if (index < first_block_capacity)
        return PFS_TM_FILE_ANODE + index;
    return UINT32_C(0x10000) | (index - first_block_capacity);
}

static uint32_t bitmap_index_sector(uint32_t group)
{
    return PFS_TM_BITMAP_INDEX0 + group * PFS_TM_RESCLUSTER;
}

static int geometry_for_path(const char *path, PfsTmGeometry *geometry)
{
    if (path == NULL || geometry == NULL)
        return -1;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    const off_t end = lseek(fd, 0, SEEK_END);
    (void)close(fd);
    if (end <= 0)
        return -1;

    uint64_t bytes = (uint64_t)end;
    if (bytes > PFS_TM_CAP_BYTES)
        bytes = PFS_TM_CAP_BYTES;
    if (bytes % PFS_TM_SECTOR_SIZE != 0U)
        return -1;
    const uint64_t sectors64 = bytes / PFS_TM_SECTOR_SIZE;
    if (sectors64 > UINT32_MAX ||
        sectors64 <= PFS_TM_LAST_RESERVED + 1U ||
        sectors64 <=
            (uint64_t)fragment_start(PFS_TM_FRAGMENTS - 1U) +
                PFS_TM_CHUNK_SECTORS)
        return -1;

    PfsTmGeometry value = {0};
    value.total_sectors = (uint32_t)sectors64;
    const uint32_t normal_blocks =
        value.total_sectors - (PFS_TM_LAST_RESERVED + 1U);
    value.bitmap_blocks =
        (normal_blocks + PFS_TM_BITMAP_BITS - 1U) / PFS_TM_BITMAP_BITS;
    value.bitmap_indexes =
        (value.bitmap_blocks + PFS_TM_INDEX_POINTERS - 1U) /
        PFS_TM_INDEX_POINTERS;
    if (value.bitmap_blocks == 0U || value.bitmap_indexes == 0U ||
        value.bitmap_indexes > PFS_TM_MAX_BITMAP_INDEXES)
        return -1;

    value.anode_index =
        bitmap_index_sector(value.bitmap_indexes);
    value.anode_block0 = value.anode_index + PFS_TM_RESCLUSTER;
    value.anode_block1 = value.anode_block0 + PFS_TM_RESCLUSTER;
    value.root_dir = value.anode_block1 + PFS_TM_RESCLUSTER;
    value.bitmap_first = value.root_dir + PFS_TM_RESCLUSTER;
    value.metadata_slots =
        6U + value.bitmap_indexes + value.bitmap_blocks;

    const uint32_t reserved_slots =
        (PFS_TM_LAST_RESERVED - PFS_TM_FIRST_RESERVED + 1U) /
        PFS_TM_RESCLUSTER;
    const uint64_t bitmap_end =
        (uint64_t)value.bitmap_first +
        (uint64_t)value.bitmap_blocks * PFS_TM_RESCLUSTER;
    if (value.metadata_slots > reserved_slots ||
        bitmap_end > (uint64_t)PFS_TM_LAST_RESERVED + 1U)
        return -1;

    *geometry = value;
    return 0;
}

static void bitmap_set(uint8_t *bitmap, uint32_t bit, int is_free)
{
    const uint32_t word = bit / 32U;
    const uint32_t within = bit % 32U;
    uint8_t *slot = bitmap + word * 4U;
    uint32_t value = infiltratr_load_be32(slot);
    const uint32_t mask = UINT32_C(0x80000000) >> within;
    if (is_free != 0)
        value |= mask;
    else
        value &= ~mask;
    infiltratr_store_be32(slot, value);
}

static int write_bytes(int fd, const void *buffer, size_t bytes, uint64_t offset)
{
    return infiltratr_pwrite_full(fd, buffer, bytes, offset);
}

static int write_sector_block(int fd, uint32_t sector,
                              const void *buffer, size_t bytes)
{
    return write_bytes(fd, buffer, bytes,
                       (uint64_t)sector * PFS_TM_SECTOR_SIZE);
}

static int read_sector_block(int fd, uint32_t sector,
                             void *buffer, size_t bytes)
{
    return infiltratr_pread_full(
        fd, buffer, bytes,
        (uint64_t)sector * PFS_TM_SECTOR_SIZE);
}

static void make_root(uint8_t block[PFS_TM_SECTOR_SIZE],
                      const PfsTmGeometry *geometry)
{
    const uint32_t reserved_slots =
        (PFS_TM_LAST_RESERVED - PFS_TM_FIRST_RESERVED + 1U) /
        PFS_TM_RESCLUSTER;
    const uint32_t normal_blocks =
        geometry->total_sectors - (PFS_TM_LAST_RESERVED + 1U);
    memset(block, 0, PFS_TM_SECTOR_SIZE);
    infiltratr_store_be32(block, UINT32_C(0x50465301));
    infiltratr_store_be32(block + 4U, PFS_TM_OPTIONS);
    infiltratr_store_be32(block + 8U, UINT32_C(0x20260926));
    block[20U] = 7U;
    memcpy(block + 21U, "LD_PFS3", 7U);
    infiltratr_store_be32(block + 52U, PFS_TM_LAST_RESERVED);
    infiltratr_store_be32(block + 56U, PFS_TM_FIRST_RESERVED);
    infiltratr_store_be32(
        block + 60U, reserved_slots - geometry->metadata_slots);
    infiltratr_store_be16(block + 64U, PFS_TM_RESBLOCK_SIZE);
    infiltratr_store_be16(block + 66U, PFS_TM_RESCLUSTER);
    infiltratr_store_be32(
        block + 68U, normal_blocks - PFS_TM_DATA_SECTORS);
    infiltratr_store_be32(block + 72U, 0U);
    infiltratr_store_be32(block + 76U, 0U);
    infiltratr_store_be32(block + 84U, geometry->total_sectors);
    infiltratr_store_be32(block + 88U, PFS_TM_EXTENSION);
    for (uint32_t group = 0U; group < geometry->bitmap_indexes; ++group)
        infiltratr_store_be32(
            block + 96U + group * 4U,
            bitmap_index_sector(group));
    infiltratr_store_be32(block + 116U, geometry->anode_index);
}

static void make_reserved_bitmap(uint8_t block[PFS_TM_SECTOR_SIZE],
                                 const PfsTmGeometry *geometry)
{
    const uint32_t reserved_slots =
        (PFS_TM_LAST_RESERVED - PFS_TM_FIRST_RESERVED + 1U) /
        PFS_TM_RESCLUSTER;
    memset(block, 0, PFS_TM_SECTOR_SIZE);
    infiltratr_store_be16(block, UINT16_C(0x424d));
    infiltratr_store_be32(block + 4U, UINT32_C(0x20260926));
    infiltratr_store_be32(block + 8U, 0U);
    for (uint32_t slot = 0U; slot < reserved_slots; ++slot)
        bitmap_set(block + 12U, slot, 1);
    for (uint32_t slot = 0U; slot < geometry->metadata_slots; ++slot)
        bitmap_set(block + 12U, slot, 0);
}

static void make_extension(uint8_t block[PFS_TM_RESBLOCK_SIZE])
{
    memset(block, 0, PFS_TM_RESBLOCK_SIZE);
    infiltratr_store_be16(block, UINT16_C(0x4558));
    infiltratr_store_be32(block + 8U, UINT32_C(0x20260926));
    infiltratr_store_be32(block + 28U, 0U);
}

static void make_bitmap_index(uint8_t block[PFS_TM_RESBLOCK_SIZE],
                              const PfsTmGeometry *geometry,
                              uint32_t group)
{
    memset(block, 0, PFS_TM_RESBLOCK_SIZE);
    infiltratr_store_be16(block, UINT16_C(0x4d49));
    infiltratr_store_be32(block + 4U, UINT32_C(0x20260926));
    infiltratr_store_be32(block + 8U, group);
    const uint32_t first = group * PFS_TM_INDEX_POINTERS;
    const uint32_t remaining =
        geometry->bitmap_blocks > first
            ? geometry->bitmap_blocks - first : 0U;
    const uint32_t count =
        remaining > PFS_TM_INDEX_POINTERS
            ? PFS_TM_INDEX_POINTERS : remaining;
    for (uint32_t slot = 0U; slot < count; ++slot) {
        const uint32_t sequence = first + slot;
        infiltratr_store_be32(
            block + 12U + slot * 4U,
            geometry->bitmap_first +
                sequence * PFS_TM_RESCLUSTER);
    }
}

static void make_allocation_bitmap(uint8_t block[PFS_TM_RESBLOCK_SIZE],
                                   uint32_t sequence,
                                   const PfsTmGeometry *geometry)
{
    const uint32_t first_data = PFS_TM_LAST_RESERVED + 1U;
    const uint32_t base =
        first_data + sequence * PFS_TM_BITMAP_BITS;
    memset(block, 0, PFS_TM_RESBLOCK_SIZE);
    infiltratr_store_be16(block, UINT16_C(0x424d));
    infiltratr_store_be32(block + 4U, UINT32_C(0x20260926));
    infiltratr_store_be32(block + 8U, sequence);
    memset(block + 12U, 0xff, PFS_TM_RESBLOCK_SIZE - 12U);

    for (uint32_t fragment = 0U; fragment < PFS_TM_FRAGMENTS; ++fragment) {
        const uint32_t start = fragment_start(fragment);
        const uint32_t end = start + PFS_TM_CHUNK_SECTORS;
        const uint32_t map_end = base + PFS_TM_BITMAP_BITS;
        const uint32_t from = start > base ? start : base;
        const uint32_t to = end < map_end ? end : map_end;
        for (uint32_t sector = from; sector < to; ++sector)
            bitmap_set(block + 12U, sector - base, 0);
    }
    if (base < geometry->total_sectors &&
        base + PFS_TM_BITMAP_BITS > geometry->total_sectors) {
        for (uint32_t sector = geometry->total_sectors;
             sector < base + PFS_TM_BITMAP_BITS; ++sector)
            bitmap_set(block + 12U, sector - base, 0);
    }
}

static void put_anode(uint8_t block[PFS_TM_RESBLOCK_SIZE], uint32_t offset,
                      uint32_t clusters, uint32_t sector, uint32_t next)
{
    uint8_t *record = block + 16U + (size_t)offset * 12U;
    infiltratr_store_be32(record, clusters);
    infiltratr_store_be32(record + 4U, sector);
    infiltratr_store_be32(record + 8U, next);
}

static void make_anode_index(uint8_t block[PFS_TM_RESBLOCK_SIZE],
                             const PfsTmGeometry *geometry)
{
    memset(block, 0, PFS_TM_RESBLOCK_SIZE);
    infiltratr_store_be16(block, UINT16_C(0x4942));
    infiltratr_store_be32(block + 4U, UINT32_C(0x20260926));
    infiltratr_store_be32(block + 8U, 0U);
    infiltratr_store_be32(block + 12U, geometry->anode_block0);
    infiltratr_store_be32(block + 16U, geometry->anode_block1);
}

static void make_anode_block(uint8_t block[PFS_TM_RESBLOCK_SIZE],
                             const PfsTmGeometry *geometry,
                             const LdtmFragmentProfile *profile,
                             uint32_t sequence)
{
    const uint32_t per_block =
        (PFS_TM_RESBLOCK_SIZE - 16U) / 12U;
    memset(block, 0, PFS_TM_RESBLOCK_SIZE);
    infiltratr_store_be16(block, UINT16_C(0x4142));
    infiltratr_store_be32(block + 4U, UINT32_C(0x20260926));
    infiltratr_store_be32(block + 8U, sequence);
    if (sequence == 0U)
        put_anode(block, PFS_TM_ROOT_ANODE, 1U,
                  geometry->root_dir, 0U);
    for (uint32_t index = 0U; index < PFS_TM_FRAGMENTS; ++index) {
        uint32_t file = 0U;
        uint32_t within = 0U;
        if (fragment_owner(profile, index, &file, &within) != 0)
            continue;
        const uint32_t number = extent_anode(index);
        const uint32_t seq = number >> 16U;
        const uint32_t offset = number & UINT32_C(0xffff);
        if (seq != sequence || offset >= per_block)
            continue;
        const uint32_t chunks = ldtm_profile_file_chunks(profile, file);
        const uint32_t next =
            within + 1U < chunks ? extent_anode(index + 1U) : 0U;
        put_anode(block, offset, PFS_TM_CHUNK_SECTORS,
                  fragment_start(index), next);
    }
}

static void make_root_directory(uint8_t block[PFS_TM_RESBLOCK_SIZE],
                                const LdtmFragmentProfile *profile)
{
    memset(block, 0, PFS_TM_RESBLOCK_SIZE);
    infiltratr_store_be16(block, UINT16_C(0x4442));
    infiltratr_store_be32(block + 4U, UINT32_C(0x20260926));
    infiltratr_store_be32(block + 12U, PFS_TM_ROOT_ANODE);
    infiltratr_store_be32(block + 16U, 0U);
    size_t offset = 20U;
    for (uint32_t file = 0U; file < profile->files; ++file) {
        char name[32];
        (void)snprintf(name, sizeof(name), "fragmented-%02u.bin", file);
        const size_t name_length = strlen(name);
        const uint64_t file_bytes = ldtm_profile_file_bytes(profile, file);
        const uint32_t first = first_fragment_for_file(profile, file);
        if (offset + 40U > PFS_TM_RESBLOCK_SIZE ||
            file_bytes == 0U || file_bytes > UINT32_MAX)
            return;
        uint8_t *entry = block + offset;
        entry[0] = 40U;
        entry[1] = UINT8_C(0xfd);
        infiltratr_store_be32(entry + 2U, extent_anode(first));
        infiltratr_store_be32(entry + 6U, (uint32_t)file_bytes);
        entry[17U] = (uint8_t)name_length;
        memcpy(entry + 18U, name, name_length);
        entry[18U + name_length] = 0U;
        offset += 40U;
    }
}

static void make_payload(uint8_t sector[PFS_TM_SECTOR_SIZE],
                         uint32_t file_index, uint32_t logical_sector)
{
    uint64_t state = UINT64_C(0x243f6a8885a308d3) ^
                     ((uint64_t)file_index << 40U) ^
                     ((uint64_t)logical_sector *
                      UINT64_C(0x9e3779b97f4a7c15));
    for (size_t offset = 0U; offset < PFS_TM_SECTOR_SIZE; ++offset) {
        state ^= state >> 12U;
        state ^= state << 25U;
        state ^= state >> 27U;
        state *= UINT64_C(0x2545f4914f6cdd1d);
        sector[offset] = (uint8_t)(state >> 56U);
    }
}

int ldtm_format_pfs3_volume(const char *path)
{
    const LdtmFilesystemSpec *spec = ldtm_find_spec("pfs3");
    const LdtmFragmentProfile profile = ldtm_fragment_profile(spec);
    PfsTmGeometry geometry;
    if (spec == NULL || !profile_matches(&profile) ||
        geometry_for_path(path, &geometry) != 0)
        return -1;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;

    uint8_t *block = malloc(PFS_TM_RESBLOCK_SIZE);
    if (block == NULL) {
        (void)close(fd);
        return -1;
    }
    int result = -1;

    make_root(block, &geometry);
    if (write_sector_block(fd, PFS_TM_FIRST_RESERVED,
                           block, PFS_TM_SECTOR_SIZE) != 0)
        goto done;
    make_reserved_bitmap(block, &geometry);
    if (write_sector_block(fd, PFS_TM_FIRST_RESERVED + 1U,
                           block, PFS_TM_SECTOR_SIZE) != 0)
        goto done;
    make_extension(block);
    if (write_sector_block(fd, PFS_TM_EXTENSION,
                           block, PFS_TM_RESBLOCK_SIZE) != 0)
        goto done;

    for (uint32_t group = 0U; group < geometry.bitmap_indexes; ++group) {
        make_bitmap_index(block, &geometry, group);
        if (write_sector_block(
                fd, bitmap_index_sector(group),
                block, PFS_TM_RESBLOCK_SIZE) != 0)
            goto done;
    }

    make_anode_index(block, &geometry);
    if (write_sector_block(fd, geometry.anode_index,
                           block, PFS_TM_RESBLOCK_SIZE) != 0)
        goto done;
    make_anode_block(block, &geometry, &profile, 0U);
    if (write_sector_block(fd, geometry.anode_block0,
                           block, PFS_TM_RESBLOCK_SIZE) != 0)
        goto done;
    make_anode_block(block, &geometry, &profile, 1U);
    if (write_sector_block(fd, geometry.anode_block1,
                           block, PFS_TM_RESBLOCK_SIZE) != 0)
        goto done;
    make_root_directory(block, &profile);
    if (write_sector_block(fd, geometry.root_dir,
                           block, PFS_TM_RESBLOCK_SIZE) != 0)
        goto done;

    for (uint32_t sequence = 0U;
         sequence < geometry.bitmap_blocks; ++sequence) {
        make_allocation_bitmap(block, sequence, &geometry);
        if (write_sector_block(
                fd,
                geometry.bitmap_first +
                    sequence * PFS_TM_RESCLUSTER,
                block, PFS_TM_RESBLOCK_SIZE) != 0)
            goto done;
    }

    for (uint32_t fragment = 0U; fragment < PFS_TM_FRAGMENTS; ++fragment) {
        uint32_t file = 0U;
        uint32_t within_file = 0U;
        if (fragment_owner(&profile, fragment, &file, &within_file) != 0)
            goto done;
        const uint32_t start = fragment_start(fragment);
        for (uint32_t within = 0U;
             within < PFS_TM_CHUNK_SECTORS; ++within) {
            make_payload(
                block, file,
                within_file * PFS_TM_CHUNK_SECTORS + within);
            if (write_sector_block(fd, start + within,
                                   block, PFS_TM_SECTOR_SIZE) != 0)
                goto done;
        }
    }
    if (fsync(fd) != 0)
        goto done;
    result = 0;

done:
    free(block);
    (void)close(fd);
    return result;
}

int ldtm_populate_pfs3_volume(
    const char *path, const LdtmFragmentProfile *profile)
{
    Pfs3Analysis analysis;
    char error[256] = {0};
    if (!profile_matches(profile))
        return -1;
    return pfs3_analyse(path, &analysis, NULL, 0U,
                        error, sizeof(error)) == 0 &&
           analysis.regular_files == LDTM_TARGET_FILE_COUNT &&
           analysis.data_blocks == PFS_TM_DATA_SECTORS &&
           analysis.fragmented_files == LDTM_TARGET_FILE_COUNT &&
           !analysis.growth_10_satisfied ? 0 : -1;
}

static int read_anode(int fd, uint32_t anode_index_sector,
                      uint32_t number, uint32_t *clusters,
                      uint32_t *sector, uint32_t *next)
{
    uint8_t index[PFS_TM_RESBLOCK_SIZE];
    uint8_t block[PFS_TM_RESBLOCK_SIZE];
    const uint32_t sequence = number >> 16U;
    const uint32_t offset = number & UINT32_C(0xffff);
    const uint32_t per_block =
        (PFS_TM_RESBLOCK_SIZE - 16U) / 12U;
    if (offset >= per_block ||
        read_sector_block(fd, anode_index_sector,
                          index, sizeof(index)) != 0 ||
        infiltratr_load_be16(index) != UINT16_C(0x4942) ||
        sequence >= PFS_TM_INDEX_POINTERS)
        return -1;
    const uint32_t anode_block =
        infiltratr_load_be32(index + 12U + sequence * 4U);
    if (anode_block == 0U ||
        read_sector_block(fd, anode_block,
                          block, sizeof(block)) != 0 ||
        infiltratr_load_be16(block) != UINT16_C(0x4142) ||
        infiltratr_load_be32(block + 8U) != sequence)
        return -1;
    const uint8_t *record =
        block + 16U + (size_t)offset * 12U;
    *clusters = infiltratr_load_be32(record);
    *sector = infiltratr_load_be32(record + 4U);
    *next = infiltratr_load_be32(record + 8U);
    return 0;
}

static int verify_current_payload(const char *path,
                                  const LdtmFragmentProfile *profile,
                                  int expect_fragmented)
{
    uint8_t root[PFS_TM_SECTOR_SIZE];
    uint8_t directory[PFS_TM_RESBLOCK_SIZE];
    uint8_t actual[PFS_TM_SECTOR_SIZE];
    uint8_t expected[PFS_TM_SECTOR_SIZE];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int result = -1;

    if (read_sector_block(fd, PFS_TM_FIRST_RESERVED,
                          root, sizeof(root)) != 0 ||
        infiltratr_load_be32(root) != UINT32_C(0x50465301))
        goto done;
    const uint32_t total_sectors =
        infiltratr_load_be32(root + 84U);
    const uint32_t anode_index =
        infiltratr_load_be32(root + 116U);
    if (anode_index == 0U || total_sectors <= PFS_TM_LAST_RESERVED)
        goto done;

    uint32_t clusters = 0U;
    uint32_t root_dir = 0U;
    uint32_t next = 0U;
    if (read_anode(fd, anode_index, PFS_TM_ROOT_ANODE,
                   &clusters, &root_dir, &next) != 0 ||
        clusters != 1U || next != 0U ||
        root_dir < PFS_TM_FIRST_RESERVED ||
        root_dir > PFS_TM_LAST_RESERVED ||
        read_sector_block(fd, root_dir,
                          directory, sizeof(directory)) != 0 ||
        infiltratr_load_be16(directory) != UINT16_C(0x4442))
        goto done;

    size_t entry_offset = 20U;
    for (uint32_t file = 0U; file < profile->files; ++file) {
        if (entry_offset + 40U > sizeof(directory))
            goto done;
        const uint8_t *entry = directory + entry_offset;
        const uint64_t expected_bytes =
            ldtm_profile_file_bytes(profile, file);
        if (entry[0] < 35U || entry[1] != UINT8_C(0xfd) ||
            expected_bytes == 0U || expected_bytes > UINT32_MAX ||
            infiltratr_load_be32(entry + 6U) != (uint32_t)expected_bytes)
            goto done;

        uint32_t anode = infiltratr_load_be32(entry + 2U);
        if (anode == 0U)
            goto done;
        uint64_t logical_sectors = 0U;
        uint32_t chain_count = 0U;
        uint32_t previous_end = 0U;
        int physically_fragmented = 0;
        while (anode != 0U) {
            uint32_t start = 0U;
            if (++chain_count > PFS_TM_FRAGMENTS ||
                read_anode(fd, anode_index, anode,
                           &clusters, &start, &next) != 0 ||
                clusters == 0U || start >= total_sectors ||
                clusters > total_sectors - start)
                goto done;
            if (chain_count > 1U && start != previous_end)
                physically_fragmented = 1;
            previous_end = start + clusters;
            for (uint32_t within = 0U; within < clusters; ++within) {
                if ((logical_sectors + 1U) * PFS_TM_SECTOR_SIZE >
                        expected_bytes ||
                    read_sector_block(fd, start + within,
                                      actual, sizeof(actual)) != 0)
                    goto done;
                make_payload(expected, file, (uint32_t)logical_sectors);
                if (memcmp(actual, expected, sizeof(actual)) != 0)
                    goto done;
                ++logical_sectors;
            }
            anode = next;
        }
        if (logical_sectors * PFS_TM_SECTOR_SIZE != expected_bytes ||
            (expect_fragmented != 0
                ? !physically_fragmented
                : physically_fragmented))
            goto done;
        entry_offset += entry[0];
    }
    result = 0;

done:
    (void)close(fd);
    return result;
}

static int verify_pfs3_payload_state(
    const char *path, const LdtmFragmentProfile *profile,
    int expect_fragmented, char *detail, size_t detail_capacity)
{
    Pfs3Analysis analysis;
    PfsTmGeometry geometry;
    char error[256] = {0};
    int result = -1;
    if (detail != NULL && detail_capacity > 0U)
        detail[0] = '\0';
    if (!profile_matches(profile) ||
        geometry_for_path(path, &geometry) != 0 ||
        pfs3_analyse(path, &analysis, NULL, 0U,
                     error, sizeof(error)) != 0 ||
        analysis.regular_files != LDTM_TARGET_FILE_COUNT ||
        analysis.data_blocks != PFS_TM_DATA_SECTORS ||
        analysis.fragmented_files !=
            (expect_fragmented != 0 ? LDTM_TARGET_FILE_COUNT : 0U) ||
        analysis.block_size != PFS_TM_SECTOR_SIZE ||
        analysis.transaction_pending ||
        verify_current_payload(path, profile, expect_fragmented) != 0)
        goto done;

    if (detail != NULL && detail_capacity > 0U) {
        (void)snprintf(
            detail, detail_capacity,
            expect_fragmented != 0
                ? "native PFS3 payload verified on %u MiB media: 8 differently sized fragmented files, 200 MiB total"
                : "native PFS3 post-defrag payload verified byte-for-byte; production analyser reports zero fragmented files",
            (unsigned)((uint64_t)geometry.total_sectors *
                       PFS_TM_SECTOR_SIZE / LDTM_MIB));
    }
    result = 0;

done:
    if (result != 0 && detail != NULL && detail_capacity > 0U &&
        detail[0] == '\0')
        (void)snprintf(
            detail, detail_capacity, "%s",
            error[0] != '\0'
                ? error : "native PFS3 payload validation failed");
    return result;
}

int ldtm_verify_pfs3_payload(
    const char *path, const LdtmFragmentProfile *profile,
    char *detail, size_t detail_capacity)
{
    return verify_pfs3_payload_state(
        path, profile, 1, detail, detail_capacity);
}

int ldtm_verify_pfs3_payload_after_defrag(
    const char *path, const LdtmFragmentProfile *profile,
    char *detail, size_t detail_capacity)
{
    return verify_pfs3_payload_state(
        path, profile, 0, detail, detail_capacity);
}
