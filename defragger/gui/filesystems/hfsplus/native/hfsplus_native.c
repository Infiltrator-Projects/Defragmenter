// SPDX-License-Identifier: GPL-3.0-or-later
#define _FILE_OFFSET_BITS 64
#include "hfsplus_native.h"

#include "ld_device.h"
#include "ld_io.h"

#include "infiltratr/endian.h"
#include "infiltratr/arithmetic.h"
#include "infiltratr/posix_io.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HFSPLUS_SIG 0x482bU
#define HFSX_SIG 0x4858U
#define HFSPLUS_VERSION 4U
#define HFSX_VERSION 5U
#define HFS_ATTR_UNMOUNTED 0x00000100U
#define HFS_ATTR_JOURNALED 0x00002000U
#define HFS_ATTR_INCONSISTENT 0x00000800U
#define HFS_ATTR_RESERVED14 0x00004000U
#define HFS_ATTR_SOFTWARE_LOCK 0x00008000U
#define HFS_EXTENTS_FILE_ID 3U
#define HFS_CATALOG_FILE_ID 4U
#define HFS_ALLOCATION_FILE_ID 6U
#define HFS_FIRST_USER_ID 16U
#define HFS_FORK_DATA 0U
#define HFS_FORK_RESOURCE 0xffU
#define HFS_RECORD_FOLDER 1U
#define HFS_RECORD_FILE 2U
#define HFS_BT_LEAF (-1)
#define HFS_MAX_NODE 65536U
#define HFS_IO_CHUNK (4U * 1024U * 1024U)
#define HFS_SHA256_LEN 32U
#define HFS_JI_IN_FS 0x00000001U
#define HFS_JI_ON_OTHER_DEVICE 0x00000002U
#define HFS_JI_NEED_INIT 0x00000004U
#define HFS_JOURNAL_MAGIC 0x4a4e4c78U
#define HFS_OLD_JOURNAL_MAGIC 0x4a484452U
#define HFS_JOURNAL_ENDIAN_MAGIC 0x12345678U
#define HFS_JOURNAL_HEADER_CKSUM_SIZE 44U

typedef struct {
    uint32_t file_id;
    uint8_t fork_type;
    uint32_t start_block;
    uint64_t data_logical_offset;
    HfsPlusExtent extents[8];
} OverflowRecord;

typedef struct {
    OverflowRecord *items;
    size_t count;
    size_t capacity;
} OverflowVec;

static uint32_t journal_checksum(const unsigned char *data, size_t length) {
    uint32_t sum = 0;
    for (size_t i = 0; i < length; ++i)
        sum = (sum << 8) ^ (sum + data[i]);
    return ~sum;
}


void hfsplus_set_error(char **error, const char *fmt, ...) {
    if (!error) return;
    va_list ap;
    va_start(ap, fmt);
    char *message = NULL;
    if (vasprintf(&message, fmt, ap) < 0) message = NULL;
    va_end(ap);
    free(*error);
    *error = message;
}

static int read_exact(int fd, uint64_t offset, void *buffer, size_t length, char **error) {
    if (infiltratr_pread_full(fd, buffer, length, offset) == 0) return 0;
    hfsplus_set_error(error, "cannot read HFS+ data at offset %" PRIu64 ": %s",
                      offset, strerror(errno));
    return -1;
}

static int write_exact(int fd, uint64_t offset, const void *buffer, size_t length, char **error) {
    if (infiltratr_pwrite_full(fd, buffer, length, offset) == 0) return 0;
    hfsplus_set_error(error, "cannot write HFS+ data at offset %" PRIu64 ": %s",
                      offset, strerror(errno));
    return -1;
}

static int fork_push_extent(HfsPlusFork *fork, HfsPlusExtent extent) {
    if (!extent.count) return 0;
    if (fork->extent_count == SIZE_MAX ||
        !infiltratr_array_reserve((void **)&fork->extents, &fork->extent_capacity,
                                  sizeof(*fork->extents), fork->extent_count + 1U, 8U))
        return -1;
    fork->extents[fork->extent_count++] = extent;
    return 0;
}

static void fork_free(HfsPlusFork *fork) {
    free(fork->extents);
    memset(fork, 0, sizeof(*fork));
}

static int parse_fork(const unsigned char *data, size_t offset, HfsPlusFork *fork,
                      char **error) {
    memset(fork, 0, sizeof(*fork));
    fork->logical_size = infiltratr_load_be64(data + offset);
    fork->total_blocks = infiltratr_load_be32(data + offset + 12U);
    for (size_t i = 0; i < 8U; ++i) {
        HfsPlusExtent extent = {
            infiltratr_load_be32(data + offset + 16U + i * 8U),
            infiltratr_load_be32(data + offset + 20U + i * 8U)
        };
        if (fork_push_extent(fork, extent)) {
            hfsplus_set_error(error, "out of memory reading HFS+ fork extents");
            return -1;
        }
    }
    return 0;
}

static uint64_t fork_described_blocks(const HfsPlusFork *fork) {
    uint64_t total = 0;
    for (size_t i = 0; i < fork->extent_count; ++i) total += fork->extents[i].count;
    return total;
}

static int fork_io(const HfsPlusVolume *volume, const HfsPlusFork *fork,
                   uint64_t logical, void *buffer, size_t length, bool writing,
                   char **error) {
    if (logical > fork->logical_size || (uint64_t)length > fork->logical_size - logical) {
        hfsplus_set_error(error, "HFS+ fork access is outside the logical fork size");
        return -1;
    }
    unsigned char *bytes = buffer;
    uint64_t cursor = 0;
    size_t done = 0;
    for (size_t i = 0; i < fork->extent_count && done < length; ++i) {
        uint64_t extent_bytes = (uint64_t)fork->extents[i].count * volume->block_size;
        if (logical >= cursor + extent_bytes) {
            cursor += extent_bytes;
            continue;
        }
        if (logical < cursor) {
            hfsplus_set_error(error, "HFS+ fork contains an unrepresented extent gap");
            return -1;
        }
        uint64_t inside = logical - cursor;
        size_t take = length - done;
        uint64_t available = extent_bytes - inside;
        if ((uint64_t)take > available) take = (size_t)available;
        uint64_t physical = (uint64_t)fork->extents[i].start * volume->block_size + inside;
        int rc = writing ? write_exact(volume->fd, physical, bytes + done, take, error)
                         : read_exact(volume->fd, physical, bytes + done, take, error);
        if (rc) return -1;
        done += take;
        logical += take;
        cursor += extent_bytes;
    }
    if (done != length) {
        hfsplus_set_error(error, "HFS+ fork extents do not cover requested data");
        return -1;
    }
    return 0;
}

static int fork_read(const HfsPlusVolume *v, const HfsPlusFork *f, uint64_t off,
                     void *buf, size_t len, char **error) {
    return fork_io(v, f, off, buf, len, false, error);
}

static int fork_write(const HfsPlusVolume *v, const HfsPlusFork *f, uint64_t off,
                      void *buf, size_t len, char **error) {
    return fork_io(v, f, off, buf, len, true, error);
}

static int overflow_push(OverflowVec *vec, OverflowRecord item) {
    if (vec->count == SIZE_MAX ||
        !infiltratr_array_reserve((void **)&vec->items, &vec->capacity,
                                  sizeof(*vec->items), vec->count + 1U, 32U))
        return -1;
    vec->items[vec->count++] = item;
    return 0;
}

