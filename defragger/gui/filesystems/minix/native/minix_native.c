// SPDX-License-Identifier: GPL-3.0-or-later
#include "minix_native.h"

#include "ld_io.h"
#include "infiltratr/arithmetic.h"
#include "infiltratr/endian.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define MINIX_SUPER_OFFSET 1024U
#define MINIX_SUPER_BYTES 64U
#define MINIX_V1_MAGIC 0x137fU
#define MINIX_V1_30_MAGIC 0x138fU
#define MINIX_V2_MAGIC 0x2468U
#define MINIX_V2_30_MAGIC 0x2478U
#define MINIX_V3_MAGIC 0x4d5aU
#define MINIX_MODE_TYPE 0170000U
#define MINIX_MODE_DIR 0040000U
#define MINIX_MODE_REG 0100000U
#define MINIX_MODE_LNK 0120000U
#define MINIX_DIRECT_ZONES 7U

typedef struct {
    int fd;
    const MinixSummary *summary;
    const uint8_t *zmap;
    size_t zmap_bytes;
} MinixWalk;

typedef struct {
    bool have_previous;
    uint32_t previous;
    bool fragmented;
    uint64_t count;
    MinixMapCell *cells;
    uint64_t cell_count;
    bool mark_fragmented;
    bool mark_directory;
} MinixVisit;

static void minix_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size != 0U)
        (void)snprintf(error, error_size, "%s", message);
}

static uint16_t minix_u16(const uint8_t *p, bool little_endian)
{
    return little_endian ? infiltratr_load_le16(p) : infiltratr_load_be16(p);
}

static uint32_t minix_u32(const uint8_t *p, bool little_endian)
{
    return little_endian ? infiltratr_load_le32(p) : infiltratr_load_be32(p);
}

static bool minix_magic_v1_v2(uint16_t magic)
{
    return magic == MINIX_V1_MAGIC || magic == MINIX_V1_30_MAGIC ||
           magic == MINIX_V2_MAGIC || magic == MINIX_V2_30_MAGIC;
}

static bool minix_detect(const uint8_t *superblock, uint16_t *magic,
                         bool *little_endian)
{
    const bool orders[] = {true, false};
    for (size_t index = 0U; index < sizeof(orders) / sizeof(orders[0]); ++index) {
        const bool little = orders[index];
        const uint16_t legacy = minix_u16(superblock + 16U, little);
        if (minix_magic_v1_v2(legacy)) {
            *magic = legacy;
            *little_endian = little;
            return true;
        }
        const uint16_t version3 = minix_u16(superblock + 24U, little);
        if (version3 == MINIX_V3_MAGIC) {
            *magic = version3;
            *little_endian = little;
            return true;
        }
    }
    return false;
}

static int minix_parse(const uint8_t *superblock, MinixSummary *summary,
                       char *error, size_t error_size)
{
    uint16_t magic = 0U;
    bool little_endian = true;
    if (!minix_detect(superblock, &magic, &little_endian)) {
        minix_error(error, error_size, "not a recognised Minix filesystem");
        return -1;
    }

    memset(summary, 0, sizeof(*summary));
    summary->magic = magic;
    summary->little_endian = little_endian;

    if (magic == MINIX_V3_MAGIC) {
        summary->version = 3U;
        summary->inode_count = minix_u32(superblock + 0U, little_endian);
        summary->imap_blocks = minix_u16(superblock + 6U, little_endian);
        summary->zmap_blocks = minix_u16(superblock + 8U, little_endian);
        summary->first_data_zone = minix_u16(superblock + 10U, little_endian);
        summary->log_zone_size = minix_u16(superblock + 12U, little_endian);
        summary->max_size = minix_u32(superblock + 16U, little_endian);
        summary->zone_count = minix_u32(superblock + 20U, little_endian);
        summary->block_size = minix_u16(superblock + 28U, little_endian);
        if (summary->block_size == 0U)
            summary->block_size = 1024U;
    } else {
        summary->version =
            (magic == MINIX_V1_MAGIC || magic == MINIX_V1_30_MAGIC) ? 1U : 2U;
        summary->long_names =
            magic == MINIX_V1_30_MAGIC || magic == MINIX_V2_30_MAGIC;
        summary->inode_count = minix_u16(superblock + 0U, little_endian);
        summary->imap_blocks = minix_u16(superblock + 4U, little_endian);
        summary->zmap_blocks = minix_u16(superblock + 6U, little_endian);
        summary->first_data_zone = minix_u16(superblock + 8U, little_endian);
        summary->log_zone_size = minix_u16(superblock + 10U, little_endian);
        summary->max_size = minix_u32(superblock + 12U, little_endian);
        summary->zone_count = summary->version == 1U
                                  ? minix_u16(superblock + 2U, little_endian)
                                  : minix_u32(superblock + 20U, little_endian);
        summary->block_size = 1024U;
    }

    if (summary->inode_count == 0U || summary->zone_count == 0U ||
        summary->imap_blocks == 0U || summary->zmap_blocks == 0U ||
        summary->first_data_zone == 0U ||
        summary->first_data_zone >= summary->zone_count ||
        summary->block_size < 1024U || summary->block_size > 65536U ||
        (summary->block_size & (summary->block_size - 1U)) != 0U ||
        summary->log_zone_size > 8U) {
        minix_error(error, error_size, "invalid Minix filesystem geometry");
        return -1;
    }

    summary->zone_size =
        (uint64_t)summary->block_size << summary->log_zone_size;
    if (error != NULL && error_size != 0U)
        error[0] = '\0';
    return 0;
}

