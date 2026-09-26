// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Defragmenter - classic Macintosh HFS read-only analyser
 * Author: Shannon Smith
 *
 * This first-party analyser reads the HFS Master Directory Block, Extents
 * Overflow B-tree and Catalog B-tree directly from an image or block device.
 * It never mounts the filesystem, invokes a filesystem utility or writes to
 * the target. Unsupported or structurally inconsistent layouts fail closed.
 */

#define _FILE_OFFSET_BITS 64

#include "ld_io.h"
#include "infiltratr/endian.h"
#include "infiltratr/core.h"
#include "infiltratr/arithmetic.h"
#include "infiltratr/posix_io.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HFS_LOGICAL_BLOCK_SIZE 512U
#define HFS_MDB_OFFSET 1024U
#define HFS_MDB_SIZE 162U
#define HFS_SIGNATURE 0x4244U
#define HFS_BTREE_HEADER_NODE 1
#define HFS_BTREE_LEAF_NODE (-1)
#define HFS_FILE_RECORD 2U
#define HFS_DIRECTORY_RECORD 1U
#define HFS_DATA_FORK 0x00U
#define HFS_RESOURCE_FORK 0xffU
#define HFS_EXTENTS_FILE_ID 3U
#define HFS_CATALOG_FILE_ID 4U
#define HFS_MAX_EXTENTS 4096U
#define HFS_MAX_OVERFLOW_RECORDS 262144U
#define HFS_MAX_BTREE_RECORDS 120U

typedef struct {
    uint16_t start;
    uint16_t count;
} hfs_extent;

typedef struct {
    uint32_t file_id;
    uint8_t fork_type;
    uint16_t first_file_block;
    hfs_extent extents[3];
} hfs_overflow_record;

typedef struct {
    uint64_t size_bytes;
    hfs_extent extents[HFS_MAX_EXTENTS];
    size_t extent_count;
} hfs_fork_map;

typedef struct {
    int fd;
    uint32_t allocation_block_size;
    uint16_t allocation_start_block;
    uint16_t total_allocation_blocks;
    uint16_t bitmap_start_block;
    uint16_t header_free_blocks;
    uint32_t header_files;
    uint32_t header_directories;
    hfs_fork_map extents_file;
    hfs_fork_map catalog_file;
    hfs_overflow_record *overflow;
    size_t overflow_count;
    size_t overflow_capacity;
} hfs_volume;

typedef struct {
    uint32_t first_leaf;
    uint32_t last_leaf;
    uint32_t total_nodes;
} hfs_btree_header;

typedef struct {
    uint32_t files;
    uint32_t directories;
    uint32_t fragmented_files;
    hfs_extent *fragmented_extents;
    size_t fragmented_extent_count;
    size_t fragmented_extent_capacity;
} hfs_scan_result;

static int read_exact(int fd, uint64_t offset, void *buffer, size_t length)
{
    return infiltratr_pread_full(fd, buffer, length, offset);
}

static uint64_t extent_capacity_bytes(const hfs_volume *volume,
                                      const hfs_extent *extent)
{
    return (uint64_t)extent->count * (uint64_t)volume->allocation_block_size;
}

static uint64_t extent_physical_offset(const hfs_volume *volume,
                                       const hfs_extent *extent)
{
    return (uint64_t)volume->allocation_start_block * HFS_LOGICAL_BLOCK_SIZE +
           (uint64_t)extent->start * (uint64_t)volume->allocation_block_size;
}

static uint64_t fork_mapped_bytes(const hfs_volume *volume,
                                  const hfs_fork_map *fork)
{
    uint64_t total = 0U;
    size_t i;
    for (i = 0U; i < fork->extent_count; ++i)
        total += extent_capacity_bytes(volume, &fork->extents[i]);
    return total;
}

static int fork_append_extent(hfs_fork_map *fork, uint16_t start, uint16_t count)
{
    if (!count)
        return 0;
    if (fork->extent_count >= HFS_MAX_EXTENTS)
        return -1;
    fork->extents[fork->extent_count].start = start;
    fork->extents[fork->extent_count].count = count;
    ++fork->extent_count;
    return 0;
}