static int file_push(HfsPlusFileVec *vec, HfsPlusFile item) {
    if (vec->count == SIZE_MAX ||
        !infiltratr_array_reserve((void **)&vec->items, &vec->capacity,
                                  sizeof(*vec->items), vec->count + 1U, 64U))
        return -1;
    vec->items[vec->count++] = item;
    return 0;
}

static void overflow_free(OverflowVec *vec) {
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static int append_overflow(HfsPlusFork *fork, const OverflowVec *overflow,
                           uint32_t file_id, uint8_t fork_type, char **error) {
    uint64_t have = fork_described_blocks(fork);
    if (have >= fork->total_blocks) return 0;
    for (;;) {
        const OverflowRecord *best = NULL;
        for (size_t i = 0; i < overflow->count; ++i) {
            const OverflowRecord *candidate = &overflow->items[i];
            if (candidate->file_id == file_id && candidate->fork_type == fork_type &&
                candidate->start_block == have) {
                best = candidate;
                break;
            }
        }
        if (!best) break;
        fork->uses_overflow = true;
        uint64_t before = have;
        for (size_t i = 0; i < 8U; ++i) {
            if (fork_push_extent(fork, best->extents[i])) {
                hfsplus_set_error(error, "out of memory expanding HFS+ overflow extents");
                return -1;
            }
            have += best->extents[i].count;
        }
        if (have == before) break;
        if (have >= fork->total_blocks) return 0;
    }
    if (fork->logical_size && have < fork->total_blocks) {
        hfsplus_set_error(error,
                          "HFS+ fork %u/%u has unresolved extents overflow records (%" PRIu64 "/%u blocks)",
                          file_id, (unsigned)fork_type, have, fork->total_blocks);
        return -1;
    }
    return 0;
}

static int record_starts(const unsigned char *node, uint32_t node_size, uint16_t count,
                         uint16_t **starts_out, char **error) {
    if ((uint32_t)(count + 1U) * 2U > node_size - 14U) {
        hfsplus_set_error(error, "invalid HFS+ B-tree node record count");
        return -1;
    }
    uint16_t *starts = calloc(count ? count : 1U, sizeof(*starts));
    if (!starts) {
        hfsplus_set_error(error, "out of memory reading HFS+ B-tree offsets");
        return -1;
    }
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t value = infiltratr_load_be16(node + node_size - 2U * (uint32_t)(i + 1U));
        if (value < 14U || value >= node_size) {
            free(starts);
            hfsplus_set_error(error, "invalid HFS+ B-tree record offset");
            return -1;
        }
        starts[i] = value;
    }
    for (uint16_t i = 0; i < count; ++i) {
        for (uint16_t j = (uint16_t)(i + 1U); j < count; ++j) {
            if (starts[j] < starts[i]) {
                uint16_t tmp = starts[i]; starts[i] = starts[j]; starts[j] = tmp;
            }
        }
        if (i && starts[i] == starts[i - 1U]) {
            free(starts);
            hfsplus_set_error(error, "duplicate HFS+ B-tree record offset");
            return -1;
        }
    }
    *starts_out = starts;
    return 0;
}

static int btree_geometry(const HfsPlusVolume *volume, const HfsPlusFork *fork,
                          uint32_t *node_size, uint32_t *first_leaf,
                          uint32_t *total_nodes, char **error) {
    unsigned char prefix[512];
    if (fork->logical_size < sizeof(prefix) || fork_read(volume, fork, 0, prefix, sizeof(prefix), error)) return -1;
    uint32_t size = infiltratr_load_be16(prefix + 32U);
    if (size < 512U || size > HFS_MAX_NODE || (size & (size - 1U))) {
        hfsplus_set_error(error, "invalid HFS+ B-tree node size %u", size);
        return -1;
    }
    uint32_t first = infiltratr_load_be32(prefix + 24U);
    uint32_t total = infiltratr_load_be32(prefix + 36U);
    if (!total || (uint64_t)total * size > fork->logical_size) {
        hfsplus_set_error(error, "invalid HFS+ B-tree total-node geometry");
        return -1;
    }
    *node_size = size;
    *first_leaf = first;
    *total_nodes = total;
    return 0;
}

static int scan_overflow(HfsPlusVolume *volume, OverflowVec *overflow, char **error) {
    uint32_t node_size = 0, first_leaf = 0, total_nodes = 0;
    if (fork_described_blocks(&volume->extents_fork) < volume->extents_fork.total_blocks) {
        hfsplus_set_error(error, "HFS+ extents overflow file itself uses unresolved overflow extents");
        return -1;
    }
    if (!volume->extents_fork.logical_size) return 0;
    if (btree_geometry(volume, &volume->extents_fork, &node_size, &first_leaf, &total_nodes, error)) return -1;
    unsigned char *node = malloc(node_size);
    if (!node) { hfsplus_set_error(error, "out of memory reading HFS+ extents B-tree"); return -1; }
    uint8_t *seen = ld_bitmap_calloc(total_nodes);
    if (!seen) { free(node); hfsplus_set_error(error, "out of memory tracking HFS+ extents B-tree"); return -1; }
    uint32_t current = first_leaf;
    while (current) {
        if (current >= total_nodes || ld_bitmap_get(seen, current)) {
            hfsplus_set_error(error, "HFS+ extents leaf chain is cyclic or out of range");
            free(seen); free(node); return -1;
        }
        ld_bitmap_set(seen, current, true);
        if (fork_read(volume, &volume->extents_fork, (uint64_t)current * node_size, node, node_size, error)) {
            free(seen); free(node); return -1;
        }
        if ((int8_t)node[8] != HFS_BT_LEAF) {
            hfsplus_set_error(error, "HFS+ extents leaf chain points to a non-leaf node");
            free(seen); free(node); return -1;
        }
        uint16_t count = infiltratr_load_be16(node + 10U);
        uint16_t *starts = NULL;
        if (record_starts(node, node_size, count, &starts, error)) { free(seen); free(node); return -1; }
        for (uint16_t r = 0; r < count; ++r) {
            uint32_t start = starts[r];
            uint32_t end = (r + 1U < count) ? starts[r + 1U] : node_size - 2U * (uint32_t)(count + 1U);
            if (end <= start + 12U || end > node_size) continue;
            uint16_t key_len = infiltratr_load_be16(node + start);
            uint32_t data_off = start + 2U + key_len;
            if (data_off & 1U) ++data_off;
            if (key_len < 10U || data_off + 64U > end) continue;
            OverflowRecord item;
            memset(&item, 0, sizeof(item));
            item.fork_type = node[start + 2U];
            item.file_id = infiltratr_load_be32(node + start + 4U);
            item.start_block = infiltratr_load_be32(node + start + 8U);
            item.data_logical_offset = (uint64_t)current * node_size + data_off;
            for (size_t i = 0; i < 8U; ++i) {
                item.extents[i].start = infiltratr_load_be32(node + data_off + i * 8U);
                item.extents[i].count = infiltratr_load_be32(node + data_off + i * 8U + 4U);
            }
            if (overflow_push(overflow, item)) {
                free(starts); free(seen); free(node);
                hfsplus_set_error(error, "out of memory collecting HFS+ overflow records");
                return -1;
            }
        }
        free(starts);
        current = infiltratr_load_be32(node);
    }
    free(seen); free(node);
    return 0;
}

static size_t fork_fragments(const HfsPlusFork *fork) {
    if (!fork->extent_count || !fork->total_blocks) return 0;
    size_t fragments = 1;
    uint64_t described = 0;
    HfsPlusExtent previous = {0, 0};
    bool have = false;
    for (size_t i = 0; i < fork->extent_count && described < fork->total_blocks; ++i) {
        HfsPlusExtent extent = fork->extents[i];
        if (!extent.count) continue;
        if (have && previous.start + previous.count != extent.start) ++fragments;
        previous = extent;
        have = true;
        described += extent.count;
    }
    return have ? fragments : 0;
}