static int minix_size_bytes(int fd, uint64_t *bytes)
{
    struct stat status;
    if (fstat(fd, &status) != 0)
        return -1;
    if (S_ISREG(status.st_mode)) {
        if (status.st_size < 0) {
            errno = EINVAL;
            return -1;
        }
        *bytes = (uint64_t)status.st_size;
        return 0;
    }
    if (!S_ISBLK(status.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(fd, BLKGETSIZE64, bytes) == 0 ? 0 : -1;
}

static bool bitmap_test(const uint8_t *map, size_t map_bytes, uint64_t bit,
                        bool little_endian)
{
    if (little_endian) {
        const uint64_t byte = bit >> 3U;
        if (byte >= map_bytes)
            return false;
        return (map[byte] & (uint8_t)(1U << (bit & 7U))) != 0U;
    }

    const uint64_t word = bit >> 4U;
    const uint64_t offset = word * 2U;
    if (offset + 1U >= map_bytes)
        return false;
    const uint16_t value =
        (uint16_t)(((uint16_t)map[offset] << 8) | map[offset + 1U]);
    return (value & (uint16_t)(1U << (bit & 15U))) != 0U;
}

static bool zone_allocated(const MinixSummary *summary, const uint8_t *zmap,
                           size_t zmap_bytes, uint32_t zone)
{
    if (zone < summary->first_data_zone || zone >= summary->zone_count)
        return false;
    const uint64_t bit =
        (uint64_t)zone - summary->first_data_zone + 1U;
    return bitmap_test(zmap, zmap_bytes, bit, summary->little_endian);
}

static int read_exact_at(int fd, void *buffer, size_t bytes, uint64_t offset,
                         char *error, size_t error_size, const char *message)
{
    const ssize_t count = ld_pread_full(fd, buffer, bytes, offset);
    if (count != (ssize_t)bytes) {
        if (count < 0 && error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "%s: %s", message, strerror(errno));
        else
            minix_error(error, error_size, message);
        return -1;
    }
    return 0;
}

static uint64_t span_for_level(uint64_t entries, unsigned int level)
{
    uint64_t span = 1U;
    for (unsigned int index = 0U; index < level; ++index) {
        if (span > UINT64_MAX / entries)
            return UINT64_MAX;
        span *= entries;
    }
    return span;
}

static void consume_hole(uint64_t *remaining, uint64_t span)
{
    if (*remaining > span)
        *remaining -= span;
    else
        *remaining = 0U;
}

static MinixMapCell *cell_for_unit(MinixMapCell *cells, uint64_t cell_count,
                                   uint64_t unit)
{
    if (cells == NULL || cell_count == 0U)
        return NULL;
    uint64_t lo = 0U;
    uint64_t hi = cell_count;
    while (lo < hi) {
        const uint64_t mid = lo + (hi - lo) / 2U;
        if (unit < cells[mid].start)
            hi = mid;
        else if (unit > cells[mid].end)
            lo = mid + 1U;
        else
            return &cells[mid];
    }
    return NULL;
}

static int visit_data_zone(const MinixWalk *walk, uint32_t zone,
                           MinixVisit *visit, char *error, size_t error_size)
{
    if (!zone_allocated(walk->summary, walk->zmap, walk->zmap_bytes, zone)) {
        minix_error(error, error_size,
                    "Minix inode references an unallocated or invalid data zone");
        return -1;
    }
    if (visit->have_previous && zone != visit->previous + 1U)
        visit->fragmented = true;
    visit->have_previous = true;
    visit->previous = zone;
    visit->count++;

    MinixMapCell *cell =
        cell_for_unit(visit->cells, visit->cell_count, zone);
    if (cell != NULL) {
        if (visit->mark_fragmented)
            cell->fragmented_count++;
        if (visit->mark_directory)
            cell->directory_count++;
    }
    return 0;
}

static uint32_t read_pointer(const uint8_t *block, uint64_t index,
                             unsigned int pointer_bytes, bool little_endian)
{
    const uint8_t *p = block + index * pointer_bytes;
    return pointer_bytes == 2U ? minix_u16(p, little_endian)
                               : minix_u32(p, little_endian);
}

static int walk_indirect(const MinixWalk *walk, uint32_t zone,
                         unsigned int level, uint64_t *remaining,
                         MinixVisit *visit, char *error, size_t error_size)
{
    const unsigned int pointer_bytes =
        walk->summary->version == 1U ? 2U : 4U;
    const uint64_t entries = walk->summary->block_size / pointer_bytes;
    if (*remaining == 0U)
        return 0;
    if (zone == 0U) {
        consume_hole(remaining, span_for_level(entries, level));
        return 0;
    }
    if (!zone_allocated(walk->summary, walk->zmap, walk->zmap_bytes, zone)) {
        minix_error(error, error_size,
                    "Minix inode references an unallocated indirect zone");
        return -1;
    }

    uint8_t *block = malloc(walk->summary->block_size);
    if (block == NULL) {
        minix_error(error, error_size, "out of memory reading Minix indirect zone");
        return -1;
    }
    const uint64_t offset = (uint64_t)zone * walk->summary->zone_size;
    if (read_exact_at(walk->fd, block, walk->summary->block_size, offset,
                      error, error_size, "cannot read Minix indirect zone") != 0) {
        free(block);
        return -1;
    }

    int result = 0;
    for (uint64_t index = 0U; index < entries && *remaining != 0U; ++index) {
        const uint32_t child =
            read_pointer(block, index, pointer_bytes, walk->summary->little_endian);
        if (level == 1U) {
            if (child != 0U &&
                visit_data_zone(walk, child, visit, error, error_size) != 0) {
                result = -1;
                break;
            }
            (*remaining)--;
        } else if (walk_indirect(walk, child, level - 1U, remaining, visit,
                                 error, error_size) != 0) {
            result = -1;
            break;
        }
    }
    free(block);
    return result;
}

static int walk_inode_data(const MinixWalk *walk, const uint8_t *inode,
                           uint64_t size_bytes, MinixVisit *visit,
                           char *error, size_t error_size)
{
    const unsigned int pointer_bytes =
        walk->summary->version == 1U ? 2U : 4U;
    const size_t zone_offset = walk->summary->version == 1U ? 14U : 24U;
    const unsigned int indirect_levels =
        walk->summary->version == 1U ? 2U : 3U;
    uint64_t remaining =
        (size_bytes + walk->summary->zone_size - 1U) / walk->summary->zone_size;

    for (unsigned int index = 0U;
         index < MINIX_DIRECT_ZONES && remaining != 0U; ++index) {
        const uint32_t zone = read_pointer(
            inode + zone_offset, index, pointer_bytes, walk->summary->little_endian);
        if (zone != 0U &&
            visit_data_zone(walk, zone, visit, error, error_size) != 0)
            return -1;
        remaining--;
    }

    for (unsigned int level = 1U;
         level <= indirect_levels && remaining != 0U; ++level) {
        const uint64_t pointer_index = MINIX_DIRECT_ZONES + (level - 1U);
        const uint32_t zone = read_pointer(
            inode + zone_offset, pointer_index, pointer_bytes,
            walk->summary->little_endian);
        if (walk_indirect(walk, zone, level, &remaining, visit,
                          error, error_size) != 0)
            return -1;
    }
    if (remaining != 0U) {
        minix_error(error, error_size,
                    "Minix inode size exceeds its addressable zone tree");
        return -1;
    }
    return 0;
}

static int load_bitmaps(int fd, const MinixSummary *summary,
                        uint8_t **imap_out, size_t *imap_bytes_out,
                        uint8_t **zmap_out, size_t *zmap_bytes_out,
                        char *error, size_t error_size)
{
    if (summary->log_zone_size != 0U) {
        minix_error(error, error_size,
                    "exact Minix analysis requires zone size equal to block size");
        return -1;
    }
    size_t imap_bytes = 0U;
    size_t zmap_bytes = 0U;
    if (!infiltratr_size_multiply_checked((size_t)summary->imap_blocks,
                                          (size_t)summary->block_size,
                                          &imap_bytes) ||
        !infiltratr_size_multiply_checked((size_t)summary->zmap_blocks,
                                          (size_t)summary->block_size,
                                          &zmap_bytes)) {
        minix_error(error, error_size, "Minix bitmap size overflows address space");
        return -1;
    }
    uint8_t *imap = malloc(imap_bytes);
    uint8_t *zmap = malloc(zmap_bytes);
    if (imap == NULL || zmap == NULL) {
        free(imap);
        free(zmap);
        minix_error(error, error_size, "out of memory reading Minix bitmaps");
        return -1;
    }

    const uint64_t imap_offset = (uint64_t)summary->block_size * 2U;
    const uint64_t zmap_offset = imap_offset + imap_bytes;
    if (read_exact_at(fd, imap, imap_bytes, imap_offset, error, error_size,
                      "cannot read Minix inode bitmap") != 0 ||
        read_exact_at(fd, zmap, zmap_bytes, zmap_offset, error, error_size,
                      "cannot read Minix zone bitmap") != 0) {
        free(imap);
        free(zmap);
        return -1;
    }

    const uint64_t inode_bits = (uint64_t)imap_bytes * 8U;
    const uint64_t zone_bits = (uint64_t)zmap_bytes * 8U;
    const uint64_t needed_zone_bits =
        (uint64_t)summary->zone_count - summary->first_data_zone + 1U;
    if (inode_bits <= summary->inode_count || zone_bits < needed_zone_bits) {
        free(imap);
        free(zmap);
        minix_error(error, error_size, "Minix allocation bitmap is too small");
        return -1;
    }
    if (!bitmap_test(imap, imap_bytes, 0U, summary->little_endian) ||
        !bitmap_test(zmap, zmap_bytes, 0U, summary->little_endian)) {
        free(imap);
        free(zmap);
        minix_error(error, error_size, "Minix reserved bitmap bit is not allocated");
        return -1;
    }

    *imap_out = imap;
    *imap_bytes_out = imap_bytes;
    *zmap_out = zmap;
    *zmap_bytes_out = zmap_bytes;
    return 0;
}

static void init_cells(MinixMapCell *cells, uint64_t cell_count,
                       uint64_t total_units, uint32_t filesystem_units)
{
    if (cells == NULL || cell_count == 0U)
        return;
    for (uint64_t index = 0U; index < cell_count; ++index) {
        const uint64_t start = index * total_units / cell_count;
        uint64_t end_exclusive = (index + 1U) * total_units / cell_count;
        if (end_exclusive <= start)
            end_exclusive = start + 1U;
        cells[index].start = start;
        cells[index].end = end_exclusive - 1U;
        cells[index].free_count = 0U;
        cells[index].used_count = 0U;
        cells[index].outside_count = 0U;
        cells[index].fragmented_count = 0U;
        cells[index].directory_count = 0U;
        const uint64_t outside_start =
            start > filesystem_units ? start : filesystem_units;
        if (end_exclusive > outside_start)
            cells[index].outside_count = end_exclusive - outside_start;
    }
}

static int scan_allocation(const MinixSummary *summary, const uint8_t *zmap,
                           size_t zmap_bytes, MinixAnalysis *analysis,
                           MinixMapCell *cells, uint64_t cell_count,
                           char *error, size_t error_size)
{
    uint64_t free_zones = 0U;
    uint64_t used_zones = summary->first_data_zone;

    for (uint64_t unit = 0U; unit < summary->first_data_zone; ++unit) {
        MinixMapCell *cell = cell_for_unit(cells, cell_count, unit);
        if (cell != NULL)
            cell->used_count++;
    }

    for (uint32_t zone = summary->first_data_zone;
         zone < summary->zone_count; ++zone) {
        const bool allocated = zone_allocated(summary, zmap, zmap_bytes, zone);
        MinixMapCell *cell = cell_for_unit(cells, cell_count, zone);
        if (allocated) {
            used_zones++;
            if (cell != NULL)
                cell->used_count++;
        } else {
            free_zones++;
            if (cell != NULL)
                cell->free_count++;
        }
    }
    if (free_zones + used_zones != summary->zone_count) {
        minix_error(error, error_size, "Minix allocation accounting is incomplete");
        return -1;
    }
    analysis->free_zones = free_zones;
    analysis->used_zones = used_zones;
    return 0;
}

static int scan_inodes(int fd, const MinixSummary *summary,
                       const uint8_t *imap, size_t imap_bytes,
                       const uint8_t *zmap, size_t zmap_bytes,
                       MinixAnalysis *analysis, MinixMapCell *cells,
                       uint64_t cell_count, char *error, size_t error_size)
{
    const size_t inode_size = summary->version == 1U ? 32U : 64U;
    const uint64_t inode_table_block =
        2U + summary->imap_blocks + summary->zmap_blocks;
    const uint64_t inode_table_bytes =
        (uint64_t)summary->inode_count * inode_size;
    const uint64_t inode_table_blocks =
        (inode_table_bytes + summary->block_size - 1U) / summary->block_size;
    if (inode_table_block + inode_table_blocks > summary->first_data_zone) {
        minix_error(error, error_size,
                    "Minix inode table overlaps the first data zone");
        return -1;
    }

    uint8_t inode[64];
    MinixWalk walk = {
        .fd = fd,
        .summary = summary,
        .zmap = zmap,
        .zmap_bytes = zmap_bytes,
    };

    for (uint32_t ino = 1U; ino <= summary->inode_count; ++ino) {
        if (!bitmap_test(imap, imap_bytes, ino, summary->little_endian))
            continue;
        const uint64_t offset =
            inode_table_block * summary->block_size +
            (uint64_t)(ino - 1U) * inode_size;
        if (read_exact_at(fd, inode, inode_size, offset, error, error_size,
                          "cannot read allocated Minix inode") != 0)
            return -1;

        const uint16_t mode = minix_u16(inode, summary->little_endian);
        const uint16_t type = (uint16_t)(mode & MINIX_MODE_TYPE);
        if (type != MINIX_MODE_REG && type != MINIX_MODE_DIR)
            continue;
        const uint64_t size_bytes =
            minix_u32(inode + (summary->version == 1U ? 4U : 8U),
                      summary->little_endian);

        MinixVisit first = {0};
        if (walk_inode_data(&walk, inode, size_bytes, &first,
                            error, error_size) != 0)
            return -1;

        const bool directory = type == MINIX_MODE_DIR;
        if (directory) {
            analysis->directories++;
            if (first.fragmented)
                analysis->fragmented_directories++;
        } else {
            analysis->regular_files++;
            if (first.fragmented)
                analysis->fragmented_files++;
        }

        if (directory || first.fragmented) {
            MinixVisit second = {
                .cells = cells,
                .cell_count = cell_count,
                .mark_fragmented = first.fragmented,
                .mark_directory = directory,
            };
            if (walk_inode_data(&walk, inode, size_bytes, &second,
                                error, error_size) != 0)
                return -1;
        }
    }
    return 0;
}

static int minix_read_summary_fd(int fd, MinixSummary *summary,
                                 char *error, size_t error_size)
{
    uint8_t superblock[MINIX_SUPER_BYTES];
    if (read_exact_at(fd, superblock, sizeof(superblock), MINIX_SUPER_OFFSET,
                      error, error_size,
                      "Minix volume is shorter than its superblock") != 0)
        return -1;
    return minix_parse(superblock, summary, error, error_size);
}

int minix_read_summary(const char *path, MinixSummary *summary,
                       char *error, size_t error_size)
{
    if (path == NULL || summary == NULL) {
        minix_error(error, error_size, "invalid Minix summary request");
        return -1;
    }

    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "open: %s", strerror(errno));
        return -1;
    }
    const int result = minix_read_summary_fd(fd, summary, error, error_size);
    const int saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;
    return result;
}