static int read_fork(const hfs_volume *volume, const hfs_fork_map *fork,
                     uint64_t offset, void *buffer, size_t length)
{
    uint8_t *out = (uint8_t *)buffer;
    size_t done = 0U;
    uint64_t logical = 0U;
    size_t i;

    if (offset > fork->size_bytes || (uint64_t)length > fork->size_bytes - offset)
        return -1;

    for (i = 0U; i < fork->extent_count && done < length; ++i) {
        uint64_t span = extent_capacity_bytes(volume, &fork->extents[i]);
        uint64_t extent_end = logical + span;
        uint64_t within;
        uint64_t available;
        size_t take;

        if (offset >= extent_end) {
            logical = extent_end;
            continue;
        }
        if (offset + (uint64_t)done < logical) {
            logical = extent_end;
            continue;
        }

        within = offset + (uint64_t)done - logical;
        available = span - within;
        take = length - done;
        if ((uint64_t)take > available)
            take = (size_t)available;
        if (read_exact(volume->fd,
                       extent_physical_offset(volume, &fork->extents[i]) + within,
                       out + done, take) != 0)
            return -1;
        done += take;
        logical = extent_end;
    }
    return done == length ? 0 : -1;
}

static int read_btree_node(const hfs_volume *volume, const hfs_fork_map *fork,
                           uint32_t node_number, uint8_t node[HFS_LOGICAL_BLOCK_SIZE])
{
    uint64_t offset = (uint64_t)node_number * HFS_LOGICAL_BLOCK_SIZE;
    return read_fork(volume, fork, offset, node, HFS_LOGICAL_BLOCK_SIZE);
}

static int node_record_bounds(const uint8_t node[HFS_LOGICAL_BLOCK_SIZE],
                              uint16_t record_count, uint16_t record_index,
                              uint16_t *start, uint16_t *end)
{
    size_t start_slot;
    size_t end_slot;
    uint16_t a;
    uint16_t b;

    if (record_index >= record_count || record_count > HFS_MAX_BTREE_RECORDS)
        return -1;
    start_slot = HFS_LOGICAL_BLOCK_SIZE - 2U * ((size_t)record_index + 1U);
    end_slot = HFS_LOGICAL_BLOCK_SIZE - 2U * ((size_t)record_index + 2U);
    a = infiltratr_load_be16(node + start_slot);
    b = infiltratr_load_be16(node + end_slot);
    if (a < 14U || b <= a || b > end_slot)
        return -1;
    *start = a;
    *end = b;
    return 0;
}

static int parse_btree_header(const hfs_volume *volume, const hfs_fork_map *fork,
                              hfs_btree_header *header)
{
    uint8_t node[HFS_LOGICAL_BLOCK_SIZE];
    uint16_t record_count;
    uint16_t start;
    uint16_t end;

    if (read_btree_node(volume, fork, 0U, node) != 0)
        return -1;
    if ((int8_t)node[8] != HFS_BTREE_HEADER_NODE)
        return -1;
    record_count = infiltratr_load_be16(node + 10U);
    if (record_count < 1U || node_record_bounds(node, record_count, 0U, &start, &end) != 0)
        return -1;
    if ((size_t)(end - start) < 30U)
        return -1;
    if (infiltratr_load_be16(node + start + 18U) != HFS_LOGICAL_BLOCK_SIZE)
        return -1;

    header->first_leaf = infiltratr_load_be32(node + start + 10U);
    header->last_leaf = infiltratr_load_be32(node + start + 14U);
    header->total_nodes = infiltratr_load_be32(node + start + 22U);
    if (!header->total_nodes ||
        header->first_leaf >= header->total_nodes ||
        header->last_leaf >= header->total_nodes)
        return -1;
    return 0;
}

static size_t record_key_skip(const uint8_t *record, size_t record_length)
{
    size_t skip;
    if (!record_length)
        return 0U;
    skip = ((size_t)record[0] + 2U) & ~(size_t)1U;
    return skip <= record_length ? skip : 0U;
}

static int overflow_append(hfs_volume *volume, const hfs_overflow_record *record)
{
    if (volume->overflow_count >= HFS_MAX_OVERFLOW_RECORDS)
        return -1;
    if (!infiltratr_array_reserve((void **)&volume->overflow,
                                  &volume->overflow_capacity,
                                  sizeof(*volume->overflow),
                                  volume->overflow_count + 1U, 256U))
        return -1;
    volume->overflow[volume->overflow_count++] = *record;
    return 0;
}

