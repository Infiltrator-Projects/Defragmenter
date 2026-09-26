// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Defragmenter - classic Macintosh HFS bounded native writer
 * Author: Shannon Smith
 *
 * The read-only parser remains authoritative in analyser.c.  This worker
 * includes that parser directly so allocation analysis and mutation share one
 * HFS decoder.  Mutation is deliberately fail-closed to regular-file data and
 * resource forks whose complete extent records fit inline in the catalog.
 * Special files and B-tree topology remain fixed.
 */

#define main hfs_readonly_main
#include "analyser.c"
#undef main

#include "version.h"
#include "ld_device.h"
#include "ld_io.h"
#include "ld_path.h"
#include "ld_protocol.h"
#include "ld_runtime.h"
#include "ld_stop.h"
#include "infiltratr/config.h"
#include "infiltratr/core.h"
#include "infiltratr/posix.h"

#include <openssl/evp.h>
#include <stdarg.h>
#include <sys/file.h>
#include <sys/sysmacros.h>

#define HFS_PROG "hfs_analyser"
#define HFS_JOURNAL_MAGIC "LINUX-DEFRAGGER-HFS-JOURNAL-1"
#define HFS_STAGE_SUFFIX ".hfs-stage"
#define HFS_IO_CHUNK (1024U * 1024U)
#define HFS_STOPPED 130
#define HFS_ATTR_HARDWARE_LOCK UINT16_C(0x0080)
#define HFS_ATTR_UNMOUNTED UINT16_C(0x0100)
#define HFS_ATTR_INCONSISTENT UINT16_C(0x0800)
#define HFS_ATTR_SOFTWARE_LOCK UINT16_C(0x8000)

typedef struct {
    uint32_t blocks;
    hfs_extent extents[3];
    size_t extent_count;
    uint64_t catalog_extent_offset;
} hfs_writable_fork;

typedef struct {
    uint32_t file_id;
    hfs_writable_fork data_fork;
    hfs_writable_fork resource_fork;
} hfs_writable_file;

typedef struct {
    hfs_volume volume;
    uint64_t bytes;
    uint16_t attributes;
    uint8_t *used_map;
    hfs_writable_file *files;
    size_t file_count;
    size_t file_capacity;
} hfs_writer_volume;

typedef struct {
    char *device;
    char *target_identity;
    char *stage;
    char operation[24];
    char phase[24];
    char volume_token[65];
    char source_sha256[65];
    char stage_sha256[65];
    uint64_t physical_bytes;
    uint32_t block_size;
    uint32_t total_blocks;
    uint16_t allocation_start;
} hfs_journal;

static void hfs_set_error(char **error, const char *format, ...)
{
    if (error == NULL)
        return;
    free(*error);
    *error = NULL;
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    const int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed >= 0) {
        *error = malloc((size_t)needed + 1U);
        if (*error != NULL)
            (void)vsnprintf(*error, (size_t)needed + 1U, format, args);
    }
    va_end(args);
}

static void writer_volume_close(hfs_writer_volume *writer)
{
    if (writer == NULL)
        return;
    free(writer->used_map);
    free(writer->files);
    writer->used_map = NULL;
    writer->files = NULL;
    writer->file_count = 0U;
    writer->file_capacity = 0U;
    hfs_close_volume(&writer->volume);
}

static int fork_contains_block(const hfs_fork_map *fork, uint16_t block)
{
    for (size_t i = 0U; i < fork->extent_count; ++i) {
        const uint32_t start = fork->extents[i].start;
        const uint32_t end = start + fork->extents[i].count;
        if ((uint32_t)block >= start && (uint32_t)block < end)
            return 1;
    }
    return 0;
}

static int validate_special_fork_used(const hfs_writer_volume *writer,
                                      const hfs_fork_map *fork,
                                      const char *name, char **error)
{
    uint64_t described = 0U;
    for (size_t i = 0U; i < fork->extent_count && described < fork->size_bytes; ++i) {
        const hfs_extent extent = fork->extents[i];
        if ((uint32_t)extent.start + extent.count >
            writer->volume.total_allocation_blocks) {
            hfs_set_error(error, "HFS %s extent exceeds allocation geometry", name);
            return -1;
        }
        for (uint32_t b = 0U; b < extent.count; ++b) {
            if (!ld_bitmap_get(writer->used_map, (uint32_t)extent.start + b)) {
                hfs_set_error(error,
                              "HFS %s references allocation block %u marked free",
                              name, (unsigned)((uint32_t)extent.start + b));
                return -1;
            }
        }
        described += (uint64_t)extent.count * writer->volume.allocation_block_size;
    }
    if (described < fork->size_bytes) {
        hfs_set_error(error, "HFS %s extent map does not cover its physical length", name);
        return -1;
    }
    return 0;
}

static int parse_inline_fork(const hfs_writer_volume *writer,
                             uint32_t file_id, const char *fork_name,
                             uint32_t physical_bytes, const uint8_t *raw,
                             uint64_t logical_offset,
                             hfs_writable_fork *fork, char **error)
{
    memset(fork, 0, sizeof(*fork));
    fork->catalog_extent_offset = logical_offset;
    if (physical_bytes == 0U) {
        for (size_t i = 0U; i < 3U; ++i) {
            if (infiltratr_load_be16(raw + i * 4U + 2U) != 0U) {
                hfs_set_error(error,
                              "HFS file %u %s fork has extents with zero physical length",
                              file_id, fork_name);
                return -1;
            }
        }
        return 0;
    }
    if (physical_bytes % writer->volume.allocation_block_size != 0U) {
        hfs_set_error(error, "HFS file %u %s fork length is not allocation-block aligned",
                      file_id, fork_name);
        return -1;
    }
    const uint32_t required = physical_bytes / writer->volume.allocation_block_size;
    uint32_t described = 0U;
    for (size_t i = 0U; i < 3U && described < required; ++i) {
        hfs_extent extent = {
            infiltratr_load_be16(raw + i * 4U),
            infiltratr_load_be16(raw + i * 4U + 2U),
        };
        if (extent.count == 0U) {
            hfs_set_error(error,
                          "HFS file %u %s fork requires an unsupported overflow extent record",
                          file_id, fork_name);
            return -1;
        }
        if ((uint32_t)extent.start + extent.count >
            writer->volume.total_allocation_blocks ||
            described + extent.count > required) {
            hfs_set_error(error, "HFS file %u %s fork extent is outside its validated length",
                          file_id, fork_name);
            return -1;
        }
        fork->extents[fork->extent_count++] = extent;
        described += extent.count;
    }
    if (described != required) {
        hfs_set_error(error,
                      "HFS file %u %s fork uses the Extents Overflow B-tree; "
                      "the bounded writer supports complete inline fork maps only",
                      file_id, fork_name);
        return -1;
    }
    fork->blocks = required;
    return 0;
}

static int append_file(hfs_writer_volume *writer, const hfs_writable_file *file,
                       char **error)
{
    for (size_t i = 0U; i < writer->file_count; ++i) {
        if (writer->files[i].file_id == file->file_id) {
            hfs_set_error(error, "HFS catalog repeats file CNID %u", file->file_id);
            return -1;
        }
    }
    if (!infiltratr_array_reserve((void **)&writer->files, &writer->file_capacity,
                                  sizeof(*writer->files), writer->file_count + 1U,
                                  64U)) {
        hfs_set_error(error, "out of memory cataloguing HFS files");
        return -1;
    }
    writer->files[writer->file_count++] = *file;
    return 0;
}

