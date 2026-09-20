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
#define PFS_TM_FIRST_RESERVED 2U
#define PFS_TM_LAST_RESERVED 2049U
#define PFS_TM_RESCLUSTER 2U
#define PFS_TM_EXTENSION 4U
#define PFS_TM_BITMAP_INDEX0 6U
#define PFS_TM_BITMAP_INDEX1 8U
#define PFS_TM_ANODE_INDEX 10U
#define PFS_TM_ANODE_BLOCK0 12U
#define PFS_TM_ANODE_BLOCK1 14U
#define PFS_TM_ROOT_DIR 16U
#define PFS_TM_BITMAP_FIRST 18U
#define PFS_TM_ROOT_ANODE 5U
#define PFS_TM_FILE_ANODE 6U
#define PFS_TM_FRAGMENTS 100U
#define PFS_TM_CHUNK_SECTORS 512U
#define PFS_TM_CHUNK_KIB 256U
#define PFS_TM_DATA_START 4096U
#define PFS_TM_DATA_STRIDE 768U
#define PFS_TM_DATA_SECTORS (PFS_TM_FRAGMENTS * PFS_TM_CHUNK_SECTORS)
#define PFS_TM_FILE_BYTES ((uint64_t)PFS_TM_DATA_SECTORS * PFS_TM_SECTOR_SIZE)
#define PFS_TM_BITMAP_BITS (((PFS_TM_RESBLOCK_SIZE / 4U) - 3U) * 32U)
#define PFS_TM_INDEX_POINTERS ((PFS_TM_RESBLOCK_SIZE - 12U) / 4U)
#define PFS_TM_OPTIONS UINT32_C(0x677)

static uint32_t fragment_start(uint32_t index)
{
    return PFS_TM_DATA_START + index * PFS_TM_DATA_STRIDE;
}

static uint32_t extent_anode(uint32_t index)
{
    const uint32_t first_block_capacity =
        (PFS_TM_RESBLOCK_SIZE - 16U) / 12U - PFS_TM_FILE_ANODE;
    if (index < first_block_capacity)
        return PFS_TM_FILE_ANODE + index;
    return UINT32_C(0x10000) | (index - first_block_capacity);
}