static int scan_extents_overflow(hfs_volume *volume)
{
    hfs_btree_header header;
    uint32_t node_number;
    uint32_t visited = 0U;

    if (fork_mapped_bytes(volume, &volume->extents_file) < volume->extents_file.size_bytes) {
        fprintf(stderr, "hfs-analyser: extents-overflow file itself exceeds its MDB extents\n");
        return -1;
    }
    if (parse_btree_header(volume, &volume->extents_file, &header) != 0)
        return -1;

    node_number = header.first_leaf;
    while (node_number) {
        uint8_t node[HFS_LOGICAL_BLOCK_SIZE];
        uint16_t record_count;
        uint16_t i;
        uint32_t next;

        if (++visited > header.total_nodes ||
            read_btree_node(volume, &volume->extents_file, node_number, node) != 0)
            return -1;
        if ((int8_t)node[8] != HFS_BTREE_LEAF_NODE)
            return -1;
        next = infiltratr_load_be32(node + 0U);
        record_count = infiltratr_load_be16(node + 10U);
        if (record_count > HFS_MAX_BTREE_RECORDS)
            return -1;

        for (i = 0U; i < record_count; ++i) {
            uint16_t start;
            uint16_t end;
            size_t length;
            size_t key_skip;
            const uint8_t *record;
            const uint8_t *data;
            hfs_overflow_record item;
            size_t j;

            if (node_record_bounds(node, record_count, i, &start, &end) != 0)
                return -1;
            record = node + start;
            length = (size_t)(end - start);
            key_skip = record_key_skip(record, length);
            if (key_skip < 8U || key_skip + 12U > length)
                return -1;
            memset(&item, 0, sizeof(item));
            item.fork_type = record[1];
            item.file_id = infiltratr_load_be32(record + 2U);
            item.first_file_block = infiltratr_load_be16(record + 6U);
            data = record + key_skip;
            for (j = 0U; j < 3U; ++j) {
                item.extents[j].start = infiltratr_load_be16(data + j * 4U);
                item.extents[j].count = infiltratr_load_be16(data + j * 4U + 2U);
            }
            if (overflow_append(volume, &item) != 0)
                return -1;
        }
        if (node_number == header.last_leaf && next != 0U)
            return -1;
        node_number = next;
    }
    return 0;
}

static const hfs_overflow_record *find_overflow(const hfs_volume *volume,
                                                 uint32_t file_id,
                                                 uint8_t fork_type,
                                                 uint16_t first_file_block)
{
    size_t i;
    for (i = 0U; i < volume->overflow_count; ++i) {
        const hfs_overflow_record *item = &volume->overflow[i];
        if (item->file_id == file_id && item->fork_type == fork_type &&
            item->first_file_block == first_file_block)
            return item;
    }
    return NULL;
}

static int extend_special_file(hfs_volume *volume, hfs_fork_map *fork,
                               uint32_t file_id, uint8_t fork_type)
{
    uint64_t mapped = fork_mapped_bytes(volume, fork);
    uint64_t block_count;

    if (mapped >= fork->size_bytes)
        return 0;
    if (fork->size_bytes % volume->allocation_block_size != 0U)
        return -1;
    block_count = mapped / volume->allocation_block_size;
    while (mapped < fork->size_bytes) {
        const hfs_overflow_record *item;
        size_t i;
        if (block_count > UINT16_MAX)
            return -1;
        item = find_overflow(volume, file_id, fork_type, (uint16_t)block_count);
        if (!item)
            return -1;
        for (i = 0U; i < 3U && mapped < fork->size_bytes; ++i) {
            uint16_t count = item->extents[i].count;
            if (!count)
                break;
            if (fork_append_extent(fork, item->extents[i].start, count) != 0)
                return -1;
            mapped += (uint64_t)count * volume->allocation_block_size;
            block_count += count;
        }
    }
    return mapped >= fork->size_bytes ? 0 : -1;
}

static int result_append_extent(hfs_scan_result *result, hfs_extent extent)
{
    if (!infiltratr_array_reserve((void **)&result->fragmented_extents,
                                  &result->fragmented_extent_capacity,
                                  sizeof(*result->fragmented_extents),
                                  result->fragmented_extent_count + 1U, 128U))
        return -1;
    result->fragmented_extents[result->fragmented_extent_count++] = extent;
    return 0;
}