static int scan_catalog(HfsPlusVolume *volume, const OverflowVec *overflow, char **error) {
    uint32_t node_size = 0, first_leaf = 0, total_nodes = 0;
    if (append_overflow(&volume->catalog_fork, overflow, HFS_CATALOG_FILE_ID, HFS_FORK_DATA, error)) return -1;
    if (btree_geometry(volume, &volume->catalog_fork, &node_size, &first_leaf, &total_nodes, error)) return -1;
    volume->catalog_node_size = node_size;
    unsigned char *node = malloc(node_size);
    uint8_t *seen = ld_bitmap_calloc(total_nodes);
    if (!node || !seen) {
        free(node); free(seen); hfsplus_set_error(error, "out of memory scanning HFS+ catalog"); return -1;
    }
    uint32_t current = first_leaf;
    while (current) {
        if (current >= total_nodes || ld_bitmap_get(seen, current)) {
            hfsplus_set_error(error, "HFS+ catalog leaf chain is cyclic or out of range");
            free(seen); free(node); return -1;
        }
        ld_bitmap_set(seen, current, true);
        uint64_t node_logical = (uint64_t)current * node_size;
        if (fork_read(volume, &volume->catalog_fork, node_logical, node, node_size, error)) {
            free(seen); free(node); return -1;
        }
        if ((int8_t)node[8] != HFS_BT_LEAF) {
            hfsplus_set_error(error, "HFS+ catalog leaf chain points to a non-leaf node");
            free(seen); free(node); return -1;
        }
        uint16_t count = infiltratr_load_be16(node + 10U);
        uint16_t *starts = NULL;
        if (record_starts(node, node_size, count, &starts, error)) { free(seen); free(node); return -1; }
        for (uint16_t r = 0; r < count; ++r) {
            uint32_t start = starts[r];
            uint32_t end = (r + 1U < count) ? starts[r + 1U] : node_size - 2U * (uint32_t)(count + 1U);
            if (end <= start + 8U || end > node_size) continue;
            uint16_t key_len = infiltratr_load_be16(node + start);
            uint32_t data_off = start + 2U + key_len;
            if (data_off & 1U) ++data_off;
            if (data_off + 2U > end) continue;
            uint16_t record_type = infiltratr_load_be16(node + data_off);
            if (record_type == HFS_RECORD_FOLDER) {
                ++volume->directories;
                continue;
            }
            if (record_type != HFS_RECORD_FILE || data_off + 248U > end) continue;
            HfsPlusFile file;
            memset(&file, 0, sizeof(file));
            file.file_id = infiltratr_load_be32(node + data_off + 8U);
            if (parse_fork(node, data_off + 88U, &file.data_fork, error) ||
                parse_fork(node, data_off + 168U, &file.resource_fork, error)) {
                fork_free(&file.data_fork); fork_free(&file.resource_fork);
                free(starts); free(seen); free(node); return -1;
            }
            file.data_fork.catalog_fork_offset = node_logical + data_off + 88U;
            file.resource_fork.catalog_fork_offset = node_logical + data_off + 168U;
            if (append_overflow(&file.data_fork, overflow, file.file_id, HFS_FORK_DATA, error) ||
                append_overflow(&file.resource_fork, overflow, file.file_id, HFS_FORK_RESOURCE, error)) {
                fork_free(&file.data_fork); fork_free(&file.resource_fork);
                free(starts); free(seen); free(node); return -1;
            }
            if (file_push(&volume->files, file)) {
                fork_free(&file.data_fork); fork_free(&file.resource_fork);
                free(starts); free(seen); free(node);
                hfsplus_set_error(error, "out of memory collecting HFS+ catalog files");
                return -1;
            }
        }
        free(starts);
        current = infiltratr_load_be32(node);
    }
    free(seen); free(node);
    return 0;
}

static int load_allocation_map(HfsPlusVolume *volume, const OverflowVec *overflow, char **error) {
    if (append_overflow(&volume->allocation_fork, overflow, HFS_ALLOCATION_FILE_ID, HFS_FORK_DATA, error)) return -1;
    uint64_t bytes = ((uint64_t)volume->total_blocks + 7U) / 8U;
    if (volume->allocation_fork.logical_size < bytes) {
        hfsplus_set_error(error, "HFS+ allocation file is too small for the volume bitmap");
        return -1;
    }
    unsigned char *bitmap = malloc((size_t)bytes);
    volume->used_map = ld_bitmap_calloc(volume->total_blocks);
    if (!bitmap || !volume->used_map) {
        free(bitmap); hfsplus_set_error(error, "out of memory reading HFS+ allocation bitmap"); return -1;
    }
    if (fork_read(volume, &volume->allocation_fork, 0, bitmap, (size_t)bytes, error)) {
        free(bitmap); return -1;
    }
    for (uint32_t block = 0; block < volume->total_blocks; ++block) {
        ld_bitmap_set(volume->used_map, block,
                      (bitmap[block / 8U] &
                       (unsigned char)(1U << (7U - (block % 8U)))) != 0U);
    }
    free(bitmap);
    return 0;
}

static bool block_in_journal_metadata(const HfsPlusVolume *volume, uint32_t block) {
    if (!(volume->attributes & HFS_ATTR_JOURNALED) || !volume->journal_checked) return false;
    if (block == volume->journal_info_block) return true;
    if (!volume->journal_size) return false;
    uint64_t first = volume->journal_offset / volume->block_size;
    uint64_t end = (volume->journal_offset + volume->journal_size + volume->block_size - 1U) /
                   volume->block_size;
    return block >= first && block < end;
}

static bool fork_is_protected(const HfsPlusVolume *volume, const HfsPlusFork *fork,
                              uint32_t file_id) {
    if (file_id < HFS_FIRST_USER_ID) return true;
    uint64_t described = 0;
    for (size_t i = 0; i < fork->extent_count && described < fork->total_blocks; ++i) {
        HfsPlusExtent e = fork->extents[i];
        for (uint32_t b = 0; b < e.count && described < fork->total_blocks; ++b, ++described)
            if (block_in_journal_metadata(volume, e.start + b)) return true;
    }
    return false;
}

static bool file_is_protected(const HfsPlusVolume *volume, const HfsPlusFile *file) {
    if (file->file_id < HFS_FIRST_USER_ID) return true;
    return (file->data_fork.total_blocks &&
            fork_is_protected(volume, &file->data_fork, file->file_id)) ||
           (file->resource_fork.total_blocks &&
            fork_is_protected(volume, &file->resource_fork, file->file_id));
}