int minix_analyse(const char *path, MinixAnalysis *analysis,
                  MinixMapCell *cells, uint64_t cell_count,
                  char *error, size_t error_size)
{
    if (path == NULL || analysis == NULL) {
        minix_error(error, error_size, "invalid Minix analysis request");
        return -1;
    }
    memset(analysis, 0, sizeof(*analysis));

    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "open: %s", strerror(errno));
        return -1;
    }

    MinixSummary summary;
    if (minix_read_summary_fd(fd, &summary, error, error_size) != 0) {
        (void)close(fd);
        return -1;
    }

    uint64_t physical = 0U;
    if (minix_size_bytes(fd, &physical) != 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "size: %s", strerror(errno));
        (void)close(fd);
        return -1;
    }
    const uint64_t filesystem_bytes =
        (uint64_t)summary.zone_count * summary.zone_size;
    if (filesystem_bytes > physical) {
        minix_error(error, error_size,
                    "Minix filesystem geometry exceeds the target size");
        (void)close(fd);
        return -1;
    }

    uint8_t *imap = NULL;
    uint8_t *zmap = NULL;
    size_t imap_bytes = 0U;
    size_t zmap_bytes = 0U;
    if (load_bitmaps(fd, &summary, &imap, &imap_bytes, &zmap, &zmap_bytes,
                     error, error_size) != 0) {
        (void)close(fd);
        return -1;
    }

    const uint64_t physical_units =
        (physical + summary.zone_size - 1U) / summary.zone_size;
    const uint64_t total_units =
        physical_units > summary.zone_count ? physical_units : summary.zone_count;
    analysis->summary = summary;
    analysis->physical_bytes = physical;
    analysis->filesystem_bytes = filesystem_bytes;
    analysis->total_units = total_units;
    init_cells(cells, cell_count, total_units, summary.zone_count);

    int result = scan_allocation(&summary, zmap, zmap_bytes, analysis,
                                 cells, cell_count, error, error_size);
    if (result == 0)
        result = scan_inodes(fd, &summary, imap, imap_bytes, zmap, zmap_bytes,
                             analysis, cells, cell_count, error, error_size);

    free(imap);
    free(zmap);
    (void)close(fd);
    if (result == 0 && error != NULL && error_size != 0U)
        error[0] = '\0';
    return result;
}