static int collect_file_fork(const hfs_volume *volume, uint32_t file_id,
                             uint8_t fork_type, uint32_t physical_bytes,
                             const uint8_t *inline_data,
                             hfs_extent extents[HFS_MAX_EXTENTS],
                             size_t *extent_count, int *fragmented)
{
    uint64_t required_blocks;
    uint64_t collected = 0U;
    size_t count = 0U;
    size_t i;

    *extent_count = 0U;
    *fragmented = 0;
    if (!physical_bytes)
        return 0;
    if (physical_bytes % volume->allocation_block_size != 0U)
        return -1;
    required_blocks = physical_bytes / volume->allocation_block_size;

    for (i = 0U; i < 3U && collected < required_blocks; ++i) {
        hfs_extent extent;
        extent.start = infiltratr_load_be16(inline_data + i * 4U);
        extent.count = infiltratr_load_be16(inline_data + i * 4U + 2U);
        if (!extent.count)
            return -1;
        if (collected + extent.count > required_blocks || count >= HFS_MAX_EXTENTS)
            return -1;
        extents[count++] = extent;
        collected += extent.count;
    }

    while (collected < required_blocks) {
        const hfs_overflow_record *item;
        if (collected > UINT16_MAX)
            return -1;
        item = find_overflow(volume, file_id, fork_type, (uint16_t)collected);
        if (!item)
            return -1;
        for (i = 0U; i < 3U && collected < required_blocks; ++i) {
            hfs_extent extent = item->extents[i];
            if (!extent.count)
                return -1;
            if (collected + extent.count > required_blocks || count >= HFS_MAX_EXTENTS)
                return -1;
            extents[count++] = extent;
            collected += extent.count;
        }
    }

    for (i = 1U; i < count; ++i) {
        uint32_t previous_end = (uint32_t)extents[i - 1U].start + extents[i - 1U].count;
        if (previous_end != extents[i].start) {
            *fragmented = 1;
            break;
        }
    }
    *extent_count = count;
    return 0;
}

static int scan_catalog(hfs_volume *volume, hfs_scan_result *result)
{
    hfs_btree_header header;
    uint32_t node_number;
    uint32_t visited = 0U;

    if (extend_special_file(volume, &volume->catalog_file,
                            HFS_CATALOG_FILE_ID, HFS_DATA_FORK) != 0)
        return -1;
    if (parse_btree_header(volume, &volume->catalog_file, &header) != 0)
        return -1;

    node_number = header.first_leaf;
    while (node_number) {
        uint8_t node[HFS_LOGICAL_BLOCK_SIZE];
        uint16_t record_count;
        uint16_t i;
        uint32_t next;

        if (++visited > header.total_nodes ||
            read_btree_node(volume, &volume->catalog_file, node_number, node) != 0)
            return -1;
        if ((int8_t)node[8] != HFS_BTREE_LEAF_NODE)
            return -1;
        next = infiltratr_load_be32(node + 0U);
        record_count = infiltratr_load_be16(node + 10U);
        if (record_count > HFS_MAX_BTREE_RECORDS)
            return -1;

        for (i = 0U; i < record_count; ++i) {
            uint16_t start;
            uint16_t end;
            size_t length;
            size_t key_skip;
            const uint8_t *record;
            const uint8_t *data;
            uint8_t type;

            if (node_record_bounds(node, record_count, i, &start, &end) != 0)
                return -1;
            record = node + start;
            length = (size_t)(end - start);
            key_skip = record_key_skip(record, length);
            if (key_skip == 0U || key_skip + 2U > length)
                return -1;
            data = record + key_skip;
            type = data[0];
            if (type == HFS_DIRECTORY_RECORD) {
                ++result->directories;
            } else if (type == HFS_FILE_RECORD) {
                uint32_t file_id;
                uint32_t data_physical;
                uint32_t resource_physical;
                hfs_extent data_extents[HFS_MAX_EXTENTS];
                hfs_extent resource_extents[HFS_MAX_EXTENTS];
                size_t data_count = 0U;
                size_t resource_count = 0U;
                int data_fragmented = 0;
                int resource_fragmented = 0;
                size_t j;

                if (key_skip + 102U > length)
                    return -1;
                ++result->files;
                file_id = infiltratr_load_be32(data + 20U);
                data_physical = infiltratr_load_be32(data + 30U);
                resource_physical = infiltratr_load_be32(data + 40U);
                if (collect_file_fork(volume, file_id, HFS_DATA_FORK,
                                      data_physical, data + 74U,
                                      data_extents, &data_count,
                                      &data_fragmented) != 0 ||
                    collect_file_fork(volume, file_id, HFS_RESOURCE_FORK,
                                      resource_physical, data + 86U,
                                      resource_extents, &resource_count,
                                      &resource_fragmented) != 0)
                    return -1;
                if (data_fragmented || resource_fragmented)
                    ++result->fragmented_files;
                if (data_fragmented) {
                    for (j = 0U; j < data_count; ++j)
                        if (result_append_extent(result, data_extents[j]) != 0)
                            return -1;
                }
                if (resource_fragmented) {
                    for (j = 0U; j < resource_count; ++j)
                        if (result_append_extent(result, resource_extents[j]) != 0)
                            return -1;
                }
            }
        }
        if (node_number == header.last_leaf && next != 0U)
            return -1;
        node_number = next;
    }
    return 0;
}