static int validate_clean_journal(HfsPlusVolume *volume, char **error) {
    volume->journal_checked = true;
    volume->journal_empty = true;
    volume->journal_need_init = false;
    volume->journal_offset = 0;
    volume->journal_size = 0;
    if (!(volume->attributes & HFS_ATTR_JOURNALED)) return 0;
    if (volume->journal_info_block >= volume->total_blocks) {
        hfsplus_set_error(error, "HFS+ journal info block is outside the volume");
        return -1;
    }
    if (!ld_bitmap_get(volume->used_map, volume->journal_info_block)) {
        hfsplus_set_error(error, "HFS+ journal info block is marked free in the allocation bitmap");
        return -1;
    }
    unsigned char jib[180];
    uint64_t jib_offset = (uint64_t)volume->journal_info_block * volume->block_size;
    if (read_exact(volume->fd, jib_offset, jib, sizeof(jib), error)) return -1;
    uint32_t flags = infiltratr_load_be32(jib);
    if (flags & ~(HFS_JI_IN_FS | HFS_JI_ON_OTHER_DEVICE | HFS_JI_NEED_INIT)) {
        hfsplus_set_error(error, "HFS+ journal info block contains unknown reserved flags 0x%08x", flags);
        return -1;
    }
    if (!(flags & HFS_JI_IN_FS) || (flags & HFS_JI_ON_OTHER_DEVICE)) {
        hfsplus_set_error(error, "HFS+ external-device journals are not supported by the raw writer");
        return -1;
    }
    volume->journal_offset = infiltratr_load_be64(jib + 36U);
    volume->journal_size = infiltratr_load_be64(jib + 44U);
    if (!volume->journal_size || volume->journal_offset >= volume->bytes ||
        volume->journal_size > volume->bytes - volume->journal_offset) {
        hfsplus_set_error(error, "HFS+ journal range is outside the volume");
        return -1;
    }
    uint64_t first_block = volume->journal_offset / volume->block_size;
    uint64_t last_block = (volume->journal_offset + volume->journal_size + volume->block_size - 1U) /
                          volume->block_size;
    if (last_block > volume->total_blocks) {
        hfsplus_set_error(error, "HFS+ journal allocation exceeds the volume");
        return -1;
    }
    for (uint64_t block = first_block; block < last_block; ++block) {
        if (!ld_bitmap_get(volume->used_map, block)) {
            hfsplus_set_error(error, "HFS+ journal block %" PRIu64 " is marked free in the allocation bitmap", block);
            return -1;
        }
    }
    if (flags & HFS_JI_NEED_INIT) {
        volume->journal_need_init = true;
        return 0;
    }
    unsigned char raw[HFS_JOURNAL_HEADER_CKSUM_SIZE];
    if (read_exact(volume->fd, volume->journal_offset, raw, sizeof(raw), error)) return -1;
    bool little = false;
    if (infiltratr_load_le32(raw + 4U) == HFS_JOURNAL_ENDIAN_MAGIC) little = true;
    else if (infiltratr_load_be32(raw + 4U) != HFS_JOURNAL_ENDIAN_MAGIC) {
        hfsplus_set_error(error, "HFS+ journal header has invalid endian magic");
        return -1;
    }
#define J32(off) (little ? infiltratr_load_le32(raw + (off)) : infiltratr_load_be32(raw + (off)))
#define J64(off) (little ? infiltratr_load_le64(raw + (off)) : infiltratr_load_be64(raw + (off)))
    uint32_t magic = J32(0U);
    if (magic != HFS_JOURNAL_MAGIC && magic != HFS_OLD_JOURNAL_MAGIC) {
        hfsplus_set_error(error, "HFS+ journal header has invalid magic 0x%08x", magic);
        return -1;
    }
    uint64_t start = J64(8U);
    uint64_t end = J64(16U);
    uint64_t size = J64(24U);
    uint32_t blhdr_size = J32(32U);
    uint32_t checksum = J32(36U);
    uint32_t jhdr_size = J32(40U);
    if (size != volume->journal_size || jhdr_size < 512U || jhdr_size > size ||
        (jhdr_size & (jhdr_size - 1U)) || !blhdr_size || blhdr_size > size ||
        start < jhdr_size || start >= size || end < jhdr_size || end >= size) {
        hfsplus_set_error(error, "HFS+ journal header geometry is inconsistent");
        return -1;
    }
    if (magic == HFS_JOURNAL_MAGIC) {
        unsigned char checksum_data[HFS_JOURNAL_HEADER_CKSUM_SIZE];
        memcpy(checksum_data, raw, sizeof(checksum_data));
        memset(checksum_data + 36U, 0, 4U);
        if (journal_checksum(checksum_data, sizeof(checksum_data)) != checksum) {
            hfsplus_set_error(error, "HFS+ journal header checksum is invalid");
            return -1;
        }
    }
    if (start != end) {
        volume->journal_empty = false;
        hfsplus_set_error(error,
            "HFS+ journal contains pending transactions (start=%" PRIu64 ", end=%" PRIu64 "); raw mutation requires journal replay first",
            start, end);
        return -1;
    }
#undef J32
#undef J64
    return 0;
}

static int validate_fork_ranges(const HfsPlusVolume *volume, const HfsPlusFork *fork,
                                uint32_t file_id, const char *kind, char **error) {
    uint64_t described = 0;
    for (size_t i = 0; i < fork->extent_count && described < fork->total_blocks; ++i) {
        HfsPlusExtent e = fork->extents[i];
        if (!e.count) continue;
        if (e.start >= volume->total_blocks || e.count > volume->total_blocks - e.start) {
            hfsplus_set_error(error, "HFS+ file %u %s fork extent is outside the volume", file_id, kind);
            return -1;
        }
        described += e.count;
    }
    if (fork->logical_size && described < fork->total_blocks) {
        hfsplus_set_error(error, "HFS+ file %u %s fork has unresolved allocation blocks", file_id, kind);
        return -1;
    }
    return 0;
}

int hfsplus_identify(const char *path, uint16_t *signature, uint16_t *version,
                     uint32_t *attributes, char **error) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        hfsplus_set_error(error, "cannot open HFS+ volume %s: %s", path, strerror(errno));
        return -1;
    }
    unsigned char header[16];
    int rc = read_exact(fd, 1024U, header, sizeof(header), error);
    close(fd);
    if (rc) return -1;
    uint16_t sig = infiltratr_load_be16(header);
    uint16_t ver = infiltratr_load_be16(header + 2U);
    if ((sig != HFSPLUS_SIG || ver != HFSPLUS_VERSION) &&
        (sig != HFSX_SIG || ver != HFSX_VERSION)) {
        hfsplus_set_error(error, "not an HFS+ or HFSX volume");
        return -1;
    }
    if (signature) *signature = sig;
    if (version) *version = ver;
    if (attributes) *attributes = infiltratr_load_be32(header + 4U);
    return 0;
}