static int writer_scan_catalog(hfs_writer_volume *writer, char **error)
{
    if (extend_special_file(&writer->volume, &writer->volume.catalog_file,
                            HFS_CATALOG_FILE_ID, HFS_DATA_FORK) != 0) {
        hfs_set_error(error, "HFS Catalog B-tree extent map is incomplete");
        return -1;
    }
    hfs_btree_header header;
    if (parse_btree_header(&writer->volume, &writer->volume.catalog_file,
                           &header) != 0) {
        hfs_set_error(error, "HFS Catalog B-tree header is invalid");
        return -1;
    }

    uint32_t node_number = header.first_leaf;
    uint32_t visited = 0U;
    while (node_number != 0U) {
        uint8_t node[HFS_LOGICAL_BLOCK_SIZE];
        if (++visited > header.total_nodes ||
            read_btree_node(&writer->volume, &writer->volume.catalog_file,
                            node_number, node) != 0 ||
            (int8_t)node[8] != HFS_BTREE_LEAF_NODE) {
            hfs_set_error(error, "HFS Catalog leaf chain is invalid");
            return -1;
        }
        const uint32_t next = infiltratr_load_be32(node);
        const uint16_t records = infiltratr_load_be16(node + 10U);
        if (records > HFS_MAX_BTREE_RECORDS) {
            hfs_set_error(error, "HFS Catalog leaf contains too many records");
            return -1;
        }
        for (uint16_t index = 0U; index < records; ++index) {
            uint16_t start = 0U, end = 0U;
            if (node_record_bounds(node, records, index, &start, &end) != 0) {
                hfs_set_error(error, "HFS Catalog record bounds are invalid");
                return -1;
            }
            const uint8_t *record = node + start;
            const size_t length = (size_t)(end - start);
            const size_t key_skip = record_key_skip(record, length);
            if (key_skip == 0U || key_skip + 2U > length) {
                hfs_set_error(error, "HFS Catalog record key is invalid");
                return -1;
            }
            const uint8_t *data = record + key_skip;
            if (data[0] != HFS_FILE_RECORD)
                continue;
            if (key_skip + 102U > length) {
                hfs_set_error(error, "HFS Catalog file record is truncated");
                return -1;
            }
            hfs_writable_file file;
            memset(&file, 0, sizeof(file));
            file.file_id = infiltratr_load_be32(data + 20U);
            const uint32_t data_physical = infiltratr_load_be32(data + 30U);
            const uint32_t resource_physical = infiltratr_load_be32(data + 40U);
            const uint64_t node_offset =
                (uint64_t)node_number * HFS_LOGICAL_BLOCK_SIZE;
            if (parse_inline_fork(
                    writer, file.file_id, "data", data_physical, data + 74U,
                    node_offset + start + key_skip + 74U,
                    &file.data_fork, error) != 0 ||
                parse_inline_fork(
                    writer, file.file_id, "resource", resource_physical, data + 86U,
                    node_offset + start + key_skip + 86U,
                    &file.resource_fork, error) != 0 ||
                append_file(writer, &file, error) != 0)
                return -1;
        }
        if (node_number == header.last_leaf && next != 0U) {
            hfs_set_error(error, "HFS Catalog leaf chain continues after lastLeafNode");
            return -1;
        }
        node_number = next;
    }
    if (writer->file_count != writer->volume.header_files) {
        hfs_set_error(error,
                      "HFS catalog file count (%zu) disagrees with MDB count (%u)",
                      writer->file_count, writer->volume.header_files);
        return -1;
    }
    return 0;
}

static int validate_file_allocations(hfs_writer_volume *writer, char **error)
{
    uint8_t *seen = ld_bitmap_calloc(writer->volume.total_allocation_blocks);
    if (seen == NULL) {
        hfs_set_error(error, "out of memory validating HFS file allocations");
        return -1;
    }
    for (size_t i = 0U; i < writer->file_count; ++i) {
        hfs_writable_fork *forks[2] = {
            &writer->files[i].data_fork, &writer->files[i].resource_fork
        };
        for (size_t k = 0U; k < 2U; ++k) {
            hfs_writable_fork *fork = forks[k];
            for (size_t e = 0U; e < fork->extent_count; ++e) {
                hfs_extent extent = fork->extents[e];
                for (uint32_t b = 0U; b < extent.count; ++b) {
                    const uint16_t block = (uint16_t)((uint32_t)extent.start + b);
                    if (!ld_bitmap_get(writer->used_map, block) || ld_bitmap_get(seen, block) ||
                        fork_contains_block(&writer->volume.extents_file, block) ||
                        fork_contains_block(&writer->volume.catalog_file, block)) {
                        free(seen);
                        hfs_set_error(error,
                                      "HFS file %u has an overlapping, free or protected allocation block %u",
                                      writer->files[i].file_id, (unsigned)block);
                        return -1;
                    }
                    ld_bitmap_set(seen, block, true);
                }
            }
        }
    }
    free(seen);
    return 0;
}

static int writer_volume_open(const char *path, hfs_writer_volume *writer,
                              char **error)
{
    memset(writer, 0, sizeof(*writer));
    writer->volume.fd = -1;
    if (hfs_open_volume(path, &writer->volume) != 0) {
        hfs_set_error(error, "invalid or unsupported classic HFS MDB");
        return -1;
    }
    if (ld_fd_size_bytes(writer->volume.fd, &writer->bytes) != 0 ||
        writer->bytes < 1536U) {
        hfs_set_error(error, "cannot determine classic HFS target capacity");
        writer_volume_close(writer);
        return -1;
    }
    const uint64_t allocation_end =
        (uint64_t)writer->volume.allocation_start_block * HFS_LOGICAL_BLOCK_SIZE +
        (uint64_t)writer->volume.total_allocation_blocks *
            writer->volume.allocation_block_size;
    if (allocation_end > writer->bytes) {
        hfs_set_error(error, "HFS allocation geometry exceeds target capacity");
        writer_volume_close(writer);
        return -1;
    }

    uint8_t attr_raw[2];
    uint8_t embed_raw[2];
    if (read_exact(writer->volume.fd, HFS_MDB_OFFSET + 10U,
                   attr_raw, sizeof(attr_raw)) != 0 ||
        read_exact(writer->volume.fd, HFS_MDB_OFFSET + 124U,
                   embed_raw, sizeof(embed_raw)) != 0) {
        hfs_set_error(error, "cannot read HFS mutation-safety MDB fields");
        writer_volume_close(writer);
        return -1;
    }
    writer->attributes = infiltratr_load_be16(attr_raw);
    const uint16_t embed_signature = infiltratr_load_be16(embed_raw);
    if ((writer->attributes & HFS_ATTR_UNMOUNTED) == 0U ||
        (writer->attributes & HFS_ATTR_INCONSISTENT) != 0U) {
        hfs_set_error(error,
                      "HFS volume is not recorded as cleanly unmounted; mutation is refused");
        writer_volume_close(writer);
        return -1;
    }
    if ((writer->attributes &
         (HFS_ATTR_HARDWARE_LOCK | HFS_ATTR_SOFTWARE_LOCK)) != 0U) {
        hfs_set_error(error, "HFS volume is marked write-protected");
        writer_volume_close(writer);
        return -1;
    }
    if (embed_signature == UINT16_C(0x482b) ||
        embed_signature == UINT16_C(0x4858)) {
        hfs_set_error(error,
                      "HFS wrapper contains an embedded HFS+/HFSX volume; "
                      "classic-HFS mutation is refused");
        writer_volume_close(writer);
        return -1;
    }

    uint8_t *bitmap = NULL;
    size_t bitmap_length = 0U;
    if (allocation_bitmap_read(&writer->volume, &bitmap, &bitmap_length) != 0) {
        hfs_set_error(error, "cannot read HFS allocation bitmap");
        writer_volume_close(writer);
        return -1;
    }
    writer->used_map = ld_bitmap_calloc(writer->volume.total_allocation_blocks);
    if (writer->used_map == NULL) {
        free(bitmap);
        hfs_set_error(error, "out of memory reading HFS allocation map");
        writer_volume_close(writer);
        return -1;
    }
    uint32_t free_blocks = 0U;
    for (uint32_t block = 0U;
         block < writer->volume.total_allocation_blocks; ++block) {
        const bool used = allocation_block_used(bitmap, (uint16_t)block);
        ld_bitmap_set(writer->used_map, block, used);
        if (!used)
            ++free_blocks;
    }
    free(bitmap);
    (void)bitmap_length;
    if (free_blocks != writer->volume.header_free_blocks) {
        hfs_set_error(error,
                      "HFS allocation bitmap free count (%u) disagrees with MDB (%u)",
                      free_blocks, (unsigned)writer->volume.header_free_blocks);
        writer_volume_close(writer);
        return -1;
    }

    if (scan_extents_overflow(&writer->volume) != 0) {
        hfs_set_error(error, "invalid or unsupported HFS Extents Overflow B-tree");
        writer_volume_close(writer);
        return -1;
    }
    if (writer_scan_catalog(writer, error) != 0 ||
        validate_special_fork_used(writer, &writer->volume.extents_file,
                                   "Extents Overflow file", error) != 0 ||
        validate_special_fork_used(writer, &writer->volume.catalog_file,
                                   "Catalog file", error) != 0 ||
        validate_file_allocations(writer, error) != 0) {
        writer_volume_close(writer);
        return -1;
    }
    return 0;
}