static int parse_mdb(hfs_volume *volume)
{
    uint8_t mdb[HFS_MDB_SIZE];
    size_t i;

    if (read_exact(volume->fd, HFS_MDB_OFFSET, mdb, sizeof(mdb)) != 0)
        return -1;
    if (infiltratr_load_be16(mdb) != HFS_SIGNATURE)
        return -1;

    volume->bitmap_start_block = infiltratr_load_be16(mdb + 14U);
    volume->total_allocation_blocks = infiltratr_load_be16(mdb + 18U);
    volume->allocation_block_size = infiltratr_load_be32(mdb + 20U);
    volume->allocation_start_block = infiltratr_load_be16(mdb + 28U);
    volume->header_free_blocks = infiltratr_load_be16(mdb + 34U);
    volume->header_files = infiltratr_load_be32(mdb + 84U);
    volume->header_directories = infiltratr_load_be32(mdb + 88U);
    if (!volume->total_allocation_blocks ||
        volume->allocation_block_size < HFS_LOGICAL_BLOCK_SIZE ||
        volume->allocation_block_size % HFS_LOGICAL_BLOCK_SIZE != 0U)
        return -1;

    volume->extents_file.size_bytes = infiltratr_load_be32(mdb + 130U);
    volume->catalog_file.size_bytes = infiltratr_load_be32(mdb + 146U);
    for (i = 0U; i < 3U; ++i) {
        uint16_t start = infiltratr_load_be16(mdb + 134U + i * 4U);
        uint16_t count = infiltratr_load_be16(mdb + 136U + i * 4U);
        if (count && fork_append_extent(&volume->extents_file, start, count) != 0)
            return -1;
        start = infiltratr_load_be16(mdb + 150U + i * 4U);
        count = infiltratr_load_be16(mdb + 152U + i * 4U);
        if (count && fork_append_extent(&volume->catalog_file, start, count) != 0)
            return -1;
    }
    /* The allocation bitmap is independently useful even when the deeper
     * Extents/Catalog B-trees are damaged or unsupported.  scan-json remains
     * strict; the exact map command deliberately preserves the old GUI's
     * allocation-map fallback behaviour. */
    return 0;
}

static int scan_json(const char *device)
{
    hfs_volume volume;
    hfs_scan_result result;
    size_t i;
    int rc = 1;

    memset(&volume, 0, sizeof(volume));
    memset(&result, 0, sizeof(result));
    volume.fd = open(device, O_RDONLY | O_CLOEXEC);
    if (volume.fd < 0) {
        fprintf(stderr, "hfs-analyser: open %s: %s\n", device, strerror(errno));
        return 1;
    }

    if (parse_mdb(&volume) != 0) {
        fprintf(stderr, "hfs-analyser: invalid or unsupported classic HFS MDB\n");
        goto done;
    }
    if (scan_extents_overflow(&volume) != 0) {
        fprintf(stderr, "hfs-analyser: invalid or unsupported HFS Extents Overflow B-tree\n");
        goto done;
    }
    if (scan_catalog(&volume, &result) != 0) {
        fprintf(stderr, "hfs-analyser: invalid or unsupported HFS Catalog B-tree\n");
        goto done;
    }

    printf("{\"files\":%" PRIu32 ",\"directories\":%" PRIu32
           ",\"fragmented_files\":%" PRIu32
           ",\"fragmented_directories\":0,\"fragmented_extents\":[",
           result.files, result.directories, result.fragmented_files);
    for (i = 0U; i < result.fragmented_extent_count; ++i) {
        if (i)
            putchar(',');
        printf("[%u,%u]", (unsigned int)result.fragmented_extents[i].start,
               (unsigned int)result.fragmented_extents[i].count);
    }
    printf("]}\n");
    rc = 0;

done:
    free(result.fragmented_extents);
    free(volume.overflow);
    close(volume.fd);
    return rc;
}