static void bitmap_set(uint8_t *bitmap, uint32_t bit, int is_free)
{
    const uint32_t word = bit / 32U;
    const uint32_t within = bit % 32U;
    uint8_t *slot = bitmap + word * 4U;
    uint32_t value = infiltratr_load_be32(slot);
    const uint32_t mask = UINT32_C(0x80000000) >> within;
    if (is_free != 0) value |= mask;
    else value &= ~mask;
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

static void make_root(uint8_t block[PFS_TM_SECTOR_SIZE],
                      uint32_t total_sectors, uint32_t bitmap_blocks)
{
    const uint32_t reserved_slots =
        (PFS_TM_LAST_RESERVED - PFS_TM_FIRST_RESERVED + 1U) / PFS_TM_RESCLUSTER;
    const uint32_t metadata_slots = 8U + bitmap_blocks;
    const uint32_t normal_blocks = total_sectors - (PFS_TM_LAST_RESERVED + 1U);
    memset(block, 0, PFS_TM_SECTOR_SIZE);
    infiltratr_store_be32(block, UINT32_C(0x50465301));
    infiltratr_store_be32(block + 4U, PFS_TM_OPTIONS);
    infiltratr_store_be32(block + 8U, UINT32_C(0x20260920));
    block[20U] = 7U;
    memcpy(block + 21U, "LD_PFS3", 7U);
    infiltratr_store_be32(block + 52U, PFS_TM_LAST_RESERVED);
    infiltratr_store_be32(block + 56U, PFS_TM_FIRST_RESERVED);
    infiltratr_store_be32(block + 60U, reserved_slots - metadata_slots);
    infiltratr_store_be16(block + 64U, PFS_TM_RESBLOCK_SIZE);
    infiltratr_store_be16(block + 66U, PFS_TM_RESCLUSTER);
    infiltratr_store_be32(block + 68U, normal_blocks - PFS_TM_DATA_SECTORS);
    infiltratr_store_be32(block + 72U, 0U);
    infiltratr_store_be32(block + 76U, 0U);
    infiltratr_store_be32(block + 84U, total_sectors);
    infiltratr_store_be32(block + 88U, PFS_TM_EXTENSION);
    infiltratr_store_be32(block + 96U, PFS_TM_BITMAP_INDEX0);
    if (bitmap_blocks > PFS_TM_INDEX_POINTERS)
        infiltratr_store_be32(block + 100U, PFS_TM_BITMAP_INDEX1);
    infiltratr_store_be32(block + 116U, PFS_TM_ANODE_INDEX);
}

static void make_reserved_bitmap(uint8_t block[PFS_TM_SECTOR_SIZE],
                                 uint32_t bitmap_blocks)
{
    const uint32_t reserved_slots =
        (PFS_TM_LAST_RESERVED - PFS_TM_FIRST_RESERVED + 1U) / PFS_TM_RESCLUSTER;
    const uint32_t metadata_slots = 8U + bitmap_blocks;
    memset(block, 0, PFS_TM_SECTOR_SIZE);
    infiltratr_store_be16(block, UINT16_C(0x424d));
    infiltratr_store_be32(block + 4U, UINT32_C(0x20260920));
    infiltratr_store_be32(block + 8U, 0U);
    for (uint32_t slot = 0U; slot < reserved_slots; ++slot)
        bitmap_set(block + 12U, slot, 1);
    for (uint32_t slot = 0U; slot < metadata_slots; ++slot)
        bitmap_set(block + 12U, slot, 0);
}

static void make_extension(uint8_t block[PFS_TM_RESBLOCK_SIZE])
{
    memset(block, 0, PFS_TM_RESBLOCK_SIZE);
    infiltratr_store_be16(block, UINT16_C(0x4558));
    infiltratr_store_be32(block + 8U, UINT32_C(0x20260920));
    infiltratr_store_be32(block + 28U, 0U);
}

static void make_bitmap_index(uint8_t block[PFS_TM_RESBLOCK_SIZE],
                              uint32_t group, uint32_t bitmap_blocks)
{
    memset(block, 0, PFS_TM_RESBLOCK_SIZE);
    infiltratr_store_be16(block, UINT16_C(0x4d49));
    infiltratr_store_be32(block + 4U, UINT32_C(0x20260920));
    infiltratr_store_be32(block + 8U, group);
    const uint32_t first = group * PFS_TM_INDEX_POINTERS;
    const uint32_t remaining = bitmap_blocks > first ? bitmap_blocks - first : 0U;
    const uint32_t count =
        remaining > PFS_TM_INDEX_POINTERS ? PFS_TM_INDEX_POINTERS : remaining;
    for (uint32_t slot = 0U; slot < count; ++slot) {
        const uint32_t sequence = first + slot;
        infiltratr_store_be32(
            block + 12U + slot * 4U,
            PFS_TM_BITMAP_FIRST + sequence * PFS_TM_RESCLUSTER);
    }
}

static void make_allocation_bitmap(uint8_t block[PFS_TM_RESBLOCK_SIZE],
                                   uint32_t sequence, uint32_t total_sectors)
{
    const uint32_t first_data = PFS_TM_LAST_RESERVED + 1U;
    const uint32_t base = first_data + sequence * PFS_TM_BITMAP_BITS;
    memset(block, 0, PFS_TM_RESBLOCK_SIZE);
    infiltratr_store_be16(block, UINT16_C(0x424d));
    infiltratr_store_be32(block + 4U, UINT32_C(0x20260920));
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
    if (base < total_sectors && base + PFS_TM_BITMAP_BITS > total_sectors) {
        for (uint32_t sector = total_sectors;
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

static void make_anode_index(uint8_t block[PFS_TM_RESBLOCK_SIZE])
{
    memset(block, 0, PFS_TM_RESBLOCK_SIZE);
    infiltratr_store_be16(block, UINT16_C(0x4942));
    infiltratr_store_be32(block + 4U, UINT32_C(0x20260920));
    infiltratr_store_be32(block + 8U, 0U);
    infiltratr_store_be32(block + 12U, PFS_TM_ANODE_BLOCK0);
    infiltratr_store_be32(block + 16U, PFS_TM_ANODE_BLOCK1);
}

static void make_anode_block(uint8_t block[PFS_TM_RESBLOCK_SIZE], uint32_t sequence)
{
    const uint32_t per_block = (PFS_TM_RESBLOCK_SIZE - 16U) / 12U;
    memset(block, 0, PFS_TM_RESBLOCK_SIZE);
    infiltratr_store_be16(block, UINT16_C(0x4142));
    infiltratr_store_be32(block + 4U, UINT32_C(0x20260920));
    infiltratr_store_be32(block + 8U, sequence);
    if (sequence == 0U)
        put_anode(block, PFS_TM_ROOT_ANODE, 1U, PFS_TM_ROOT_DIR, 0U);
    for (uint32_t index = 0U; index < PFS_TM_FRAGMENTS; ++index) {
        const uint32_t number = extent_anode(index);
        const uint32_t seq = number >> 16U;
        const uint32_t offset = number & UINT32_C(0xffff);
        if (seq != sequence || offset >= per_block)
            continue;
        const uint32_t next =
            index + 1U < PFS_TM_FRAGMENTS ? extent_anode(index + 1U) : 0U;
        put_anode(block, offset, PFS_TM_CHUNK_SECTORS,
                  fragment_start(index), next);
    }
}

static void make_root_directory(uint8_t block[PFS_TM_RESBLOCK_SIZE])
{
    memset(block, 0, PFS_TM_RESBLOCK_SIZE);
    infiltratr_store_be16(block, UINT16_C(0x4442));
    infiltratr_store_be32(block + 4U, UINT32_C(0x20260920));
    infiltratr_store_be32(block + 12U, PFS_TM_ROOT_ANODE);
    infiltratr_store_be32(block + 16U, 0U);
    uint8_t *entry = block + 20U;
    entry[0] = 40U;
    entry[1] = UINT8_C(0xfd);
    infiltratr_store_be32(entry + 2U, PFS_TM_FILE_ANODE);
    infiltratr_store_be32(entry + 6U, (uint32_t)PFS_TM_FILE_BYTES);
    entry[17U] = 17U;
    memcpy(entry + 18U, "fragmented-00.bin", 17U);
    entry[35U] = 0U;
}

static void make_payload(uint8_t sector[PFS_TM_SECTOR_SIZE],
                         uint32_t fragment, uint32_t within)
{
    uint64_t state = UINT64_C(0x243f6a8885a308d3) ^
                     ((uint64_t)fragment << 32U) ^
                     ((uint64_t)within * UINT64_C(0x9e3779b97f4a7c15));
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
    if (path == NULL) return -1;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    const off_t bytes = lseek(fd, 0, SEEK_END);
    if (bytes <= 0 || (uint64_t)bytes / PFS_TM_SECTOR_SIZE > UINT32_MAX) {
        (void)close(fd);
        return -1;
    }
    const uint32_t total_sectors = (uint32_t)((uint64_t)bytes / PFS_TM_SECTOR_SIZE);
    const uint32_t first_data = PFS_TM_LAST_RESERVED + 1U;
    if (total_sectors <= fragment_start(PFS_TM_FRAGMENTS - 1U) + PFS_TM_CHUNK_SECTORS ||
        total_sectors <= first_data) {
        (void)close(fd);
        return -1;
    }
    const uint32_t normal_blocks = total_sectors - first_data;
    const uint32_t bitmap_blocks =
        (normal_blocks + PFS_TM_BITMAP_BITS - 1U) / PFS_TM_BITMAP_BITS;
    if (bitmap_blocks == 0U || bitmap_blocks > 2U * PFS_TM_INDEX_POINTERS ||
        8U + bitmap_blocks >
            (PFS_TM_LAST_RESERVED - PFS_TM_FIRST_RESERVED + 1U) / PFS_TM_RESCLUSTER) {
        (void)close(fd);
        return -1;
    }

    uint8_t *block = malloc(PFS_TM_RESBLOCK_SIZE);
    if (block == NULL) {
        (void)close(fd);
        return -1;
    }
    int result = -1;

    make_root(block, total_sectors, bitmap_blocks);
    if (write_sector_block(fd, 2U, block, PFS_TM_SECTOR_SIZE) != 0) goto done;
    make_reserved_bitmap(block, bitmap_blocks);
    if (write_sector_block(fd, 3U, block, PFS_TM_SECTOR_SIZE) != 0) goto done;
    make_extension(block);
    if (write_sector_block(fd, PFS_TM_EXTENSION, block, PFS_TM_RESBLOCK_SIZE) != 0) goto done;
    make_bitmap_index(block, 0U, bitmap_blocks);
    if (write_sector_block(fd, PFS_TM_BITMAP_INDEX0, block, PFS_TM_RESBLOCK_SIZE) != 0) goto done;
    if (bitmap_blocks > PFS_TM_INDEX_POINTERS) {
        make_bitmap_index(block, 1U, bitmap_blocks);
        if (write_sector_block(fd, PFS_TM_BITMAP_INDEX1, block, PFS_TM_RESBLOCK_SIZE) != 0) goto done;
    }
    make_anode_index(block);
    if (write_sector_block(fd, PFS_TM_ANODE_INDEX, block, PFS_TM_RESBLOCK_SIZE) != 0) goto done;
    make_anode_block(block, 0U);
    if (write_sector_block(fd, PFS_TM_ANODE_BLOCK0, block, PFS_TM_RESBLOCK_SIZE) != 0) goto done;
    make_anode_block(block, 1U);
    if (write_sector_block(fd, PFS_TM_ANODE_BLOCK1, block, PFS_TM_RESBLOCK_SIZE) != 0) goto done;
    make_root_directory(block);
    if (write_sector_block(fd, PFS_TM_ROOT_DIR, block, PFS_TM_RESBLOCK_SIZE) != 0) goto done;

    for (uint32_t sequence = 0U; sequence < bitmap_blocks; ++sequence) {
        make_allocation_bitmap(block, sequence, total_sectors);
        if (write_sector_block(
                fd, PFS_TM_BITMAP_FIRST + sequence * PFS_TM_RESCLUSTER,
                block, PFS_TM_RESBLOCK_SIZE) != 0) goto done;
    }

    for (uint32_t fragment = 0U; fragment < PFS_TM_FRAGMENTS; ++fragment) {
        const uint32_t start = fragment_start(fragment);
        for (uint32_t within = 0U; within < PFS_TM_CHUNK_SECTORS; ++within) {
            make_payload(block, fragment, within);
            if (write_sector_block(fd, start + within,
                                   block, PFS_TM_SECTOR_SIZE) != 0) goto done;
        }
    }
    if (fsync(fd) != 0) goto done;
    result = 0;

done:
    free(block);
    (void)close(fd);
    return result;
}

static int profile_matches(const LdtmFragmentProfile *profile)
{
    return profile != NULL && profile->files == 1U &&
           profile->chunks == PFS_TM_FRAGMENTS &&
           profile->chunk_kib == PFS_TM_CHUNK_KIB &&
           profile->directory_initial == 0U && profile->directory_second == 0U;
}

int ldtm_populate_pfs3_volume(const char *path, const LdtmFragmentProfile *profile)
{
    Pfs3Analysis analysis;
    char error[256] = {0};
    if (!profile_matches(profile)) return -1;
    return pfs3_analyse(path, &analysis, NULL, 0U, error, sizeof(error)) == 0 &&
           analysis.regular_files == 1U &&
           analysis.data_blocks == PFS_TM_DATA_SECTORS &&
           analysis.fragmented_files == 1U &&
           !analysis.growth_10_satisfied ? 0 : -1;
}

int ldtm_verify_pfs3_payload(const char *path, const LdtmFragmentProfile *profile,
                             char *detail, size_t detail_capacity)
{
    Pfs3Analysis analysis;
    char error[256] = {0};
    uint8_t actual[PFS_TM_SECTOR_SIZE];
    uint8_t expected[PFS_TM_SECTOR_SIZE];
    int fd = -1;
    int result = -1;
    if (detail != NULL && detail_capacity > 0U) detail[0] = '\0';
    if (!profile_matches(profile) ||
        pfs3_analyse(path, &analysis, NULL, 0U, error, sizeof(error)) != 0 ||
        analysis.regular_files != 1U ||
        analysis.data_blocks != PFS_TM_DATA_SECTORS ||
        analysis.fragmented_files != 1U ||
        analysis.growth_10_satisfied ||
        analysis.block_size != PFS_TM_SECTOR_SIZE ||
        analysis.transaction_pending)
        goto done;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) goto done;
    for (uint32_t fragment = 0U; fragment < PFS_TM_FRAGMENTS; ++fragment) {
        const uint32_t start = fragment_start(fragment);
        for (uint32_t within = 0U; within < PFS_TM_CHUNK_SECTORS; ++within) {
            if (infiltratr_pread_full(
                    fd, actual, sizeof(actual),
                    (uint64_t)(start + within) * PFS_TM_SECTOR_SIZE) != 0)
                goto done;
            make_payload(expected, fragment, within);
            if (memcmp(actual, expected, sizeof(actual)) != 0)
                goto done;
        }
    }
    if (detail != NULL && detail_capacity > 0U)
        (void)snprintf(detail, detail_capacity,
                       "native PFS3 payload verified: 1 x 25 MiB file, 100 fragments, Growth Defrag reserve intentionally unsatisfied");
    result = 0;
done:
    if (fd >= 0) (void)close(fd);
    if (result != 0 && detail != NULL && detail_capacity > 0U && detail[0] == '\0')
        (void)snprintf(detail, detail_capacity, "%s",
                       error[0] != '\0' ? error : "native PFS3 payload validation failed");
    return result;
}