/*
 * Offline Minix relayout
 * ----------------------
 *
 * Mutation deliberately reuses the exact analyser's validated geometry and
 * pointer-tree rules.  A complete sparse stage is built without touching the
 * source, every allocated data zone is copied from the original descriptor,
 * indirect trees and the zone bitmap are regenerated, and the stage is
 * reopened against the canonical placement invariant before it can become a
 * recovery source.
 *
 * The current writer intentionally requires log_zone_size == 0.  Larger Minix
 * zones remain readable at the superblock level but are rejected by exact
 * analysis and therefore cannot cross the write gate.
 */

typedef struct MinixRelayoutObject {
    uint32_t inode_number;
    uint16_t type;
    uint64_t inode_offset;
    uint64_t size_bytes;
    uint64_t logical_zones;
    uint64_t allocated_zones;
    uint32_t *zones;
    uint32_t *targets;
} MinixRelayoutObject;

typedef struct MinixRelayoutCatalogue {
    MinixSummary summary;
    uint64_t physical_bytes;
    uint64_t filesystem_bytes;
    uint8_t *imap;
    size_t imap_bytes;
    uint8_t *zmap;
    size_t zmap_bytes;
    uint8_t *zone_kind; /* 1=data, 2=indirect metadata */
    MinixRelayoutObject *objects;
    size_t object_count;
    size_t object_capacity;
} MinixRelayoutCatalogue;

static void relayout_catalogue_free(MinixRelayoutCatalogue *catalogue)
{
    if (catalogue == NULL)
        return;
    for (size_t index = 0U; index < catalogue->object_count; ++index) {
        free(catalogue->objects[index].zones);
        free(catalogue->objects[index].targets);
    }
    free(catalogue->objects);
    free(catalogue->imap);
    free(catalogue->zmap);
    free(catalogue->zone_kind);
    memset(catalogue, 0, sizeof(*catalogue));
}

static int relayout_reserve_object(MinixRelayoutCatalogue *catalogue,
                                   MinixRelayoutObject **object,
                                   char *error, size_t error_size)
{
    if (catalogue->object_count == SIZE_MAX ||
        !infiltratr_array_reserve((void **)&catalogue->objects,
                                  &catalogue->object_capacity,
                                  sizeof(*catalogue->objects),
                                  catalogue->object_count + 1U, 16U)) {
        minix_error(error, error_size,
                    "out of memory growing Minix inode catalogue");
        return -1;
    }
    *object = &catalogue->objects[catalogue->object_count++];
    memset(*object, 0, sizeof(**object));
    return 0;
}

static void bitmap_store(uint8_t *map, size_t map_bytes, uint64_t bit,
                         bool little_endian, bool allocated)
{
    if (little_endian) {
        const uint64_t byte = bit >> 3U;
        if (byte >= map_bytes)
            return;
        const uint8_t mask = (uint8_t)(1U << (bit & 7U));
        if (allocated)
            map[byte] |= mask;
        else
            map[byte] &= (uint8_t)~mask;
        return;
    }

    const uint64_t word = bit >> 4U;
    const uint64_t offset = word * 2U;
    if (offset + 1U >= map_bytes)
        return;
    uint16_t value =
        (uint16_t)(((uint16_t)map[offset] << 8U) | map[offset + 1U]);
    const uint16_t mask = (uint16_t)(1U << (bit & 15U));
    if (allocated)
        value |= mask;
    else
        value &= (uint16_t)~mask;
    map[offset] = (uint8_t)(value >> 8U);
    map[offset + 1U] = (uint8_t)value;
}

static void store_pointer(uint8_t *block, uint64_t index,
                          unsigned int pointer_bytes, bool little_endian,
                          uint32_t value)
{
    uint8_t *p = block + index * pointer_bytes;
    if (pointer_bytes == 2U) {
        if (little_endian)
            infiltratr_store_le16(p, (uint16_t)value);
        else
            infiltratr_store_be16(p, (uint16_t)value);
    } else if (little_endian) {
        infiltratr_store_le32(p, value);
    } else {
        infiltratr_store_be32(p, value);
    }
}

static uint8_t zone_kind_get(const uint8_t *map, uint32_t zone)
{
    const unsigned shift = (zone & 3U) * 2U;
    return (uint8_t)((map[zone >> 2U] >> shift) & 3U);
}

static void zone_kind_set(uint8_t *map, uint32_t zone, uint8_t kind)
{
    const unsigned shift = (zone & 3U) * 2U;
    const uint8_t mask = (uint8_t)(3U << shift);
    map[zone >> 2U] =
        (uint8_t)((map[zone >> 2U] & (uint8_t)~mask) |
                  (uint8_t)((kind & 3U) << shift));
}

static int claim_relayout_zone(const MinixSummary *summary,
                               const uint8_t *zmap, size_t zmap_bytes,
                               uint8_t *zone_kind, uint32_t zone,
                               uint8_t kind, char *error, size_t error_size)
{
    if (!zone_allocated(summary, zmap, zmap_bytes, zone)) {
        minix_error(error, error_size,
                    "Minix inode tree references an unallocated or invalid zone");
        return -1;
    }
    if (zone_kind_get(zone_kind, zone) != 0U) {
        minix_error(error, error_size,
                    "Minix allocation tree references the same zone more than once");
        return -1;
    }
    zone_kind_set(zone_kind, zone, kind);
    return 0;
}