static int hfs_open_volume(const char *device, hfs_volume *volume)
{
    memset(volume, 0, sizeof(*volume));
    volume->fd = open(device, O_RDONLY | O_CLOEXEC);
    if (volume->fd < 0)
        return -1;
    if (parse_mdb(volume) != 0) {
        close(volume->fd);
        volume->fd = -1;
        return -1;
    }
    return 0;
}

static void hfs_close_volume(hfs_volume *volume)
{
    free(volume->overflow);
    volume->overflow = NULL;
    volume->overflow_count = 0U;
    volume->overflow_capacity = 0U;
    if (volume->fd >= 0) {
        close(volume->fd);
        volume->fd = -1;
    }
}

static int allocation_bitmap_read(const hfs_volume *volume,
                                  uint8_t **bitmap_out, size_t *length_out)
{
    size_t length = ((size_t)volume->total_allocation_blocks + 7U) / 8U;
    uint8_t *bitmap;
    uint64_t offset;

    if (!length)
        return -1;
    bitmap = (uint8_t *)malloc(length);
    if (!bitmap)
        return -1;
    offset = (uint64_t)volume->bitmap_start_block * HFS_LOGICAL_BLOCK_SIZE;
    if (read_exact(volume->fd, offset, bitmap, length) != 0) {
        free(bitmap);
        return -1;
    }
    *bitmap_out = bitmap;
    *length_out = length;
    return 0;
}

static int allocation_block_used(const uint8_t *bitmap, uint16_t block)
{
    uint8_t mask = (uint8_t)(0x80U >> (block & 7U));
    return (bitmap[block >> 3U] & mask) != 0U;
}

static uint64_t fragmented_in_range(const uint8_t *fragmented,
                                    uint64_t start, uint64_t end)
{
    uint64_t count = 0U;
    uint64_t block;
    for (block = start; block < end; ++block)
        if (ld_bitmap_get(fragmented, block))
            ++count;
    return count;
}

static int identify_json(const char *device)
{
    hfs_volume volume;
    if (hfs_open_volume(device, &volume) != 0) {
        fprintf(stderr, "hfs-analyser: invalid or unsupported classic HFS MDB\n");
        return 1;
    }
    printf("{\"filesystem\":\"hfs\",\"block_size\":%" PRIu32
           ",\"total_blocks\":%u}\n",
           volume.allocation_block_size,
           (unsigned int)volume.total_allocation_blocks);
    hfs_close_volume(&volume);
    return 0;
}

static int parse_cells(const char *text, uint64_t *cells)
{
    return infiltratr_parse_u64_range(text, 10U, 1U, UINT64_MAX, cells)
        ? 0 : -1;
}