int hfsplus_scan(const char *path, bool writable, HfsPlusVolume *volume, char **error) {
    memset(volume, 0, sizeof(*volume));
    volume->fd = -1;
    int flags = writable ? O_RDWR : O_RDONLY;
    volume->fd = open(path, flags | O_CLOEXEC);
    if (volume->fd < 0) {
        hfsplus_set_error(error, "cannot open HFS+ volume %s: %s", path, strerror(errno));
        return -1;
    }
    if (ld_fd_size_bytes(volume->fd, &volume->bytes) != 0) {
        hfsplus_set_error(error, "cannot determine HFS+ volume capacity: %s",
                          strerror(errno));
        hfsplus_close(volume); return -1;
    }
    if (volume->bytes < 4096U) {
        hfsplus_set_error(error, "HFS+ volume is too small"); hfsplus_close(volume); return -1;
    }
    unsigned char header[512];
    if (read_exact(volume->fd, 1024U, header, sizeof(header), error)) { hfsplus_close(volume); return -1; }
    volume->signature = infiltratr_load_be16(header);
    volume->version = infiltratr_load_be16(header + 2U);
    if ((volume->signature != HFSPLUS_SIG || volume->version != HFSPLUS_VERSION) &&
        (volume->signature != HFSX_SIG || volume->version != HFSX_VERSION)) {
        hfsplus_set_error(error, "not an HFS+ or HFSX volume"); hfsplus_close(volume); return -1;
    }
    volume->attributes = infiltratr_load_be32(header + 4U);
    volume->journal_info_block = infiltratr_load_be32(header + 12U);
    volume->block_size = infiltratr_load_be32(header + 40U);
    volume->total_blocks = infiltratr_load_be32(header + 44U);
    volume->free_blocks = infiltratr_load_be32(header + 48U);
    if (volume->block_size < 512U || (volume->block_size & (volume->block_size - 1U)) || !volume->total_blocks) {
        hfsplus_set_error(error, "invalid HFS+ allocation geometry"); hfsplus_close(volume); return -1;
    }
    if ((uint64_t)volume->total_blocks * volume->block_size > volume->bytes) {
        hfsplus_set_error(error, "HFS+ allocation geometry exceeds the device"); hfsplus_close(volume); return -1;
    }
    if (parse_fork(header, 112U, &volume->allocation_fork, error) ||
        parse_fork(header, 192U, &volume->extents_fork, error) ||
        parse_fork(header, 272U, &volume->catalog_fork, error)) {
        hfsplus_close(volume); return -1;
    }
    OverflowVec overflow = {0};
    if (scan_overflow(volume, &overflow, error) ||
        load_allocation_map(volume, &overflow, error) ||
        validate_clean_journal(volume, error) ||
        scan_catalog(volume, &overflow, error)) {
        overflow_free(&overflow); hfsplus_close(volume); return -1;
    }
    overflow_free(&overflow);
    for (size_t i = 0; i < volume->files.count; ++i) {
        HfsPlusFile *file = &volume->files.items[i];
        if (validate_fork_ranges(volume, &file->data_fork, file->file_id, "data", error) ||
            validate_fork_ranges(volume, &file->resource_fork, file->file_id, "resource", error)) {
            hfsplus_close(volume); return -1;
        }
    }
    return 0;
}

void hfsplus_close(HfsPlusVolume *volume) {
    if (!volume) return;
    if (volume->fd >= 0) close(volume->fd);
    fork_free(&volume->allocation_fork);
    fork_free(&volume->extents_fork);
    fork_free(&volume->catalog_fork);
    for (size_t i = 0; i < volume->files.count; ++i) {
        fork_free(&volume->files.items[i].data_fork);
        fork_free(&volume->files.items[i].resource_fork);
    }
    free(volume->files.items);
    free(volume->used_map);
    memset(volume, 0, sizeof(*volume));
    volume->fd = -1;
}

static void emit_ranges_json(const HfsPlusVolume *volume, bool used, const char *name) {
    printf("\"%s\":[", name);
    bool first = true;
    uint32_t block = 0;
    while (block < volume->total_blocks) {
        bool state = ld_bitmap_get(volume->used_map, block);
        if (state != used) { ++block; continue; }
        uint32_t start = block++;
        while (block < volume->total_blocks && (ld_bitmap_get(volume->used_map, block)) == used) ++block;
        if (!first) putchar(',');
        printf("[%u,%u]", start, block);
        first = false;
    }
    putchar(']');
}

static void emit_fork_blocks(const HfsPlusFork *fork, bool *first) {
    uint64_t described = 0;
    for (size_t i = 0; i < fork->extent_count && described < fork->total_blocks; ++i) {
        HfsPlusExtent e = fork->extents[i];
        if (!e.count) continue;
        if (!*first) putchar(',');
        printf("[%u,%u]", e.start, e.start + e.count);
        *first = false;
        described += e.count;
    }
}

int hfsplus_analyse_json(const char *path, char **error) {
    HfsPlusVolume volume;
    if (hfsplus_scan(path, false, &volume, error)) return -1;
    uint32_t fragmented_files = 0;
    printf("{\"filesystem\":\"hfsplus\",\"variant\":\"%s\",\"signature\":\"%s\",",
           volume.signature == HFSX_SIG ? "HFSX" : "HFS+",
           volume.signature == HFSX_SIG ? "HX" : "H+");
    printf("\"block_size\":%u,\"total_blocks\":%u,\"free_blocks\":%u,",
           volume.block_size, volume.total_blocks, volume.free_blocks);
    size_t regular_files = 0;
    for (size_t i = 0; i < volume.files.count; ++i) {
        HfsPlusFile *file = &volume.files.items[i];
        if (file_is_protected(&volume, file)) continue;
        bool protected_data = fork_is_protected(&volume, &file->data_fork, file->file_id);
        bool protected_rsrc = fork_is_protected(&volume, &file->resource_fork, file->file_id);
        ++regular_files;
        if ((!protected_data && fork_fragments(&file->data_fork) > 1U) ||
            (!protected_rsrc && fork_fragments(&file->resource_fork) > 1U)) ++fragmented_files;
    }
    printf("\"journaled\":%s,\"journal_empty\":%s,\"regular_files\":%zu,\"directories\":%u,",
           (volume.attributes & HFS_ATTR_JOURNALED) ? "true" : "false",
           (!(volume.attributes & HFS_ATTR_JOURNALED) || volume.journal_empty || volume.journal_need_init) ? "true" : "false",
           regular_files, volume.directories);
    printf("\"fragmented_files\":%u,\"fragmented_directories\":0,", fragmented_files);
    emit_ranges_json(&volume, false, "free_ranges");
    printf(",\"fragmented_ranges\":[");
    bool first = true;
    for (size_t i = 0; i < volume.files.count; ++i) {
        HfsPlusFile *file = &volume.files.items[i];
        if (!fork_is_protected(&volume, &file->data_fork, file->file_id) &&
            fork_fragments(&file->data_fork) > 1U) emit_fork_blocks(&file->data_fork, &first);
        if (!fork_is_protected(&volume, &file->resource_fork, file->file_id) &&
            fork_fragments(&file->resource_fork) > 1U) emit_fork_blocks(&file->resource_fork, &first);
    }
    printf("],\"directory_ranges\":[");
    first = true;
    emit_fork_blocks(&volume.catalog_fork, &first);
    printf("]}\n");
    hfsplus_close(&volume);
    return 0;
}

static int copy_bytes(int from, int to, uint64_t offset, uint64_t length, char **error) {
    size_t buffer_size = HFS_IO_CHUNK;
    unsigned char *buffer = malloc(buffer_size);
    if (!buffer) { hfsplus_set_error(error, "out of memory copying HFS+ stage"); return -1; }
    uint64_t done = 0;
    while (done < length) {
        size_t take = buffer_size;
        if ((uint64_t)take > length - done) take = (size_t)(length - done);
        if (read_exact(from, offset + done, buffer, take, error) ||
            write_exact(to, offset + done, buffer, take, error)) {
            free(buffer); return -1;
        }
        done += take;
    }
    free(buffer);
    return 0;
}

static int copy_allocated(const HfsPlusVolume *source, int target, char **error) {
    uint32_t block = 0;
    while (block < source->total_blocks) {
        if (!ld_bitmap_get(source->used_map, block)) { ++block; continue; }
        uint32_t start = block++;
        while (block < source->total_blocks && ld_bitmap_get(source->used_map, block)) ++block;
        uint64_t offset = (uint64_t)start * source->block_size;
        uint64_t length = (uint64_t)(block - start) * source->block_size;
        if (copy_bytes(source->fd, target, offset, length, error)) return -1;
    }
    if (copy_bytes(source->fd, target, 0, 1536U, error)) return -1;
    if (source->bytes >= 1024U && copy_bytes(source->fd, target, source->bytes - 1024U, 1024U, error)) return -1;
    return 0;
}