static hfs_writable_file *find_writer_file(hfs_writer_volume *writer,
                                           uint32_t file_id)
{
    for (size_t i = 0U; i < writer->file_count; ++i)
        if (writer->files[i].file_id == file_id)
            return &writer->files[i];
    return NULL;
}

static uint64_t allocation_offset(const hfs_writer_volume *writer,
                                  uint32_t block)
{
    return (uint64_t)writer->volume.allocation_start_block *
               HFS_LOGICAL_BLOCK_SIZE +
           (uint64_t)block * writer->volume.allocation_block_size;
}

static int fork_block_at(const hfs_writable_fork *fork, uint32_t logical,
                         uint32_t *physical)
{
    uint32_t cursor = 0U;
    for (size_t i = 0U; i < fork->extent_count; ++i) {
        if (logical < cursor + fork->extents[i].count) {
            *physical = (uint32_t)fork->extents[i].start + logical - cursor;
            return 0;
        }
        cursor += fork->extents[i].count;
    }
    return -1;
}

static int write_catalog_bytes(const hfs_writer_volume *writer, int fd,
                               uint64_t logical_offset, const void *buffer,
                               size_t length, char **error)
{
    const hfs_fork_map *catalog = &writer->volume.catalog_file;
    const uint8_t *source = buffer;
    size_t done = 0U;
    uint64_t logical = 0U;
    if (logical_offset > catalog->size_bytes ||
        (uint64_t)length > catalog->size_bytes - logical_offset) {
        hfs_set_error(error, "HFS Catalog rewrite exceeds catalog-file length");
        return -1;
    }
    for (size_t i = 0U; i < catalog->extent_count && done < length; ++i) {
        const uint64_t span = extent_capacity_bytes(&writer->volume,
                                                     &catalog->extents[i]);
        const uint64_t end = logical + span;
        if (logical_offset + done >= end) {
            logical = end;
            continue;
        }
        const uint64_t within = logical_offset + done - logical;
        uint64_t available = span - within;
        size_t take = length - done;
        if ((uint64_t)take > available)
            take = (size_t)available;
        const uint64_t physical =
            extent_physical_offset(&writer->volume, &catalog->extents[i]) + within;
        if (ld_pwrite_full(fd, source + done, take, physical) !=
            (ssize_t)take) {
            hfs_set_error(error, "cannot rewrite HFS Catalog extent descriptor");
            return -1;
        }
        done += take;
        logical = end;
    }
    if (done != length) {
        hfs_set_error(error, "HFS Catalog rewrite could not map all bytes");
        return -1;
    }
    return 0;
}

static int rewrite_inline_fork(const hfs_writer_volume *writer, int stage_fd,
                               const hfs_writable_fork *fork,
                               uint32_t destination, char **error)
{
    if (fork->blocks == 0U)
        return 0;
    if (fork->blocks > UINT16_MAX || destination > UINT16_MAX) {
        hfs_set_error(error, "HFS contiguous fork exceeds 16-bit extent geometry");
        return -1;
    }
    uint8_t raw[12] = {0};
    infiltratr_store_be16(raw, (uint16_t)destination);
    infiltratr_store_be16(raw + 2U, (uint16_t)fork->blocks);
    return write_catalog_bytes(writer, stage_fd, fork->catalog_extent_offset,
                               raw, sizeof(raw), error);
}

static int copy_fork(const hfs_writer_volume *source, int stage_fd,
                     const hfs_writable_fork *fork, uint32_t destination,
                     char **error)
{
    if (fork->blocks == 0U)
        return 0;
    uint8_t *buffer = malloc(source->volume.allocation_block_size);
    if (buffer == NULL) {
        hfs_set_error(error, "out of memory relocating HFS fork");
        return -1;
    }
    for (uint32_t logical = 0U; logical < fork->blocks; ++logical) {
        uint32_t physical = 0U;
        if (fork_block_at(fork, logical, &physical) != 0 ||
            ld_pread_full(source->volume.fd, buffer,
                          source->volume.allocation_block_size,
                          allocation_offset(source, physical)) !=
                (ssize_t)source->volume.allocation_block_size ||
            ld_pwrite_full(stage_fd, buffer,
                           source->volume.allocation_block_size,
                           allocation_offset(source, destination + logical)) !=
                (ssize_t)source->volume.allocation_block_size) {
            free(buffer);
            hfs_set_error(error, "short I/O relocating HFS fork data");
            return -1;
        }
    }
    free(buffer);
    return 0;
}

static int choose_run(uint8_t *claimed, uint32_t total, uint32_t blocks,
                      uint32_t reserve, uint32_t *destination)
{
    if (blocks == 0U || blocks > total || reserve > total - blocks)
        return -1;
    const uint32_t span = blocks + reserve;
    for (uint32_t start = 0U; start <= total - span; ++start) {
        uint32_t offset = 0U;
        while (offset < span && !ld_bitmap_get(claimed, (uint64_t)start + offset))
            ++offset;
        if (offset != span) {
            start += offset;
            continue;
        }
        *destination = start;
        for (uint32_t offset = 0U; offset < span; ++offset)
            ld_bitmap_set(claimed, (uint64_t)start + offset, true);
        return 0;
    }
    return -1;
}

static int rewrite_bitmap(const hfs_writer_volume *writer, int stage_fd,
                          const uint8_t *final_used, char **error)
{
    const size_t length =
        ((size_t)writer->volume.total_allocation_blocks + 7U) / 8U;
    uint8_t *bitmap = calloc(length, 1U);
    if (bitmap == NULL) {
        hfs_set_error(error, "out of memory rebuilding HFS allocation bitmap");
        return -1;
    }
    for (uint32_t block = 0U;
         block < writer->volume.total_allocation_blocks; ++block) {
        if (ld_bitmap_get(final_used, block))
            bitmap[block >> 3U] |=
                (uint8_t)(UINT8_C(0x80) >> (block & 7U));
    }
    const uint64_t offset =
        (uint64_t)writer->volume.bitmap_start_block * HFS_LOGICAL_BLOCK_SIZE;
    const int rc =
        ld_pwrite_full(stage_fd, bitmap, length, offset) == (ssize_t)length
            ? 0 : -1;
    free(bitmap);
    if (rc != 0)
        hfs_set_error(error, "cannot rewrite HFS allocation bitmap");
    return rc;
}