static int collect_indirect_for_relayout(
    int fd, const MinixSummary *summary,
    const uint8_t *zmap, size_t zmap_bytes, uint8_t *zone_kind,
    uint32_t root, unsigned int level,
    uint64_t *remaining, uint32_t *zones, uint64_t *position,
    char *error, size_t error_size)
{
    const unsigned int pointer_bytes = summary->version == 1U ? 2U : 4U;
    const uint64_t entries = summary->block_size / pointer_bytes;
    if (*remaining == 0U)
        return 0;

    if (root == 0U) {
        uint64_t skip = span_for_level(entries, level);
        if (skip > *remaining)
            skip = *remaining;
        *position += skip;
        *remaining -= skip;
        return 0;
    }
    if (claim_relayout_zone(summary, zmap, zmap_bytes, zone_kind,
                            root, 2U, error, error_size) != 0)
        return -1;

    uint8_t *block = malloc(summary->block_size);
    if (block == NULL) {
        minix_error(error, error_size,
                    "out of memory reading Minix indirect tree");
        return -1;
    }
    if (read_exact_at(fd, block, summary->block_size,
                      (uint64_t)root * summary->zone_size,
                      error, error_size,
                      "cannot read Minix indirect tree") != 0) {
        free(block);
        return -1;
    }

    int result = 0;
    for (uint64_t index = 0U; index < entries && *remaining != 0U; ++index) {
        const uint32_t child =
            read_pointer(block, index, pointer_bytes, summary->little_endian);
        if (level == 1U) {
            if (child != 0U &&
                claim_relayout_zone(summary, zmap, zmap_bytes, zone_kind,
                                    child, 1U, error, error_size) != 0) {
                result = -1;
                break;
            }
            zones[*position] = child;
            (*position)++;
            (*remaining)--;
        } else if (collect_indirect_for_relayout(
                       fd, summary, zmap, zmap_bytes, zone_kind,
                       child, level - 1U, remaining, zones, position,
                       error, error_size) != 0) {
            result = -1;
            break;
        }
    }
    free(block);
    return result;
}

static int collect_inode_for_relayout(
    int fd, const MinixSummary *summary,
    const uint8_t *zmap, size_t zmap_bytes, uint8_t *zone_kind,
    const uint8_t *inode, MinixRelayoutObject *object,
    char *error, size_t error_size)
{
    const unsigned int pointer_bytes = summary->version == 1U ? 2U : 4U;
    const size_t zone_offset = summary->version == 1U ? 14U : 24U;
    const unsigned int indirect_levels = summary->version == 1U ? 2U : 3U;

    object->logical_zones =
        (object->size_bytes + summary->zone_size - 1U) / summary->zone_size;
    if (object->logical_zones > SIZE_MAX / sizeof(uint32_t)) {
        minix_error(error, error_size,
                    "Minix inode zone vector exceeds addressable memory");
        return -1;
    }
    if (object->logical_zones != 0U) {
        object->zones = calloc((size_t)object->logical_zones,
                               sizeof(*object->zones));
        object->targets = calloc((size_t)object->logical_zones,
                                 sizeof(*object->targets));
        if (object->zones == NULL || object->targets == NULL) {
            minix_error(error, error_size,
                        "out of memory cataloguing Minix inode zones");
            return -1;
        }
    }

    uint64_t remaining = object->logical_zones;
    uint64_t position = 0U;
    for (unsigned int index = 0U;
         index < MINIX_DIRECT_ZONES && remaining != 0U; ++index) {
        const uint32_t zone =
            read_pointer(inode + zone_offset, index, pointer_bytes,
                         summary->little_endian);
        if (zone != 0U &&
            claim_relayout_zone(summary, zmap, zmap_bytes, zone_kind,
                                zone, 1U, error, error_size) != 0)
            return -1;
        object->zones[position++] = zone;
        remaining--;
    }

    for (unsigned int level = 1U;
         level <= indirect_levels && remaining != 0U; ++level) {
        const uint32_t root =
            read_pointer(inode + zone_offset,
                         MINIX_DIRECT_ZONES + (level - 1U),
                         pointer_bytes, summary->little_endian);
        if (collect_indirect_for_relayout(
                fd, summary, zmap, zmap_bytes, zone_kind,
                root, level, &remaining, object->zones, &position,
                error, error_size) != 0)
            return -1;
    }
    if (remaining != 0U || position != object->logical_zones) {
        minix_error(error, error_size,
                    "Minix inode exceeds its addressable zone tree");
        return -1;
    }

    for (uint64_t index = 0U; index < object->logical_zones; ++index)
        if (object->zones[index] != 0U)
            object->allocated_zones++;
    return 0;
}

static int relayout_catalogue_load(int fd, MinixRelayoutCatalogue *catalogue,
                                   char *error, size_t error_size)
{
    memset(catalogue, 0, sizeof(*catalogue));
    if (minix_read_summary_fd(fd, &catalogue->summary,
                              error, error_size) != 0)
        return -1;
    if (catalogue->summary.log_zone_size != 0U) {
        minix_error(error, error_size,
                    "Minix mutation requires zone size equal to block size");
        return -1;
    }
    if (minix_size_bytes(fd, &catalogue->physical_bytes) != 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "size: %s", strerror(errno));
        return -1;
    }
    catalogue->filesystem_bytes =
        (uint64_t)catalogue->summary.zone_count *
        catalogue->summary.zone_size;
    if (catalogue->filesystem_bytes > catalogue->physical_bytes) {
        minix_error(error, error_size,
                    "Minix filesystem geometry exceeds the target size");
        return -1;
    }

    if (load_bitmaps(fd, &catalogue->summary,
                     &catalogue->imap, &catalogue->imap_bytes,
                     &catalogue->zmap, &catalogue->zmap_bytes,
                     error, error_size) != 0)
        return -1;

    const size_t zone_kind_bytes =
        ((size_t)catalogue->summary.zone_count + 3U) / 4U;
    catalogue->zone_kind = calloc(zone_kind_bytes, 1U);
    if (catalogue->zone_kind == NULL) {
        minix_error(error, error_size,
                    "out of memory cataloguing Minix allocation ownership");
        return -1;
    }

    const size_t inode_size =
        catalogue->summary.version == 1U ? 32U : 64U;
    const uint64_t inode_table_block =
        2U + catalogue->summary.imap_blocks + catalogue->summary.zmap_blocks;
    const uint64_t inode_table_bytes =
        (uint64_t)catalogue->summary.inode_count * inode_size;
    const uint64_t inode_table_blocks =
        (inode_table_bytes + catalogue->summary.block_size - 1U) /
        catalogue->summary.block_size;
    if (inode_table_block + inode_table_blocks >
        catalogue->summary.first_data_zone) {
        minix_error(error, error_size,
                    "Minix inode table overlaps the first data zone");
        return -1;
    }

    uint8_t inode[64];
    for (uint32_t ino = 1U; ino <= catalogue->summary.inode_count; ++ino) {
        if (!bitmap_test(catalogue->imap, catalogue->imap_bytes, ino,
                         catalogue->summary.little_endian))
            continue;

        const uint64_t offset =
            inode_table_block * catalogue->summary.block_size +
            (uint64_t)(ino - 1U) * inode_size;
        if (read_exact_at(fd, inode, inode_size, offset, error, error_size,
                          "cannot read allocated Minix inode") != 0)
            return -1;

        const uint16_t mode =
            minix_u16(inode, catalogue->summary.little_endian);
        const uint16_t type = (uint16_t)(mode & MINIX_MODE_TYPE);
        const uint64_t size_bytes =
            minix_u32(inode +
                          (catalogue->summary.version == 1U ? 4U : 8U),
                      catalogue->summary.little_endian);

        if (type != MINIX_MODE_REG && type != MINIX_MODE_DIR &&
            type != MINIX_MODE_LNK) {
            /*
             * Device/FIFO/socket inodes do not own data-zone allocation in
             * Minix.  Their zone words can encode device numbers, so they are
             * intentionally not interpreted as allocation pointers here.
             */
            if (size_bytes != 0U) {
                minix_error(error, error_size,
                            "Minix special inode has unsupported non-zero size");
                return -1;
            }
            continue;
        }

        MinixRelayoutObject *object = NULL;
        if (relayout_reserve_object(catalogue, &object,
                                    error, error_size) != 0)
            return -1;
        object->inode_number = ino;
        object->type = type;
        object->inode_offset = offset;
        object->size_bytes = size_bytes;
        if (collect_inode_for_relayout(
                fd, &catalogue->summary,
                catalogue->zmap, catalogue->zmap_bytes,
                catalogue->zone_kind, inode, object,
                error, error_size) != 0)
            return -1;
    }

    for (uint32_t zone = catalogue->summary.first_data_zone;
         zone < catalogue->summary.zone_count; ++zone) {
        if (zone_allocated(&catalogue->summary, catalogue->zmap,
                           catalogue->zmap_bytes, zone) &&
            zone_kind_get(catalogue->zone_kind, zone) == 0U) {
            minix_error(error, error_size,
                        "Minix contains an allocated data-zone not owned by a validated inode tree");
            return -1;
        }
    }
    return 0;
}