static int fork_hash(const HfsPlusVolume *volume, const HfsPlusFork *fork,
                     unsigned char digest[HFS_SHA256_LEN], char **error) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(ctx);
        hfsplus_set_error(error, "cannot initialise HFS+ SHA-256");
        return -1;
    }
    unsigned char *buffer = malloc(1024U * 1024U);
    if (!buffer) {
        EVP_MD_CTX_free(ctx);
        hfsplus_set_error(error, "out of memory hashing HFS+ fork");
        return -1;
    }
    uint64_t logical = 0;
    while (logical < fork->logical_size) {
        size_t take = 1024U * 1024U;
        if ((uint64_t)take > fork->logical_size - logical)
            take = (size_t)(fork->logical_size - logical);
        if (fork_read(volume, fork, logical, buffer, take, error) ||
            EVP_DigestUpdate(ctx, buffer, take) != 1) {
            free(buffer);
            EVP_MD_CTX_free(ctx);
            if (!*error) hfsplus_set_error(error, "cannot hash HFS+ fork");
            return -1;
        }
        logical += take;
    }
    free(buffer);
    unsigned int digest_length = 0;
    int rc = EVP_DigestFinal_ex(ctx, digest, &digest_length);
    EVP_MD_CTX_free(ctx);
    if (rc != 1 || digest_length != HFS_SHA256_LEN) {
        hfsplus_set_error(error, "cannot finalise HFS+ SHA-256");
        return -1;
    }
    return 0;
}

static HfsPlusFile *find_file(HfsPlusVolume *volume, uint32_t file_id) {
    for (size_t i = 0; i < volume->files.count; ++i)
        if (volume->files.items[i].file_id == file_id) return &volume->files.items[i];
    return NULL;
}

static int compare_payloads(HfsPlusVolume *source, HfsPlusVolume *stage, char **error) {
    for (size_t i = 0; i < source->files.count; ++i) {
        HfsPlusFile *before = &source->files.items[i];
        HfsPlusFile *after = find_file(stage, before->file_id);
        if (!after) { hfsplus_set_error(error, "HFS+ file %u disappeared from staged catalog", before->file_id); return -1; }
        HfsPlusFork *bf[2] = {&before->data_fork, &before->resource_fork};
        HfsPlusFork *af[2] = {&after->data_fork, &after->resource_fork};
        for (size_t k = 0; k < 2U; ++k) {
            if (bf[k]->logical_size != af[k]->logical_size) {
                hfsplus_set_error(error, "HFS+ file %u fork logical size changed", before->file_id); return -1;
            }
            unsigned char left[HFS_SHA256_LEN], right[HFS_SHA256_LEN];
            if (fork_hash(source, bf[k], left, error) || fork_hash(stage, af[k], right, error)) return -1;
            if (memcmp(left, right, sizeof(left))) {
                hfsplus_set_error(error, "HFS+ file %u fork payload hash changed", before->file_id); return -1;
            }
        }
    }
    return 0;
}

static int fork_rewritable(const HfsPlusFork *fork) {
    return fork->catalog_fork_offset != 0U &&
           fork->total_blocks == fork_described_blocks(fork);
}

static int mutation_preflight(const HfsPlusVolume *volume, bool growth,
                              unsigned growth_percent, char **error) {
    (void)growth;
    if (growth_percent != 10U) {
        hfsplus_set_error(error, "HFS+ Growth Defrag requires exactly 10 percent reserve"); return -1;
    }
    if (!(volume->attributes & HFS_ATTR_UNMOUNTED)) {
        hfsplus_set_error(error, "HFS+ volume header does not record a clean unmount"); return -1;
    }
    if (volume->attributes & HFS_ATTR_INCONSISTENT) {
        hfsplus_set_error(error, "HFS+ volume header marks the filesystem inconsistent"); return -1;
    }
    if (volume->attributes & HFS_ATTR_RESERVED14) {
        hfsplus_set_error(error, "HFS+ volume header has reserved attribute bit 14 set"); return -1;
    }
    if (volume->attributes & HFS_ATTR_SOFTWARE_LOCK) {
        hfsplus_set_error(error, "HFS+ volume is software write-locked"); return -1;
    }
    for (size_t i = 0; i < volume->files.count; ++i) {
        HfsPlusFile *file = &volume->files.items[i];
        HfsPlusFork *forks[2] = {&file->data_fork, &file->resource_fork};
        for (size_t k = 0; k < 2U; ++k) {
            if (!forks[k]->total_blocks) continue;
            if (fork_is_protected(volume, forks[k], file->file_id)) continue;
            if (!fork_rewritable(forks[k])) {
                hfsplus_set_error(error,
                    "HFS+ file %u has a fork whose complete extent descriptor chain is not safely rewritable",
                    file->file_id);
                return -1;
            }
        }
    }
    return 0;
}

static int choose_run(uint8_t *claimed, uint32_t total, uint32_t need, uint32_t reserve,
                      uint32_t *start) {
    if (!need) { *start = 0; return 0; }
    uint64_t span = (uint64_t)need + reserve;
    for (uint32_t s = 0; (uint64_t)s + span <= total; ++s) {
        bool ok = true;
        for (uint32_t k = 0; k < need + reserve; ++k) {
            if (ld_bitmap_get(claimed, (uint64_t)s + k)) { s += k; ok = false; break; }
        }
        if (ok) {
            *start = s;
            for (uint32_t k = 0; k < need + reserve; ++k)
                ld_bitmap_set(claimed, (uint64_t)s + k, true);
            return 0;
        }
    }
    return -1;
}

static int rewrite_inline_fork(HfsPlusVolume *stage, HfsPlusFork *fork,
                               uint32_t start, char **error) {
    unsigned char raw[80];
    if (fork_read(stage, &stage->catalog_fork, fork->catalog_fork_offset, raw, sizeof(raw), error)) return -1;
    for (size_t i = 0; i < 8U; ++i) {
        infiltratr_store_be32(raw + 16U + i * 8U, 0U);
        infiltratr_store_be32(raw + 20U + i * 8U, 0U);
    }
    if (fork->total_blocks) {
        infiltratr_store_be32(raw + 16U, start);
        infiltratr_store_be32(raw + 20U, fork->total_blocks);
    }
    if (fork_write(stage, &stage->catalog_fork, fork->catalog_fork_offset, raw, sizeof(raw), error)) return -1;
    return 0;
}