static int compare_fork_payload(const hfs_writer_volume *before,
                                const hfs_writable_fork *a,
                                const hfs_writer_volume *after,
                                const hfs_writable_fork *b,
                                uint32_t file_id, const char *name,
                                char **error)
{
    if (a->blocks != b->blocks) {
        hfs_set_error(error, "HFS file %u %s fork block count changed",
                      file_id, name);
        return -1;
    }
    if (a->blocks == 0U)
        return 0;
    if (before->volume.allocation_block_size != after->volume.allocation_block_size) {
        hfs_set_error(error, "HFS allocation block size changed in stage");
        return -1;
    }
    const size_t block_size = before->volume.allocation_block_size;
    uint8_t *left = malloc(block_size);
    uint8_t *right = malloc(block_size);
    if (left == NULL || right == NULL) {
        free(left);
        free(right);
        hfs_set_error(error, "out of memory verifying HFS payload");
        return -1;
    }
    for (uint32_t logical = 0U; logical < a->blocks; ++logical) {
        uint32_t pa = 0U, pb = 0U;
        if (fork_block_at(a, logical, &pa) != 0 ||
            fork_block_at(b, logical, &pb) != 0 ||
            ld_pread_full(before->volume.fd, left, block_size,
                          allocation_offset(before, pa)) != (ssize_t)block_size ||
            ld_pread_full(after->volume.fd, right, block_size,
                          allocation_offset(after, pb)) != (ssize_t)block_size ||
            memcmp(left, right, block_size) != 0) {
            free(left);
            free(right);
            hfs_set_error(error, "HFS file %u %s fork payload changed during staging",
                          file_id, name);
            return -1;
        }
    }
    free(left);
    free(right);
    return 0;
}

static int compare_payloads(hfs_writer_volume *before,
                            hfs_writer_volume *after, char **error)
{
    if (before->file_count != after->file_count) {
        hfs_set_error(error, "HFS staged catalog file count changed");
        return -1;
    }
    for (size_t i = 0U; i < before->file_count; ++i) {
        hfs_writable_file *other =
            find_writer_file(after, before->files[i].file_id);
        if (other == NULL ||
            compare_fork_payload(before, &before->files[i].data_fork,
                                 after, &other->data_fork,
                                 before->files[i].file_id, "data", error) != 0 ||
            compare_fork_payload(before, &before->files[i].resource_fork,
                                 after, &other->resource_fork,
                                 before->files[i].file_id, "resource", error) != 0)
            return -1;
    }
    return 0;
}

static int copy_full_image(int source_fd, int target_fd, uint64_t bytes,
                           char **error)
{
    uint8_t *buffer = malloc(HFS_IO_CHUNK);
    if (buffer == NULL) {
        hfs_set_error(error, "out of memory staging HFS image");
        return -1;
    }
    int rc = 0;
    for (uint64_t offset = 0U; offset < bytes;) {
        if (ld_stop_requested()) {
            hfs_set_error(error, "HFS staging stopped before authoritative writes");
            rc = HFS_STOPPED;
            break;
        }
        const uint64_t remain = bytes - offset;
        const size_t count = remain > HFS_IO_CHUNK
                           ? HFS_IO_CHUNK : (size_t)remain;
        if (ld_pread_full(source_fd, buffer, count, offset) != (ssize_t)count ||
            ld_pwrite_full(target_fd, buffer, count, offset) != (ssize_t)count) {
            hfs_set_error(error, "short I/O copying HFS stage at offset %" PRIu64,
                          offset);
            rc = -1;
            break;
        }
        offset += count;
    }
    free(buffer);
    return rc;
}

static int hfs_build_stage(const char *source_path, const char *stage_path,
                           bool growth, unsigned growth_percent,
                           bool live, uint64_t *commit_bytes, char **error)
{
    if (growth && growth_percent != 10U) {
        hfs_set_error(error, "HFS Growth Defrag requires exactly 10 percent reserve");
        return -1;
    }
    hfs_writer_volume source;
    if (writer_volume_open(source_path, &source, error) != 0)
        return -1;

    (void)unlink(stage_path);
    int flags = O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int stage_fd = open(stage_path, flags, 0600);
    if (stage_fd < 0 || ftruncate(stage_fd, (off_t)source.bytes) != 0) {
        if (stage_fd >= 0)
            (void)close(stage_fd);
        hfs_set_error(error, "cannot create HFS working image: %s",
                      strerror(errno));
        writer_volume_close(&source);
        return -1;
    }
    const int copy_rc =
        copy_full_image(source.volume.fd, stage_fd, source.bytes, error);
    if (copy_rc != 0) {
        (void)close(stage_fd);
        writer_volume_close(&source);
        return copy_rc;
    }

    uint8_t *claimed =
        ld_bitmap_calloc(source.volume.total_allocation_blocks);
    uint8_t *final_used =
        ld_bitmap_calloc(source.volume.total_allocation_blocks);
    size_t allocation_map_bytes = 0U;
    if (claimed == NULL || final_used == NULL ||
        !ld_bitmap_size(source.volume.total_allocation_blocks,
                        &allocation_map_bytes)) {
        free(claimed);
        free(final_used);
        (void)close(stage_fd);
        writer_volume_close(&source);
        hfs_set_error(error, "out of memory planning HFS relocation");
        return -1;
    }
    memcpy(claimed, source.used_map, allocation_map_bytes);
    memcpy(final_used, source.used_map, allocation_map_bytes);

    for (size_t i = 0U; i < source.file_count; ++i) {
        hfs_writable_fork *forks[2] = {
            &source.files[i].data_fork, &source.files[i].resource_fork
        };
        for (size_t k = 0U; k < 2U; ++k) {
            for (size_t e = 0U; e < forks[k]->extent_count; ++e) {
                const hfs_extent extent = forks[k]->extents[e];
                for (uint32_t block = 0U; block < extent.count; ++block) {
                    ld_bitmap_set(claimed, (uint64_t)extent.start + block, false);
                    ld_bitmap_set(final_used, (uint64_t)extent.start + block, false);
                }
            }
        }
    }

    uint64_t relocated = 0U;
    size_t sequence = 0U;
    int rc = 0;
    for (size_t i = 0U; i < source.file_count && rc == 0; ++i) {
        hfs_writable_fork *forks[2] = {
            &source.files[i].data_fork, &source.files[i].resource_fork
        };
        const char *names[2] = {"data", "resource"};
        for (size_t k = 0U; k < 2U; ++k) {
            hfs_writable_fork *fork = forks[k];
            if (fork->blocks == 0U)
                continue;
            const uint32_t reserve = growth
                ? (fork->blocks * growth_percent + 99U) / 100U : 0U;
            uint32_t destination = 0U;
            if (choose_run(claimed, source.volume.total_allocation_blocks,
                           fork->blocks, reserve, &destination) != 0) {
                hfs_set_error(error,
                              "HFS layout cannot place file %u %s fork contiguously with required reserve",
                              source.files[i].file_id, names[k]);
                rc = -1;
                break;
            }
            if (copy_fork(&source, stage_fd, fork, destination, error) != 0 ||
                rewrite_inline_fork(&source, stage_fd, fork,
                                    destination, error) != 0) {
                rc = -1;
                break;
            }
            for (uint32_t block = 0U; block < fork->blocks; ++block)
                ld_bitmap_set(final_used, (uint64_t)destination + block, true);
            relocated += fork->blocks;
            if (live) {
                (void)printf(
                    "@@LIVE_RANGES {\"ranges\":[[%u,%u,1]],\"sequence\":%zu}\n",
                    destination, destination + fork->blocks, ++sequence);
                (void)fflush(stdout);
            }
        }
    }
    if (rc == 0)
        rc = rewrite_bitmap(&source, stage_fd, final_used, error);
    if (rc == 0 && ld_sync_fd(stage_fd) != 0) {
        hfs_set_error(error, "cannot sync HFS working image: %s", strerror(errno));
        rc = -1;
    }
    free(claimed);
    free(final_used);
    (void)close(stage_fd);

    if (rc == 0) {
        hfs_writer_volume verified;
        if (writer_volume_open(stage_path, &verified, error) != 0)
            rc = -1;
        else {
            rc = compare_payloads(&source, &verified, error);
            writer_volume_close(&verified);
        }
    }
    if (rc == 0 && commit_bytes != NULL)
        *commit_bytes = source.bytes;
    if (rc == 0)
        (void)printf(
            "Classic HFS native C layout: relocated %" PRIu64
            " allocation blocks; special files and B-tree topology remained fixed.\n",
            relocated);
    writer_volume_close(&source);
    return rc;
}