static int map_json(const char *device, uint64_t requested_cells)
{
    hfs_volume volume;
    hfs_scan_result result;
    uint8_t *bitmap = NULL;
    size_t bitmap_length = 0U;
    uint8_t *fragmented = NULL;
    uint64_t total_units;
    uint64_t cell_count;
    uint64_t free_total = 0U;
    uint64_t used_total = 0U;
    int deep_ok = 0;
    size_t i;
    int rc = 1;

    memset(&result, 0, sizeof(result));
    if (hfs_open_volume(device, &volume) != 0) {
        fprintf(stderr, "hfs-analyser: invalid or unsupported classic HFS MDB\n");
        return 1;
    }
    if (allocation_bitmap_read(&volume, &bitmap, &bitmap_length) != 0) {
        fprintf(stderr, "hfs-analyser: cannot read HFS allocation bitmap\n");
        goto done;
    }
    (void)bitmap_length;

    total_units = volume.total_allocation_blocks;
    cell_count = requested_cells < total_units ? requested_cells : total_units;
    if (!cell_count)
        cell_count = 1U;

    if (scan_extents_overflow(&volume) == 0 &&
        scan_catalog(&volume, &result) == 0) {
        deep_ok = 1;
    } else {
        free(result.fragmented_extents);
        memset(&result, 0, sizeof(result));
        result.files = volume.header_files;
        result.directories = volume.header_directories;
    }

    fragmented = ld_bitmap_calloc(total_units);
    if (!fragmented) {
        fprintf(stderr, "hfs-analyser: out of memory\n");
        goto done;
    }
    if (deep_ok) {
        for (i = 0U; i < result.fragmented_extent_count; ++i) {
            uint64_t start = result.fragmented_extents[i].start;
            uint64_t end = start + result.fragmented_extents[i].count;
            uint64_t block;
            if (end > total_units)
                end = total_units;
            for (block = start; block < end; ++block)
                ld_bitmap_set(fragmented, block, true);
        }
    }

    printf("{\"schema\":1,\"backend\":\"read-only-domain\","
           "\"filesystem\":\"hfs\",\"map_accuracy\":\"exact\","
           "\"unit_size\":%" PRIu32 ",\"total_units\":%" PRIu64
           ",\"cell_count\":%" PRIu64 ",\"cells\":[",
           volume.allocation_block_size, total_units, cell_count);

    for (uint64_t cell = 0U; cell < cell_count; ++cell) {
        uint64_t start = (cell * total_units) / cell_count;
        uint64_t end = ((cell + 1U) * total_units) / cell_count;
        uint64_t used = 0U;
        uint64_t block;
        uint64_t free_count;
        uint64_t frag_count;
        for (block = start; block < end; ++block)
            if (allocation_block_used(bitmap, (uint16_t)block))
                ++used;
        free_count = (end - start) - used;
        frag_count = fragmented_in_range(fragmented, start, end);
        free_total += free_count;
        used_total += used;
        if (cell)
            putchar(',');
        printf("{\"start\":%" PRIu64 ",\"end\":%" PRIu64
               ",\"free\":%" PRIu64 ",\"used\":%" PRIu64
               ",\"unknown\":0,\"bad\":0,\"fragmented\":%" PRIu64
               ",\"directory\":0}",
               start, end - 1U, free_count, used, frag_count);
    }

    printf("],\"total_bytes\":%" PRIu64 ",\"free_bytes\":%" PRIu64
           ",\"used_bytes\":%" PRIu64 ",\"unknown_bytes\":0,"
           "\"regular_files\":%" PRIu32 ",\"directories\":%" PRIu32
           ",\"fragmented_files\":%" PRIu32
           ",\"fragmented_directories\":0,\"details\":{"
           "\"header_free_blocks\":%u,\"file_count\":%" PRIu32
           ",\"folder_count\":%" PRIu32
           ",\"fragmentation_available\":%s}}\n",
           total_units * (uint64_t)volume.allocation_block_size,
           free_total * (uint64_t)volume.allocation_block_size,
           used_total * (uint64_t)volume.allocation_block_size,
           result.files, result.directories, result.fragmented_files,
           (unsigned int)volume.header_free_blocks,
           volume.header_files, volume.header_directories,
           deep_ok ? "true" : "false");
    rc = 0;

done:
    free(fragmented);
    free(bitmap);
    free(result.fragmented_extents);
    hfs_close_volume(&volume);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "scan-json") == 0)
        return scan_json(argv[2]);
    if (argc == 3 && strcmp(argv[1], "identify") == 0)
        return identify_json(argv[2]);
    if (argc == 5 && strcmp(argv[1], "map") == 0 &&
        strcmp(argv[3], "--cells") == 0) {
        uint64_t cells = 0U;
        if (parse_cells(argv[4], &cells) != 0) {
            fprintf(stderr, "hfs-analyser: invalid cell count\n");
            return 2;
        }
        return map_json(argv[2], cells);
    }
    fprintf(stderr,
            "usage: hfs-analyser identify DEVICE | scan-json DEVICE | "
            "map DEVICE --cells COUNT\n");
    return 2;
}