static uint64_t growth_reserve_zones(const MinixRelayoutObject *object,
                                     bool growth, unsigned growth_percent)
{
    if (!growth || object->type != MINIX_MODE_REG ||
        object->allocated_zones == 0U)
        return 0U;
    return (object->allocated_zones * growth_percent + 99U) / 100U;
}

static int plan_relayout(MinixRelayoutCatalogue *catalogue,
                         bool growth, unsigned growth_percent,
                         uint32_t *pointer_cursor,
                         char *error, size_t error_size)
{
    uint64_t cursor = catalogue->summary.first_data_zone;
    for (size_t object_index = 0U;
         object_index < catalogue->object_count; ++object_index) {
        MinixRelayoutObject *object = &catalogue->objects[object_index];
        for (uint64_t logical = 0U;
             logical < object->logical_zones; ++logical) {
            if (object->zones[logical] == 0U)
                continue;
            if (cursor >= catalogue->summary.zone_count) {
                minix_error(error, error_size,
                            "Minix canonical layout does not fit the filesystem");
                return -1;
            }
            object->targets[logical] = (uint32_t)cursor++;
        }
        const uint64_t reserve =
            growth_reserve_zones(object, growth, growth_percent);
        if (reserve > catalogue->summary.zone_count - cursor) {
            minix_error(error, error_size,
                        "Minix exact growth reserve does not fit the filesystem");
            return -1;
        }
        cursor += reserve;
    }
    if (cursor > UINT32_MAX) {
        minix_error(error, error_size,
                    "Minix canonical pointer cursor exceeds on-disk range");
        return -1;
    }
    *pointer_cursor = (uint32_t)cursor;
    return 0;
}

static bool target_range_has_data(const uint32_t *targets,
                                  uint64_t start, uint64_t count)
{
    for (uint64_t index = 0U; index < count; ++index)
        if (targets[start + index] != 0U)
            return true;
    return false;
}

static int allocate_pointer_zone(const MinixSummary *summary,
                                 uint32_t *cursor, uint8_t *zmap,
                                 size_t zmap_bytes, uint32_t *zone,
                                 char *error, size_t error_size)
{
    if (*cursor >= summary->zone_count) {
        minix_error(error, error_size,
                    "Minix indirect metadata does not fit the canonical layout");
        return -1;
    }
    *zone = (*cursor)++;
    bitmap_store(zmap, zmap_bytes,
                 (uint64_t)*zone - summary->first_data_zone + 1U,
                 summary->little_endian, true);
    return 0;
}

static int build_pointer_tree(
    int stage_fd, const MinixSummary *summary,
    const uint32_t *targets, uint64_t logical_start, uint64_t logical_count,
    unsigned int level, uint32_t *cursor,
    uint8_t *zmap, size_t zmap_bytes, uint32_t *root,
    char *error, size_t error_size)
{
    *root = 0U;
    if (logical_count == 0U ||
        !target_range_has_data(targets, logical_start, logical_count))
        return 0;

    uint32_t pointer_zone = 0U;
    if (allocate_pointer_zone(summary, cursor, zmap, zmap_bytes,
                              &pointer_zone, error, error_size) != 0)
        return -1;

    uint8_t *block = calloc(summary->block_size, 1U);
    if (block == NULL) {
        minix_error(error, error_size,
                    "out of memory rebuilding Minix indirect tree");
        return -1;
    }
    const unsigned int pointer_bytes =
        summary->version == 1U ? 2U : 4U;
    const uint64_t entries = summary->block_size / pointer_bytes;
    const uint64_t child_span =
        level == 1U ? 1U : span_for_level(entries, level - 1U);

    uint64_t consumed = 0U;
    int result = 0;
    for (uint64_t entry = 0U;
         entry < entries && consumed < logical_count; ++entry) {
        uint64_t take = child_span;
        if (take > logical_count - consumed)
            take = logical_count - consumed;
        uint32_t value = 0U;
        if (level == 1U) {
            value = targets[logical_start + consumed];
        } else if (build_pointer_tree(
                       stage_fd, summary, targets,
                       logical_start + consumed, take, level - 1U,
                       cursor, zmap, zmap_bytes, &value,
                       error, error_size) != 0) {
            result = -1;
            break;
        }
        if (pointer_bytes == 2U && value > UINT16_MAX) {
            minix_error(error, error_size,
                        "Minix v1 indirect pointer exceeds 16-bit range");
            result = -1;
            break;
        }
        store_pointer(block, entry, pointer_bytes,
                      summary->little_endian, value);
        consumed += take;
    }

    if (result == 0 &&
        ld_pwrite_full(stage_fd, block, summary->block_size,
                       (uint64_t)pointer_zone * summary->zone_size) !=
            (ssize_t)summary->block_size) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size,
                           "cannot write Minix indirect tree: %s",
                           strerror(errno));
        result = -1;
    }
    free(block);
    if (result == 0)
        *root = pointer_zone;
    return result;
}

static int copy_exact_region(int source_fd, int target_fd,
                             uint64_t source_offset, uint64_t target_offset,
                             uint64_t bytes, char *error, size_t error_size)
{
    const size_t chunk_size = 1024U * 1024U;
    uint8_t *buffer = malloc(chunk_size);
    if (buffer == NULL) {
        minix_error(error, error_size,
                    "out of memory staging Minix data");
        return -1;
    }
    int result = 0;
    for (uint64_t done = 0U; done < bytes;) {
        const uint64_t remaining = bytes - done;
        const size_t take =
            remaining > chunk_size ? chunk_size : (size_t)remaining;
        if (ld_pread_full(source_fd, buffer, take,
                          source_offset + done) != (ssize_t)take ||
            ld_pwrite_full(target_fd, buffer, take,
                           target_offset + done) != (ssize_t)take) {
            if (error != NULL && error_size != 0U)
                (void)snprintf(error, error_size,
                               "short I/O staging Minix data: %s",
                               strerror(errno));
            result = -1;
            break;
        }
        done += take;
    }
    free(buffer);
    return result;
}