static int hfs_verify_layout(const char *path, bool growth,
                             unsigned growth_percent, char **error)
{
    if (growth && growth_percent != 10U) {
        hfs_set_error(error, "HFS Growth Defrag requires exactly 10 percent reserve");
        return -1;
    }
    hfs_writer_volume writer;
    if (writer_volume_open(path, &writer, error) != 0)
        return -1;
    int rc = 0;
    for (size_t i = 0U; i < writer.file_count && rc == 0; ++i) {
        hfs_writable_fork *forks[2] = {
            &writer.files[i].data_fork, &writer.files[i].resource_fork
        };
        const char *names[2] = {"data", "resource"};
        for (size_t k = 0U; k < 2U; ++k) {
            hfs_writable_fork *fork = forks[k];
            if (fork->blocks == 0U)
                continue;
            if (fork->extent_count != 1U ||
                fork->extents[0].count != fork->blocks) {
                hfs_set_error(error, "HFS file %u %s fork remains fragmented",
                              writer.files[i].file_id, names[k]);
                rc = -1;
                break;
            }
            for (uint32_t block = 0U; block < fork->blocks; ++block) {
                const uint32_t at = (uint32_t)fork->extents[0].start + block;
                if (at >= writer.volume.total_allocation_blocks ||
                    !ld_bitmap_get(writer.used_map, at)) {
                    hfs_set_error(error,
                                  "HFS file %u %s fork references a block marked free",
                                  writer.files[i].file_id, names[k]);
                    rc = -1;
                    break;
                }
            }
            if (rc != 0 || !growth)
                continue;
            const uint32_t reserve =
                (fork->blocks * growth_percent + 99U) / 100U;
            const uint32_t end =
                (uint32_t)fork->extents[0].start + fork->blocks;
            for (uint32_t r = 0U; r < reserve; ++r) {
                if (end + r >= writer.volume.total_allocation_blocks ||
                    ld_bitmap_get(writer.used_map, (uint64_t)end + r)) {
                    hfs_set_error(error,
                                  "HFS file %u %s fork lacks its required 10 percent growth reserve",
                                  writer.files[i].file_id, names[k]);
                    rc = -1;
                    break;
                }
            }
        }
    }
    writer_volume_close(&writer);
    return rc;
}

static bool writer_safe_value(const char *value)
{
    return value != NULL && strchr(value, '\n') == NULL &&
           strchr(value, '\r') == NULL && strchr(value, '=') == NULL;
}

static void writer_unlink(const char *path)
{
    if (path == NULL || *path == '\0')
        return;
    const int failure = infiltratr_unlink_durable(path, true);
    if (failure != 0)
        (void)fprintf(stderr, "%s: warning: cannot durably remove %s: %s\n",
                      HFS_PROG, path, strerror(failure));
}

static void journal_free(hfs_journal *state)
{
    free(state->device);
    free(state->target_identity);
    free(state->stage);
    memset(state, 0, sizeof(*state));
}

static bool journal_write_stream(FILE *file, const void *user_data)
{
    const hfs_journal *state = user_data;
    (void)fprintf(file, "%s\n", HFS_JOURNAL_MAGIC);
    (void)fprintf(file, "device=%s\n", state->device);
    (void)fprintf(file, "target_identity=%s\n", state->target_identity);
    (void)fprintf(file, "stage=%s\n", state->stage);
    (void)fprintf(file, "operation=%s\n", state->operation);
    (void)fprintf(file, "phase=%s\n", state->phase);
    (void)fprintf(file, "volume_token=%s\n", state->volume_token);
    (void)fprintf(file, "source_sha256=%s\n", state->source_sha256);
    (void)fprintf(file, "stage_sha256=%s\n", state->stage_sha256);
    (void)fprintf(file, "physical_bytes=%" PRIu64 "\n", state->physical_bytes);
    (void)fprintf(file, "block_size=%u\n", state->block_size);
    (void)fprintf(file, "total_blocks=%u\n", state->total_blocks);
    (void)fprintf(file, "allocation_start=%u\n",
                  (unsigned)state->allocation_start);
    return !ferror(file);
}

static int journal_save(const char *path, const hfs_journal *state, char **error)
{
    if (!writer_safe_value(state->device) ||
        !writer_safe_value(state->target_identity) ||
        !writer_safe_value(state->stage)) {
        hfs_set_error(error, "HFS transaction paths contain unsupported journal characters");
        return -1;
    }
    char *parent = ld_path_parent_directory(path);
    if (parent == NULL ||
        ld_path_ensure_trusted_directory_tree(parent) != 0) {
        hfs_set_error(error, "cannot create HFS journal directory: %s",
                      strerror(errno));
        free(parent);
        return -1;
    }
    free(parent);
    const int failure = infiltratr_atomic_file_write(
        path, INFILTRATR_ATOMIC_FILE_PRIVATE, journal_write_stream, state);
    if (failure != 0) {
        hfs_set_error(error, "cannot publish HFS recovery journal: %s",
                      strerror(failure));
        return -1;
    }
    return 0;
}

static int journal_parse_u64(const char *text, uint64_t *value)
{
    return infiltratr_parse_u64(text, 10U, value) ? 0 : -1;
}

static int journal_load(const char *path, hfs_journal *state, char **error)
{
    memset(state, 0, sizeof(*state));
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        hfs_set_error(error, "cannot open HFS recovery journal: %s",
                      strerror(errno));
        return -1;
    }
    char *line = NULL;
    size_t capacity = 0U;
    if (getline(&line, &capacity, file) < 0)
        goto invalid;
    infiltratr_trim_line_end(line);
    if (strcmp(line, HFS_JOURNAL_MAGIC) != 0)
        goto invalid;
    while (getline(&line, &capacity, file) >= 0) {
        char *key = NULL;
        char *value = NULL;
        if (infiltratr_config_parse_line(line, &key, &value) !=
            INFILTRATR_CONFIG_LINE_ENTRY)
            goto invalid;
        if (strcmp(key, "device") == 0) {
            free(state->device);
            state->device = ld_xstrdup(value);
        } else if (strcmp(key, "target_identity") == 0) {
            free(state->target_identity);
            state->target_identity = ld_xstrdup(value);
        } else if (strcmp(key, "stage") == 0) {
            free(state->stage);
            state->stage = ld_xstrdup(value);
        } else if (strcmp(key, "operation") == 0) {
            infiltratr_copy_string(state->operation,
                                    sizeof(state->operation), value);
        } else if (strcmp(key, "phase") == 0) {
            infiltratr_copy_string(state->phase, sizeof(state->phase), value);
        } else if (strcmp(key, "volume_token") == 0) {
            infiltratr_copy_string(state->volume_token,
                                    sizeof(state->volume_token), value);
        } else if (strcmp(key, "source_sha256") == 0) {
            infiltratr_copy_string(state->source_sha256,
                                    sizeof(state->source_sha256), value);
        } else if (strcmp(key, "stage_sha256") == 0) {
            infiltratr_copy_string(state->stage_sha256,
                                    sizeof(state->stage_sha256), value);
        } else if (strcmp(key, "physical_bytes") == 0) {
            if (journal_parse_u64(value, &state->physical_bytes) != 0)
                goto invalid;
        } else if (strcmp(key, "block_size") == 0) {
            uint64_t parsed = 0U;
            if (journal_parse_u64(value, &parsed) != 0 ||
                parsed > UINT32_MAX)
                goto invalid;
            state->block_size = (uint32_t)parsed;
        } else if (strcmp(key, "total_blocks") == 0) {
            uint64_t parsed = 0U;
            if (journal_parse_u64(value, &parsed) != 0 ||
                parsed > UINT32_MAX)
                goto invalid;
            state->total_blocks = (uint32_t)parsed;
        } else if (strcmp(key, "allocation_start") == 0) {
            uint64_t parsed = 0U;
            if (journal_parse_u64(value, &parsed) != 0 ||
                parsed > UINT16_MAX)
                goto invalid;
            state->allocation_start = (uint16_t)parsed;
        }
    }
    free(line);
    (void)fclose(file);
    if (state->device == NULL || state->target_identity == NULL ||
        state->stage == NULL || state->operation[0] == '\0' ||
        state->phase[0] == '\0' || strlen(state->volume_token) != 64U ||
        strlen(state->source_sha256) != 64U ||
        strlen(state->stage_sha256) != 64U ||
        state->physical_bytes == 0U || state->block_size == 0U ||
        state->total_blocks == 0U)
        goto invalid_state;
    return 0;