static int rewrite_overflow_fork(HfsPlusVolume *stage, uint32_t file_id,
                                 uint8_t fork_type, HfsPlusFork *fork,
                                 uint32_t destination, char **error) {
    unsigned char catalog_raw[80];
    if (fork_read(stage, &stage->catalog_fork, fork->catalog_fork_offset,
                  catalog_raw, sizeof(catalog_raw), error)) return -1;
    uint32_t logical_block = 0;
    uint32_t physical = destination;
    for (size_t i = 0; i < 8U && logical_block < fork->total_blocks; ++i) {
        uint32_t count = infiltratr_load_be32(catalog_raw + 20U + i * 8U);
        if (!count) continue;
        if (count > fork->total_blocks - logical_block) {
            hfsplus_set_error(error, "HFS+ catalog extent descriptor exceeds fork length");
            return -1;
        }
        infiltratr_store_be32(catalog_raw + 16U + i * 8U, physical);
        physical += count;
        logical_block += count;
    }
    if (fork_write(stage, &stage->catalog_fork, fork->catalog_fork_offset,
                   catalog_raw, sizeof(catalog_raw), error)) return -1;
    if (logical_block == fork->total_blocks) return 0;

    OverflowVec overflow = {0};
    if (scan_overflow(stage, &overflow, error)) return -1;
    while (logical_block < fork->total_blocks) {
        OverflowRecord *record = NULL;
        for (size_t i = 0; i < overflow.count; ++i) {
            if (overflow.items[i].file_id == file_id &&
                overflow.items[i].fork_type == fork_type &&
                overflow.items[i].start_block == logical_block) {
                record = &overflow.items[i];
                break;
            }
        }
        if (!record) {
            overflow_free(&overflow);
            hfsplus_set_error(error,
                "HFS+ file %u fork overflow chain has no record at logical allocation block %u",
                file_id, logical_block);
            return -1;
        }
        unsigned char extent_raw[64];
        if (fork_read(stage, &stage->extents_fork, record->data_logical_offset,
                      extent_raw, sizeof(extent_raw), error)) {
            overflow_free(&overflow); return -1;
        }
        uint32_t before = logical_block;
        for (size_t i = 0; i < 8U && logical_block < fork->total_blocks; ++i) {
            uint32_t count = infiltratr_load_be32(extent_raw + i * 8U + 4U);
            if (!count) continue;
            if (count > fork->total_blocks - logical_block) {
                overflow_free(&overflow);
                hfsplus_set_error(error, "HFS+ overflow extent descriptor exceeds fork length");
                return -1;
            }
            infiltratr_store_be32(extent_raw + i * 8U, physical);
            physical += count;
            logical_block += count;
        }
        if (logical_block == before) {
            overflow_free(&overflow);
            hfsplus_set_error(error, "HFS+ overflow extent record contains no allocation blocks");
            return -1;
        }
        if (fork_write(stage, &stage->extents_fork, record->data_logical_offset,
                       extent_raw, sizeof(extent_raw), error)) {
            overflow_free(&overflow); return -1;
        }
    }
    overflow_free(&overflow);
    return 0;
}

static int rewrite_allocation_bitmap(HfsPlusVolume *stage, const uint8_t *final_used, char **error) {
    uint64_t bytes = ((uint64_t)stage->total_blocks + 7U) / 8U;
    unsigned char *bitmap = calloc((size_t)bytes, 1U);
    if (!bitmap) { hfsplus_set_error(error, "out of memory rebuilding HFS+ allocation bitmap"); return -1; }
    for (uint32_t block = 0; block < stage->total_blocks; ++block) {
        if (ld_bitmap_get(final_used, block))
            bitmap[block / 8U] |= (unsigned char)(1U << (7U - (block % 8U)));
    }
    if (fork_write(stage, &stage->allocation_fork, 0, bitmap, (size_t)bytes, error)) { free(bitmap); return -1; }
    free(bitmap);
    size_t map_bytes = 0U;
    if (!ld_bitmap_size(stage->total_blocks, &map_bytes)) {
        hfsplus_set_error(error, "HFS+ packed allocation-map size overflow");
        return -1;
    }
    memcpy(stage->used_map, final_used, map_bytes);
    return 0;
}

static int move_fork(const HfsPlusVolume *source, HfsPlusVolume *stage,
                     uint32_t file_id, uint8_t fork_type,
                     const HfsPlusFork *source_fork, HfsPlusFork *stage_fork,
                     uint32_t destination, char **error) {
    uint32_t block_index = 0;
    unsigned char *buffer = malloc(source->block_size);
    if (!buffer) { hfsplus_set_error(error, "out of memory relocating HFS+ fork"); return -1; }
    uint64_t described = 0;
    for (size_t i = 0; i < source_fork->extent_count && described < source_fork->total_blocks; ++i) {
        HfsPlusExtent e = source_fork->extents[i];
        for (uint32_t j = 0; j < e.count && block_index < source_fork->total_blocks; ++j, ++block_index) {
            uint64_t from = (uint64_t)(e.start + j) * source->block_size;
            uint64_t to = (uint64_t)(destination + block_index) * stage->block_size;
            if (read_exact(source->fd, from, buffer, source->block_size, error) ||
                write_exact(stage->fd, to, buffer, stage->block_size, error)) {
                free(buffer); return -1;
            }
        }
        described += e.count;
    }
    free(buffer);
    if (source_fork->uses_overflow)
        return rewrite_overflow_fork(stage, file_id, fork_type, stage_fork, destination, error);
    return rewrite_inline_fork(stage, stage_fork, destination, error);
}

int hfsplus_build_stage(const char *source_path, const char *stage_path, bool growth,
                        unsigned gp, bool live, uint64_t *commit_bytes, char **error) {
    HfsPlusVolume source;
    if (hfsplus_scan(source_path, false, &source, error)) return -1;
    if (mutation_preflight(&source, growth, gp, error)) { hfsplus_close(&source); return -1; }
    (void)unlink(stage_path);
    int stage_flags = O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
    stage_flags |= O_NOFOLLOW;
#endif
    int out = open(stage_path, stage_flags, 0600);
    if (out < 0) { hfsplus_set_error(error, "cannot create HFS+ working image: %s", strerror(errno)); hfsplus_close(&source); return -1; }
    if (ftruncate(out, (off_t)source.bytes) || copy_allocated(&source, out, error)) {
        if (!*error) hfsplus_set_error(error, "cannot initialise HFS+ sparse working image: %s", strerror(errno));
        close(out); hfsplus_close(&source); return -1;
    }
    close(out);
    HfsPlusVolume stage;
    if (hfsplus_scan(stage_path, true, &stage, error)) { hfsplus_close(&source); return -1; }
    uint8_t *claimed = ld_bitmap_calloc(stage.total_blocks);
    uint8_t *final_used = ld_bitmap_calloc(stage.total_blocks);
    size_t allocation_map_bytes = 0U;
    if (!claimed || !final_used ||
        !ld_bitmap_size(stage.total_blocks, &allocation_map_bytes)) {
        free(claimed); free(final_used); hfsplus_close(&stage); hfsplus_close(&source);
        hfsplus_set_error(error, "out of memory planning HFS+ layout"); return -1;
    }
    memcpy(claimed, source.used_map, allocation_map_bytes);
    memcpy(final_used, source.used_map, allocation_map_bytes);
    for (size_t i = 0; i < source.files.count; ++i) {
        HfsPlusFile *file = &source.files.items[i];
        HfsPlusFork *forks[2] = {&file->data_fork, &file->resource_fork};
        for (size_t k = 0; k < 2U; ++k) {
            HfsPlusFork *fork = forks[k];
            if (!fork->total_blocks || fork_is_protected(&source, fork, file->file_id) ||
                !fork_rewritable(fork)) continue;
            uint64_t described = 0;
            for (size_t e = 0; e < fork->extent_count && described < fork->total_blocks; ++e) {
                HfsPlusExtent ex = fork->extents[e];
                for (uint32_t b = 0; b < ex.count && described < fork->total_blocks; ++b, ++described) {
                    ld_bitmap_set(claimed, (uint64_t)ex.start + b, false);
                    ld_bitmap_set(final_used, (uint64_t)ex.start + b, false);
                }
            }
        }
    }
    uint64_t relocated = 0;
    size_t sequence = 0;
    for (size_t i = 0; i < source.files.count; ++i) {
        HfsPlusFile *before = &source.files.items[i];
        HfsPlusFile *after = find_file(&stage, before->file_id);
        if (!after) { hfsplus_set_error(error, "HFS+ staged catalog lost file %u", before->file_id); goto fail; }
        HfsPlusFork *bf[2] = {&before->data_fork, &before->resource_fork};
        HfsPlusFork *af[2] = {&after->data_fork, &after->resource_fork};
        for (size_t k = 0; k < 2U; ++k) {
            if (!bf[k]->total_blocks) continue;
            if (fork_is_protected(&source, bf[k], before->file_id)) continue;
            if (!fork_rewritable(bf[k])) {
                hfsplus_set_error(error, "HFS+ file %u fork is not safely rewritable", before->file_id);
                goto fail;
            }
            uint32_t reserve = growth ? (bf[k]->total_blocks * gp + 99U) / 100U : 0U;
            uint32_t destination = 0;
            if (choose_run(claimed, source.total_blocks, bf[k]->total_blocks, reserve, &destination)) {
                hfsplus_set_error(error, "HFS+ layout cannot place file %u fork contiguously with required reserve", before->file_id);
                goto fail;
            }
            if (move_fork(&source, &stage, before->file_id,
                          k == 0U ? HFS_FORK_DATA : HFS_FORK_RESOURCE,
                          bf[k], af[k], destination, error)) goto fail;
            for (uint32_t b = 0; b < bf[k]->total_blocks; ++b) ld_bitmap_set(final_used, (uint64_t)destination + b, true);
            relocated += bf[k]->total_blocks;
            if (live) {
                printf("@@LIVE_RANGES {\"ranges\":[[%u,%u,1]],\"sequence\":%zu}\n",
                       destination, destination + bf[k]->total_blocks, ++sequence);
                fflush(stdout);
            }
        }
    }
    if (rewrite_allocation_bitmap(&stage, final_used, error) || ld_sync_fd(stage.fd)) goto fail;
    free(claimed); free(final_used);
    hfsplus_close(&stage);
    HfsPlusVolume verified;
    if (hfsplus_scan(stage_path, false, &verified, error)) { hfsplus_close(&source); return -1; }
    if (compare_payloads(&source, &verified, error)) { hfsplus_close(&verified); hfsplus_close(&source); return -1; }
    if (commit_bytes) {
        uint64_t total = 0;
        for (uint32_t b = 0; b < verified.total_blocks; ++b) if (ld_bitmap_get(verified.used_map, b)) total += verified.block_size;
        *commit_bytes = total;
    }
    printf("HFS+ native C layout: relocated %" PRIu64 " allocation blocks; special files and B-tree topology remained fixed.\n", relocated);
    hfsplus_close(&verified); hfsplus_close(&source);
    return 0;
fail:
    free(claimed); free(final_used); hfsplus_close(&stage); hfsplus_close(&source); return -1;
}