static int write_relayout_inode(
    int source_fd, int stage_fd, const MinixSummary *summary,
    MinixRelayoutObject *object, uint32_t *pointer_cursor,
    uint8_t *zmap, size_t zmap_bytes,
    char *error, size_t error_size)
{
    const size_t inode_size = summary->version == 1U ? 32U : 64U;
    uint8_t inode[64];
    if (read_exact_at(source_fd, inode, inode_size, object->inode_offset,
                      error, error_size,
                      "cannot reread Minix inode for staging") != 0)
        return -1;

    const unsigned int pointer_bytes =
        summary->version == 1U ? 2U : 4U;
    const size_t zone_offset = summary->version == 1U ? 14U : 24U;
    const unsigned int indirect_levels =
        summary->version == 1U ? 2U : 3U;
    const unsigned int pointer_slots =
        MINIX_DIRECT_ZONES + indirect_levels;
    memset(inode + zone_offset, 0,
           (size_t)pointer_slots * pointer_bytes);

    uint64_t logical = 0U;
    for (unsigned int index = 0U;
         index < MINIX_DIRECT_ZONES && logical < object->logical_zones;
         ++index, ++logical) {
        const uint32_t value = object->targets[logical];
        if (pointer_bytes == 2U && value > UINT16_MAX) {
            minix_error(error, error_size,
                        "Minix v1 direct pointer exceeds 16-bit range");
            return -1;
        }
        store_pointer(inode + zone_offset, index, pointer_bytes,
                      summary->little_endian, value);
    }

    const uint64_t entries = summary->block_size / pointer_bytes;
    for (unsigned int level = 1U;
         level <= indirect_levels && logical < object->logical_zones;
         ++level) {
        uint64_t capacity = span_for_level(entries, level);
        uint64_t take = object->logical_zones - logical;
        if (take > capacity)
            take = capacity;
        uint32_t root = 0U;
        if (build_pointer_tree(
                stage_fd, summary, object->targets, logical, take, level,
                pointer_cursor, zmap, zmap_bytes, &root,
                error, error_size) != 0)
            return -1;
        if (pointer_bytes == 2U && root > UINT16_MAX) {
            minix_error(error, error_size,
                        "Minix v1 indirect root exceeds 16-bit range");
            return -1;
        }
        store_pointer(inode + zone_offset,
                      MINIX_DIRECT_ZONES + (level - 1U),
                      pointer_bytes, summary->little_endian, root);
        logical += take;
    }
    if (logical != object->logical_zones) {
        minix_error(error, error_size,
                    "Minix inode cannot be represented by its pointer tree");
        return -1;
    }

    if (ld_pwrite_full(stage_fd, inode, inode_size,
                       object->inode_offset) != (ssize_t)inode_size) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size,
                           "cannot write staged Minix inode: %s",
                           strerror(errno));
        return -1;
    }
    return 0;
}

static int compare_relayout_payloads(
    int source_fd, const MinixRelayoutCatalogue *source,
    int stage_fd, const MinixRelayoutCatalogue *stage,
    char *error, size_t error_size)
{
    if (source->object_count != stage->object_count) {
        minix_error(error, error_size,
                    "Minix staged inode catalogue changed");
        return -1;
    }
    uint8_t *left = malloc(source->summary.zone_size);
    uint8_t *right = malloc(source->summary.zone_size);
    if (left == NULL || right == NULL) {
        free(left);
        free(right);
        minix_error(error, error_size,
                    "out of memory verifying Minix staged payloads");
        return -1;
    }

    int result = 0;
    for (size_t object_index = 0U;
         object_index < source->object_count && result == 0;
         ++object_index) {
        const MinixRelayoutObject *before =
            &source->objects[object_index];
        const MinixRelayoutObject *after =
            &stage->objects[object_index];
        if (before->inode_number != after->inode_number ||
            before->type != after->type ||
            before->size_bytes != after->size_bytes ||
            before->logical_zones != after->logical_zones) {
            minix_error(error, error_size,
                        "Minix staged inode identity changed");
            result = -1;
            break;
        }
        for (uint64_t logical = 0U;
             logical < before->logical_zones; ++logical) {
            const uint32_t source_zone = before->zones[logical];
            const uint32_t stage_zone = after->zones[logical];
            if ((source_zone == 0U) != (stage_zone == 0U)) {
                minix_error(error, error_size,
                            "Minix staged sparse-hole topology changed");
                result = -1;
                break;
            }
            if (source_zone == 0U)
                continue;
            if (ld_pread_full(source_fd, left, source->summary.zone_size,
                              (uint64_t)source_zone *
                                  source->summary.zone_size) !=
                    (ssize_t)source->summary.zone_size ||
                ld_pread_full(stage_fd, right, stage->summary.zone_size,
                              (uint64_t)stage_zone *
                                  stage->summary.zone_size) !=
                    (ssize_t)stage->summary.zone_size ||
                memcmp(left, right, source->summary.zone_size) != 0) {
                minix_error(error, error_size,
                            "Minix staged payload verification failed");
                result = -1;
                break;
            }
        }
    }
    free(left);
    free(right);
    return result;
}

int minix_verify_layout(const char *path, bool growth,
                        unsigned growth_percent,
                        char *error, size_t error_size)
{
    if (path == NULL || (growth && growth_percent != 10U)) {
        minix_error(error, error_size,
                    "invalid Minix layout verification request");
        return -1;
    }
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "open: %s", strerror(errno));
        return -1;
    }

    MinixRelayoutCatalogue catalogue;
    int result =
        relayout_catalogue_load(fd, &catalogue, error, error_size);
    if (result != 0) {
        relayout_catalogue_free(&catalogue);
        (void)close(fd);
        return -1;
    }

    uint64_t cursor = catalogue.summary.first_data_zone;
    for (size_t object_index = 0U;
         object_index < catalogue.object_count && result == 0;
         ++object_index) {
        const MinixRelayoutObject *object =
            &catalogue.objects[object_index];
        for (uint64_t logical = 0U;
             logical < object->logical_zones; ++logical) {
            if (object->zones[logical] == 0U)
                continue;
            if (cursor >= catalogue.summary.zone_count ||
                object->zones[logical] != (uint32_t)cursor) {
                minix_error(error, error_size,
                            "Minix data zones are not in canonical inode order");
                result = -1;
                break;
            }
            cursor++;
        }
        if (result != 0)
            break;
        const uint64_t reserve =
            growth_reserve_zones(object, growth, growth_percent);
        if (reserve > catalogue.summary.zone_count - cursor) {
            minix_error(error, error_size,
                        "Minix growth reserve extends beyond the filesystem");
            result = -1;
            break;
        }
        for (uint64_t gap = 0U; gap < reserve; ++gap) {
            const uint32_t zone = (uint32_t)(cursor + gap);
            if (zone_allocated(&catalogue.summary, catalogue.zmap,
                               catalogue.zmap_bytes, zone)) {
                minix_error(error, error_size,
                            "Minix regular file lost its exact 10 percent growth reserve");
                result = -1;
                break;
            }
        }
        cursor += reserve;
    }

    uint64_t pointer_count = 0U;
    if (result == 0) {
        for (uint32_t zone = catalogue.summary.first_data_zone;
             zone < catalogue.summary.zone_count; ++zone)
            if (zone_kind_get(catalogue.zone_kind, zone) == 2U)
                pointer_count++;
        if (pointer_count > catalogue.summary.zone_count - cursor) {
            minix_error(error, error_size,
                        "Minix canonical indirect metadata exceeds the filesystem");
            result = -1;
        }
    }
    if (result == 0) {
        for (uint64_t index = 0U; index < pointer_count; ++index) {
            const uint32_t zone = (uint32_t)(cursor + index);
            if (zone_kind_get(catalogue.zone_kind, zone) != 2U) {
                minix_error(error, error_size,
                            "Minix indirect metadata is not canonically packed");
                result = -1;
                break;
            }
        }
    }
    if (result == 0) {
        const uint64_t end = cursor + pointer_count;
        for (uint64_t zone = end; zone < catalogue.summary.zone_count; ++zone) {
            if (zone_allocated(&catalogue.summary, catalogue.zmap,
                               catalogue.zmap_bytes, (uint32_t)zone)) {
                minix_error(error, error_size,
                            "Minix has avoidable allocation after the canonical layout");
                result = -1;
                break;
            }
        }
    }

    relayout_catalogue_free(&catalogue);
    (void)close(fd);
    if (result == 0 && error != NULL && error_size != 0U)
        error[0] = '\0';
    return result;
}