invalid:
    free(line);
    (void)fclose(file);
invalid_state:
    journal_free(state);
    hfs_set_error(error, "HFS recovery journal is malformed or incomplete");
    return -1;
}

static int journal_phase(const char *path, hfs_journal *state,
                         const char *phase, char **error)
{
    infiltratr_copy_string(state->phase, sizeof(state->phase), phase);
    return journal_save(path, state, error);
}

static char *journal_stage_name(const char *journal)
{
    return ld_path_append_suffix(journal, HFS_STAGE_SUFFIX);
}

static void transaction_cleanup(const char *journal, const hfs_journal *state)
{
    if (state != NULL)
        writer_unlink(state->stage);
    writer_unlink(journal);
}

static int digest_final(EVP_MD_CTX *context, char output[65], char **error)
{
    unsigned char digest[32];
    unsigned int length = 0U;
    if (EVP_DigestFinal_ex(context, digest, &length) != 1 ||
        length != sizeof(digest)) {
        hfs_set_error(error, "finalising HFS SHA-256 failed");
        return -1;
    }
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0U; i < sizeof(digest); ++i) {
        output[i * 2U] = digits[digest[i] >> 4U];
        output[i * 2U + 1U] = digits[digest[i] & 15U];
    }
    output[64] = '\0';
    return 0;
}

static int hash_path(const char *path, uint64_t bytes, bool stoppable,
                     char output[65], char **error)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        hfs_set_error(error, "cannot open HFS bytes for SHA-256: %s",
                      strerror(errno));
        return -1;
    }
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    uint8_t *buffer = malloc(HFS_IO_CHUNK);
    if (context == NULL || buffer == NULL ||
        EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(context);
        free(buffer);
        (void)close(fd);
        hfs_set_error(error, "cannot initialise HFS SHA-256");
        return -1;
    }
    int rc = 0;
    for (uint64_t offset = 0U; offset < bytes;) {
        if (stoppable && ld_stop_requested()) {
            rc = HFS_STOPPED;
            break;
        }
        const uint64_t remain = bytes - offset;
        const size_t count = remain > HFS_IO_CHUNK
                           ? HFS_IO_CHUNK : (size_t)remain;
        if (ld_pread_full(fd, buffer, count, offset) != (ssize_t)count ||
            EVP_DigestUpdate(context, buffer, count) != 1) {
            hfs_set_error(error, "hashing HFS bytes failed");
            rc = -1;
            break;
        }
        offset += count;
    }
    if (rc == 0)
        rc = digest_final(context, output, error);
    EVP_MD_CTX_free(context);
    free(buffer);
    (void)close(fd);
    return rc;
}

static int volume_token(const char *path, uint64_t physical_bytes,
                        uint32_t *block_size, uint32_t *total_blocks,
                        uint16_t *allocation_start,
                        char output[65], char **error)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        hfs_set_error(error, "cannot open HFS target for identity verification: %s",
                      strerror(errno));
        return -1;
    }
    uint8_t header[512];
    const ssize_t got = ld_pread_full(fd, header, sizeof(header), HFS_MDB_OFFSET);
    (void)close(fd);
    if (got != (ssize_t)sizeof(header) ||
        infiltratr_load_be16(header) != HFS_SIGNATURE) {
        hfs_set_error(error, "HFS target identity check found a different filesystem");
        return -1;
    }
    const uint32_t found_block_size = infiltratr_load_be32(header + 20U);
    const uint32_t found_total_blocks = infiltratr_load_be16(header + 18U);
    const uint16_t found_allocation_start = infiltratr_load_be16(header + 28U);
    if (found_block_size < HFS_LOGICAL_BLOCK_SIZE ||
        found_block_size % HFS_LOGICAL_BLOCK_SIZE != 0U ||
        found_total_blocks == 0U ||
        (uint64_t)found_allocation_start * HFS_LOGICAL_BLOCK_SIZE +
            (uint64_t)found_total_blocks * found_block_size >
            physical_bytes) {
        hfs_set_error(error, "HFS target identity check found invalid allocation geometry");
        return -1;
    }
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (context == NULL ||
        EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(context, header, sizeof(header)) != 1 ||
        digest_final(context, output, error) != 0) {
        EVP_MD_CTX_free(context);
        if (error != NULL && *error == NULL)
            hfs_set_error(error, "cannot hash HFS volume identity");
        return -1;
    }
    EVP_MD_CTX_free(context);
    *block_size = found_block_size;
    *total_blocks = found_total_blocks;
    *allocation_start = found_allocation_start;
    return 0;
}

static int capture_target(const char *device, hfs_journal *state, char **error)
{
    if (ld_device_capture_binding(device, &state->device,
                                  &state->target_identity,
                                  &state->physical_bytes) != 0) {
        hfs_set_error(error, "cannot bind HFS target: %s", strerror(errno));
        return -1;
    }
    if (volume_token(state->device, state->physical_bytes,
                     &state->block_size, &state->total_blocks,
                     &state->allocation_start, state->volume_token, error) != 0)
        return -1;
    hfs_writer_volume volume;
    if (writer_volume_open(state->device, &volume, error) != 0)
        return -1;
    writer_volume_close(&volume);
    return hash_path(state->device, state->physical_bytes, true,
                     state->source_sha256, error);
}

static int check_target_identity(const char *device, const hfs_journal *state,
                                 char **error)
{
    char *canonical = NULL;
    char *identity = NULL;
    uint64_t size = 0U;
    int rc = ld_device_capture_binding(device, &canonical, &identity, &size);
    if (rc != 0)
        hfs_set_error(error, "cannot rebind HFS target: %s", strerror(errno));
    if (rc == 0 &&
        (strcmp(canonical, state->device) != 0 ||
         strcmp(identity, state->target_identity) != 0 ||
         size != state->physical_bytes)) {
        hfs_set_error(error, "HFS target path, identity or capacity changed before commit");
        rc = -1;
    }
    char token[65];
    uint32_t block_size = 0U, total_blocks = 0U;
    uint16_t allocation_start = 0U;
    if (rc == 0 &&
        (volume_token(canonical, size, &block_size, &total_blocks,
                      &allocation_start, token, error) != 0 ||
         block_size != state->block_size ||
         total_blocks != state->total_blocks ||
         allocation_start != state->allocation_start ||
         strcmp(token, state->volume_token) != 0)) {
        if (error != NULL && *error == NULL)
            hfs_set_error(error, "HFS filesystem identity changed before commit");
        rc = -1;
    }
    free(identity);
    free(canonical);
    return rc;
}