static int reserve_is_free(const HfsPlusVolume *volume, const HfsPlusFork *fork,
                           unsigned gp, char **error, uint32_t file_id) {
    if (!fork->total_blocks) return 0;
    uint32_t reserve = (fork->total_blocks * gp + 99U) / 100U;
    if (!reserve) return 0;
    HfsPlusExtent tail = fork->extents[fork->extent_count - 1U];
    uint32_t end = tail.start + tail.count;
    for (uint32_t r = 0; r < reserve; ++r) {
        if (end + r >= volume->total_blocks || ld_bitmap_get(volume->used_map, (uint64_t)end + r)) {
            hfsplus_set_error(error, "HFS+ file %u fork does not have its required 10 percent growth reserve", file_id);
            return -1;
        }
    }
    return 0;
}

int hfsplus_verify_layout(const char *path, bool growth, unsigned gp, char **error) {
    HfsPlusVolume volume;
    if (hfsplus_scan(path, false, &volume, error)) return -1;
    if (mutation_preflight(&volume, growth, gp, error)) { hfsplus_close(&volume); return -1; }
    for (size_t i = 0; i < volume.files.count; ++i) {
        HfsPlusFile *file = &volume.files.items[i];
        HfsPlusFork *forks[2] = {&file->data_fork, &file->resource_fork};
        for (size_t k = 0; k < 2U; ++k) {
            bool protected_fork = fork_is_protected(&volume, forks[k], file->file_id);
            if (!protected_fork && fork_fragments(forks[k]) > 1U) {
                hfsplus_set_error(error, "HFS+ file %u remains fragmented", file->file_id);
                hfsplus_close(&volume); return -1;
            }
            if (growth && !protected_fork && reserve_is_free(&volume, forks[k], gp, error, file->file_id)) {
                hfsplus_close(&volume); return -1;
            }
            uint64_t described = 0;
            for (size_t e = 0; e < forks[k]->extent_count && described < forks[k]->total_blocks; ++e) {
                HfsPlusExtent ex = forks[k]->extents[e];
                for (uint32_t b = 0; b < ex.count && described < forks[k]->total_blocks; ++b, ++described) {
                    if (!ld_bitmap_get(volume.used_map, (uint64_t)ex.start + b)) {
                        hfsplus_set_error(error, "HFS+ file %u references a block marked free in the allocation file", file->file_id);
                        hfsplus_close(&volume); return -1;
                    }
                }
            }
        }
    }
    hfsplus_close(&volume);
    return 0;
}

int hfsplus_commit_stage(const char *stage_path, const char *target_path,
                         uint64_t *written, char **error) {
    HfsPlusVolume stage;
    if (hfsplus_scan(stage_path, false, &stage, error)) return -1;
    int target = ld_device_open_verified_fd(target_path, true, NULL, 0U);
    if (target < 0) { hfsplus_set_error(error, "cannot open HFS+ source for commit: %s", strerror(errno)); hfsplus_close(&stage); return -1; }
    unsigned char *buffer = malloc(HFS_IO_CHUNK);
    if (!buffer) { hfsplus_set_error(error, "out of memory committing HFS+ stage"); close(target); hfsplus_close(&stage); return -1; }
    uint64_t total_written = 0;
    uint32_t block = 0;
    while (block < stage.total_blocks) {
        if (!ld_bitmap_get(stage.used_map, block)) { ++block; continue; }
        uint32_t start = block++;
        while (block < stage.total_blocks && ld_bitmap_get(stage.used_map, block)) ++block;
        uint64_t offset = (uint64_t)start * stage.block_size;
        uint64_t remain = (uint64_t)(block - start) * stage.block_size;
        uint64_t cursor = 0;
        while (cursor < remain) {
            size_t take = HFS_IO_CHUNK;
            if ((uint64_t)take > remain - cursor) take = (size_t)(remain - cursor);
            if (read_exact(stage.fd, offset + cursor, buffer, take, error) ||
                write_exact(target, offset + cursor, buffer, take, error)) {
                free(buffer); close(target); hfsplus_close(&stage); return -1;
            }
            cursor += take; total_written += take;
        }
    }
    if (read_exact(stage.fd, 0, buffer, 1536U, error) || write_exact(target, 0, buffer, 1536U, error)) {
        free(buffer); close(target); hfsplus_close(&stage); return -1;
    }
    if (stage.bytes >= 1024U &&
        (read_exact(stage.fd, stage.bytes - 1024U, buffer, 1024U, error) ||
         write_exact(target, stage.bytes - 1024U, buffer, 1024U, error))) {
        free(buffer); close(target); hfsplus_close(&stage); return -1;
    }
    if (fsync(target)) {
        hfsplus_set_error(error, "cannot sync HFS+ source: %s", strerror(errno));
        free(buffer); close(target); hfsplus_close(&stage); return -1;
    }
    free(buffer); close(target); hfsplus_close(&stage);
    if (written) *written = total_written;
    return 0;
}