int minix_build_stage(const char *source_path, const char *stage_path,
                      bool growth, unsigned growth_percent,
                      uint64_t *commit_bytes,
                      char *error, size_t error_size)
{
    if (source_path == NULL || stage_path == NULL ||
        (growth && growth_percent != 10U)) {
        minix_error(error, error_size,
                    "invalid Minix staging request");
        return -1;
    }

    int source_fd = open(source_path, O_RDONLY | O_CLOEXEC);
    if (source_fd < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "open: %s", strerror(errno));
        return -1;
    }

    MinixRelayoutCatalogue source;
    int result =
        relayout_catalogue_load(source_fd, &source, error, error_size);
    if (result != 0) {
        relayout_catalogue_free(&source);
        (void)close(source_fd);
        return -1;
    }

    uint32_t pointer_cursor = 0U;
    if (plan_relayout(&source, growth, growth_percent,
                      &pointer_cursor, error, error_size) != 0) {
        relayout_catalogue_free(&source);
        (void)close(source_fd);
        return -1;
    }

    int stage_fd =
        open(stage_path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (stage_fd < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size,
                           "cannot create Minix stage: %s", strerror(errno));
        relayout_catalogue_free(&source);
        (void)close(source_fd);
        return -1;
    }
    if (ftruncate(stage_fd, (off_t)source.filesystem_bytes) != 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size,
                           "cannot size Minix stage: %s", strerror(errno));
        result = -1;
        goto stage_done;
    }

    const uint64_t fixed_bytes =
        (uint64_t)source.summary.first_data_zone *
        source.summary.zone_size;
    if (copy_exact_region(source_fd, stage_fd, 0U, 0U, fixed_bytes,
                          error, error_size) != 0) {
        result = -1;
        goto stage_done;
    }

    uint8_t *new_zmap = malloc(source.zmap_bytes);
    if (new_zmap == NULL) {
        minix_error(error, error_size,
                    "out of memory rebuilding Minix allocation bitmap");
        result = -1;
        goto stage_done;
    }
    memcpy(new_zmap, source.zmap, source.zmap_bytes);
    for (uint32_t zone = source.summary.first_data_zone;
         zone < source.summary.zone_count; ++zone)
        bitmap_store(new_zmap, source.zmap_bytes,
                     (uint64_t)zone - source.summary.first_data_zone + 1U,
                     source.summary.little_endian, false);

    for (size_t object_index = 0U;
         object_index < source.object_count && result == 0;
         ++object_index) {
        MinixRelayoutObject *object =
            &source.objects[object_index];
        for (uint64_t logical = 0U;
             logical < object->logical_zones; ++logical) {
            if (object->zones[logical] == 0U)
                continue;
            const uint32_t target = object->targets[logical];
            bitmap_store(new_zmap, source.zmap_bytes,
                         (uint64_t)target -
                             source.summary.first_data_zone + 1U,
                         source.summary.little_endian, true);
            if (copy_exact_region(
                    source_fd, stage_fd,
                    (uint64_t)object->zones[logical] *
                        source.summary.zone_size,
                    (uint64_t)target * source.summary.zone_size,
                    source.summary.zone_size,
                    error, error_size) != 0) {
                result = -1;
                break;
            }
        }
    }

    if (result == 0) {
        for (size_t object_index = 0U;
             object_index < source.object_count; ++object_index) {
            if (write_relayout_inode(
                    source_fd, stage_fd, &source.summary,
                    &source.objects[object_index], &pointer_cursor,
                    new_zmap, source.zmap_bytes,
                    error, error_size) != 0) {
                result = -1;
                break;
            }
        }
    }

    if (result == 0) {
        const uint64_t zmap_offset =
            (uint64_t)(2U + source.summary.imap_blocks) *
            source.summary.block_size;
        if (ld_pwrite_full(stage_fd, new_zmap, source.zmap_bytes,
                           zmap_offset) != (ssize_t)source.zmap_bytes) {
            if (error != NULL && error_size != 0U)
                (void)snprintf(error, error_size,
                               "cannot write staged Minix zone bitmap: %s",
                               strerror(errno));
            result = -1;
        }
    }
    free(new_zmap);

    if (result == 0 && fsync(stage_fd) != 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size,
                           "cannot sync Minix stage: %s", strerror(errno));
        result = -1;
    }

stage_done:
    if (stage_fd >= 0)
        (void)close(stage_fd);

    if (result == 0 &&
        minix_verify_layout(stage_path, growth, growth_percent,
                            error, error_size) != 0)
        result = -1;

    if (result == 0) {
        int verify_fd = open(stage_path, O_RDONLY | O_CLOEXEC);
        MinixRelayoutCatalogue staged;
        if (verify_fd < 0 ||
            relayout_catalogue_load(verify_fd, &staged,
                                    error, error_size) != 0) {
            if (verify_fd >= 0)
                (void)close(verify_fd);
            result = -1;
        } else {
            result = compare_relayout_payloads(
                source_fd, &source, verify_fd, &staged,
                error, error_size);
            relayout_catalogue_free(&staged);
            (void)close(verify_fd);
        }
    }

    if (result != 0)
        (void)unlink(stage_path);
    else if (commit_bytes != NULL)
        *commit_bytes = source.filesystem_bytes;

    relayout_catalogue_free(&source);
    (void)close(source_fd);
    if (result == 0 && error != NULL && error_size != 0U)
        error[0] = '\0';
    return result;
}

bool minix_probe(const char *path)
{
    MinixSummary summary;
    return minix_read_summary(path, &summary, NULL, 0U) == 0;
}

const char *minix_variant_name(const MinixSummary *summary)
{
    if (summary == NULL)
        return "unknown";
    switch (summary->magic) {
    case MINIX_V1_MAGIC:
        return "v1";
    case MINIX_V1_30_MAGIC:
        return "v1-30char";
    case MINIX_V2_MAGIC:
        return "v2";
    case MINIX_V2_30_MAGIC:
        return "v2-30char";
    case MINIX_V3_MAGIC:
        return "v3";
    default:
        return "unknown";
    }
}

const char *minix_byte_order_name(const MinixSummary *summary)
{
    if (summary == NULL)
        return "unknown";
    return summary->little_endian ? "little" : "big";
}