static int check_source_unchanged(const char *device,
                                  const hfs_journal *state, char **error)
{
    if (check_target_identity(device, state, error) != 0)
        return -1;
    char digest[65];
    const int rc = hash_path(state->device, state->physical_bytes, true,
                             digest, error);
    if (rc != 0)
        return rc;
    if (strcmp(digest, state->source_sha256) != 0) {
        hfs_set_error(error, "HFS source changed after preflight; refusing source writes");
        return -1;
    }
    return 0;
}

static int safe_commit_stage(const char *stage_path, const char *target_path,
                             const hfs_journal *state,
                             uint64_t *written, char **error)
{
    char stage_token[65];
    uint32_t stage_block_size = 0U, stage_total_blocks = 0U;
    uint16_t stage_allocation_start = 0U;
    if (volume_token(stage_path, state->physical_bytes,
                     &stage_block_size, &stage_total_blocks,
                     &stage_allocation_start, stage_token, error) != 0 ||
        stage_block_size != state->block_size ||
        stage_total_blocks != state->total_blocks ||
        stage_allocation_start != state->allocation_start ||
        strcmp(stage_token, state->volume_token) != 0) {
        if (error != NULL && *error == NULL)
            hfs_set_error(error, "verified HFS stage geometry differs from source");
        return -1;
    }
    int stage = open(stage_path, O_RDONLY | O_CLOEXEC);
    int target = ld_device_open_verified_fd(target_path, true,
                                            state->target_identity,
                                            state->physical_bytes);
    if (stage < 0 || target < 0) {
        if (stage >= 0)
            (void)close(stage);
        if (target >= 0)
            (void)close(target);
        hfs_set_error(error, "cannot open HFS stage or target for commit: %s",
                      strerror(errno));
        return -1;
    }
    if (flock(target, LOCK_EX | LOCK_NB) != 0) {
        hfs_set_error(error, "cannot lock HFS target for commit: %s",
                      strerror(errno));
        (void)close(stage);
        (void)close(target);
        return -1;
    }
    uint8_t *buffer = malloc(HFS_IO_CHUNK);
    if (buffer == NULL) {
        hfs_set_error(error, "out of memory committing HFS stage");
        (void)flock(target, LOCK_UN);
        (void)close(stage);
        (void)close(target);
        return -1;
    }
    uint64_t total = 0U;
    int rc = 0;
    for (uint64_t offset = 0U; offset < state->physical_bytes;) {
        if (ld_stop_requested()) {
            if (ld_sync_fd(target) != 0) {
                hfs_set_error(error, "cannot sync HFS target at Stop boundary: %s",
                              strerror(errno));
                rc = -1;
            } else {
                rc = HFS_STOPPED;
            }
            break;
        }
        const uint64_t remain = state->physical_bytes - offset;
        const size_t count = remain > HFS_IO_CHUNK
                           ? HFS_IO_CHUNK : (size_t)remain;
        if (ld_pread_full(stage, buffer, count, offset) != (ssize_t)count ||
            ld_pwrite_full(target, buffer, count, offset) != (ssize_t)count) {
            hfs_set_error(error, "short I/O committing HFS bytes at offset %" PRIu64,
                          offset);
            rc = -1;
            break;
        }
        offset += count;
        total += count;
    }
    if (rc == 0 && ld_sync_fd(target) != 0) {
        hfs_set_error(error, "cannot sync HFS target: %s", strerror(errno));
        rc = -1;
    }
    free(buffer);
    (void)flock(target, LOCK_UN);
    (void)close(stage);
    (void)close(target);
    if (written != NULL)
        *written = total;
    return rc;
}

static bool valid_operation(const char *operation)
{
    return strcmp(operation, "defrag") == 0 ||
           strcmp(operation, "growth-defrag") == 0;
}

static bool valid_phase(const char *phase)
{
    return strcmp(phase, "staged") == 0 ||
           strcmp(phase, "committing") == 0 ||
           strcmp(phase, "committed") == 0;
}

static int recover_transaction(const char *device, const char *journal,
                               bool live, char **error)
{
    hfs_journal state;
    if (journal_load(journal, &state, error) != 0)
        return 1;
    if (!ld_path_is_derived_from(state.stage, journal, HFS_STAGE_SUFFIX)) {
        hfs_set_error(error,
                      "HFS recovery stage is not derived from the selected journal path");
        journal_free(&state);
        return 1;
    }
    if (!valid_operation(state.operation) || !valid_phase(state.phase)) {
        hfs_set_error(error,
                      "HFS recovery journal contains an unsupported operation or phase");
        journal_free(&state);
        return 1;
    }
    int identity_rc = strcmp(state.phase, "staged") == 0
        ? check_source_unchanged(device, &state, error)
        : check_target_identity(device, &state, error);
    if (identity_rc == HFS_STOPPED) {
        ld_emit_result_event(stdout, "recover", "stopped",
                             "Stopped before recovery source writes; artifacts remain intact.");
        journal_free(&state);
        return HFS_STOPPED;
    }
    if (identity_rc != 0) {
        journal_free(&state);
        return 1;
    }
    char digest[65];
    const int digest_rc = hash_path(state.stage, state.physical_bytes, true,
                                    digest, error);
    if (digest_rc == HFS_STOPPED) {
        ld_emit_result_event(stdout, "recover", "stopped",
                             "Stopped before recovery source writes; artifacts remain intact.");
        journal_free(&state);
        return HFS_STOPPED;
    }
    if (digest_rc != 0 || strcmp(digest, state.stage_sha256) != 0) {
        if (digest_rc == 0)
            hfs_set_error(error,
                          "HFS recovery stage SHA-256 does not match the journal");
        journal_free(&state);
        return 1;
    }
    const bool growth = strcmp(state.operation, "growth-defrag") == 0;
    if (hfs_verify_layout(state.stage, growth, 10U, error) != 0) {
        journal_free(&state);
        return 1;
    }
    if (strcmp(state.phase, "committed") == 0) {
        if (hfs_verify_layout(device, growth, 10U, error) != 0) {
            journal_free(&state);
            return 1;
        }
        transaction_cleanup(journal, &state);
        ld_emit_result_event(stdout, "recover", "completed",
                             "Verified an already committed classic HFS transaction.");
        journal_free(&state);
        return 0;
    }
    if (journal_phase(journal, &state, "committing", error) != 0) {
        journal_free(&state);
        return 1;
    }
    uint64_t written = 0U;
    const int commit_rc =
        safe_commit_stage(state.stage, state.device, &state, &written, error);
    if (commit_rc == HFS_STOPPED) {
        ld_emit_result_event(stdout, "recover", "stopped",
                             "Recovery stopped at a durable boundary and can be resumed.");
        journal_free(&state);
        return HFS_STOPPED;
    }
    if (commit_rc != 0 ||
        hfs_verify_layout(state.device, growth, 10U, error) != 0) {
        journal_free(&state);
        return 1;
    }
    if (journal_phase(journal, &state, "committed", error) != 0) {
        journal_free(&state);
        return 1;
    }
    transaction_cleanup(journal, &state);
    if (live) {
        (void)printf(
            "@@LIVE_RESET {\"reason\":\"authoritative post-recovery classic HFS map\"}\n");
        (void)fflush(stdout);
    }
    (void)printf("Recovered verified classic HFS source; committed %" PRIu64
                 " KiB.\n", written / 1024U);
    ld_emit_result_event(stdout, "recover", "completed", "");
    journal_free(&state);
    return 0;
}

static bool parse_writer_unsigned(const char *text, unsigned *value)
{
    uint64_t parsed = 0U;
    if (!infiltratr_parse_u64_range(text, 10U, 0U, UINT_MAX, &parsed))
        return false;
    *value = (unsigned)parsed;
    return true;
}

static void writer_usage(FILE *stream)
{
    (void)fprintf(
        stream,
        "usage: %s --version | identify DEVICE | scan-json DEVICE | "
        "map DEVICE --cells COUNT | "
        "defrag|growth-defrag|recover DEVICE --write --confirm DEVICE "
        "--journal PATH [--growth-percent 10] [--live-updates]\n",
        HFS_PROG);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        (void)printf("%s %s\n", HFS_PROG, LD_VERSION);
        return 0;
    }
    if (argc >= 2 &&
        (strcmp(argv[1], "identify") == 0 ||
         strcmp(argv[1], "scan-json") == 0 ||
         strcmp(argv[1], "map") == 0))
        return hfs_readonly_main(argc, argv);

    if (argc < 3) {
        writer_usage(stderr);
        return 2;
    }
    const char *mode = argv[1];
    const char *device = argv[2];
    const bool growth = strcmp(mode, "growth-defrag") == 0;
    const bool defrag = strcmp(mode, "defrag") == 0;
    const bool recover = strcmp(mode, "recover") == 0;
    if (!growth && !defrag && !recover) {
        writer_usage(stderr);
        return 2;
    }

    const char *confirm = NULL;
    const char *journal = NULL;
    unsigned growth_percent = 10U;
    bool write = false;
    bool live = false;
    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--write") == 0)
            write = true;
        else if (strcmp(argv[i], "--live-updates") == 0)
            live = true;
        else if (strcmp(argv[i], "--confirm") == 0 && i + 1 < argc)
            confirm = argv[++i];
        else if (strcmp(argv[i], "--journal") == 0 && i + 1 < argc)
            journal = argv[++i];
        else if (strcmp(argv[i], "--growth-percent") == 0 && i + 1 < argc) {
            if (!parse_writer_unsigned(argv[++i], &growth_percent)) {
                (void)fprintf(stderr, "%s: invalid --growth-percent\n", HFS_PROG);
                return 2;
            }
        } else if ((strcmp(argv[i], "--workers") == 0 ||
                    strcmp(argv[i], "--ram-buffer") == 0 ||
                    strcmp(argv[i], "--batch-clusters") == 0 ||
                    strcmp(argv[i], "--live-map-cells") == 0) &&
                   i + 1 < argc) {
            ++i;
        } else {
            (void)fprintf(stderr, "%s: unknown or incomplete HFS option: %s\n",
                          HFS_PROG, argv[i]);
            return 2;
        }
    }
    if (!write || confirm == NULL || journal == NULL ||
        strcmp(confirm, device) != 0) {
        (void)fprintf(stderr,
                      "%s: HFS mutation requires --write --confirm DEVICE --journal PATH\n",
                      HFS_PROG);
        return 2;
    }
    if (growth && growth_percent != 10U) {
        (void)fprintf(stderr,
                      "%s: HFS Growth Defrag requires exactly 10 percent reserve\n",
                      HFS_PROG);
        return 2;
    }
    if (ld_path_is_mounted(device)) {
        (void)fprintf(stderr,
                      "%s: HFS target is mounted; raw mutation and recovery require an unmounted filesystem\n",
                      HFS_PROG);
        return 1;
    }

    ld_stop_clear();
    ld_stop_install_handlers();
    char *error = NULL;
    if (recover) {
        const int rc = recover_transaction(device, journal, live, &error);
        if (rc != 0 && rc != HFS_STOPPED)
            (void)fprintf(stderr, "%s: %s\n", HFS_PROG,
                          error != NULL ? error : "HFS recovery failed");
        free(error);
        return rc;
    }

    hfs_journal state;
    memset(&state, 0, sizeof(state));
    infiltratr_copy_string(state.operation, sizeof(state.operation), mode);
    int capture_rc = capture_target(device, &state, &error);
    if (capture_rc == HFS_STOPPED) {
        ld_emit_result_event(stdout, mode, "stopped",
                             "Stopped during read-only HFS preflight.");
        journal_free(&state);
        free(error);
        return HFS_STOPPED;
    }
    if (capture_rc != 0)
        goto fail;

    state.stage = journal_stage_name(journal);
    if (state.stage == NULL) {
        hfs_set_error(&error, "out of memory creating HFS stage path");
        goto fail;
    }
    if (access(journal, F_OK) == 0 || access(state.stage, F_OK) == 0) {
        hfs_set_error(&error,
                      "existing HFS recovery artifacts must be recovered or removed before starting a new transaction");
        goto fail;
    }

    (void)printf("Starting native C classic HFS %s on %s.\n",
                 growth ? "Growth Defrag" : "Defrag", device);
    uint64_t planned = 0U;
    const int stage_rc =
        hfs_build_stage(state.device, state.stage, growth, growth_percent,
                        live, &planned, &error);
    if (stage_rc != 0 ||
        hfs_verify_layout(state.stage, growth, growth_percent, &error) != 0) {
        writer_unlink(state.stage);
        if (stage_rc == HFS_STOPPED) {
            ld_emit_result_event(stdout, mode, "stopped",
                                 "Stopped before any classic HFS source writes.");
            journal_free(&state);
            free(error);
            return HFS_STOPPED;
        }
        goto fail;
    }
    if (ld_stop_requested()) {
        writer_unlink(state.stage);
        ld_emit_result_event(stdout, mode, "stopped",
                             "Stopped before any classic HFS source writes.");
        journal_free(&state);
        free(error);
        return HFS_STOPPED;
    }
    const int unchanged =
        check_source_unchanged(state.device, &state, &error);
    if (unchanged != 0) {
        writer_unlink(state.stage);
        if (unchanged == HFS_STOPPED) {
            ld_emit_result_event(stdout, mode, "stopped",
                                 "Stopped before any classic HFS source writes.");
            journal_free(&state);
            free(error);
            return HFS_STOPPED;
        }
        goto fail;
    }
    const int digest_rc =
        hash_path(state.stage, state.physical_bytes, true,
                  state.stage_sha256, &error);
    if (digest_rc != 0) {
        writer_unlink(state.stage);
        if (digest_rc == HFS_STOPPED) {
            ld_emit_result_event(stdout, mode, "stopped",
                                 "Stopped before any classic HFS source writes.");
            journal_free(&state);
            free(error);
            return HFS_STOPPED;
        }
        goto fail;
    }
    infiltratr_copy_string(state.phase, sizeof(state.phase), "staged");
    if (journal_save(journal, &state, &error) != 0) {
        writer_unlink(state.stage);
        goto fail;
    }
    if (journal_phase(journal, &state, "committing", &error) != 0)
        goto fail;

    (void)printf("Classic HFS source commit: replaying %" PRIu64
                 " KiB from the verified full-volume stage.\n",
                 planned / 1024U);
    (void)fflush(stdout);
    uint64_t written = 0U;
    const int commit_rc =
        safe_commit_stage(state.stage, state.device, &state, &written, &error);
    if (commit_rc == HFS_STOPPED) {
        ld_emit_result_event(stdout, mode, "stopped",
                             "Run Recover to resume the verified classic HFS transaction.");
        journal_free(&state);
        free(error);
        return HFS_STOPPED;
    }
    if (commit_rc != 0 ||
        hfs_verify_layout(state.device, growth, growth_percent, &error) != 0)
        goto fail;
    if (journal_phase(journal, &state, "committed", &error) != 0)
        goto fail;
    transaction_cleanup(journal, &state);
    if (live) {
        (void)printf(
            "@@LIVE_RESET {\"reason\":\"authoritative post-commit classic HFS map\"}\n");
        (void)fflush(stdout);
    }
    (void)printf("Classic HFS %s completed; committed %" PRIu64 " KiB.\n",
                 growth ? "Growth Defrag" : "Defrag", written / 1024U);
    ld_emit_result_event(stdout, mode, "completed", "");
    journal_free(&state);
    free(error);
    return 0;

fail:
    (void)fprintf(stderr, "%s: %s\n", HFS_PROG,
                  error != NULL ? error : "HFS transaction failed");
    journal_free(&state);
    free(error);
    return 1;
}
