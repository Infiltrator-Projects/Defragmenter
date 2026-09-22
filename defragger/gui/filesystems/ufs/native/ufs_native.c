// SPDX-License-Identifier: GPL-3.0-or-later
#include "ufs_native.h"

#include "ld_io.h"
#include "ld_stop.h"
#include "infiltratr/arithmetic.h"
#include "infiltratr/core.h"
#include "infiltratr/endian.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define UFS_WINDOW_BYTES 8192U
#define UFS_MIN_WINDOW 512U
#define UFS_DISK_STRUCT_BYTES 1376U
#define UFS_DISK_MAGIC_OFFSET 1372U
#define UFS_DISK_SBLKNO_OFFSET 8U
#define UFS_DISK_CBLKNO_OFFSET 12U
#define UFS_DISK_IBLKNO_OFFSET 16U
#define UFS_DISK_DBLKNO_OFFSET 20U
#define UFS_DISK_OLD_CGOFFSET_OFFSET 24U
#define UFS_DISK_OLD_CGMASK_OFFSET 28U
#define UFS_DISK_OLD_SIZE_OFFSET 36U
#define UFS_DISK_OLD_DSIZE_OFFSET 40U
#define UFS_DISK_BSIZE_OFFSET 48U
#define UFS_DISK_FSIZE_OFFSET 52U
#define UFS_DISK_FRAG_OFFSET 56U
#define UFS_DISK_CGSIZE_OFFSET 160U
#define UFS_DISK_IPG_OFFSET 184U
#define UFS_DISK_FPG_OFFSET 188U
#define UFS_DISK_NCG_OFFSET 44U
#define UFS_DISK_NINDIR_OFFSET 116U
#define UFS_DISK_INOPB_OFFSET 120U
#define UFS_DISK_OLD_CSTOTAL_NBFREE_OFFSET 196U
#define UFS_DISK_OLD_CSTOTAL_NFFREE_OFFSET 204U
#define UFS_DISK_CLEAN_OFFSET 209U
#define UFS_DISK_CSTOTAL_NBFREE_OFFSET 1016U
#define UFS_DISK_CSTOTAL_NFFREE_OFFSET 1032U
#define UFS_DISK_SIZE_OFFSET 1080U
#define UFS_DISK_DSIZE_OFFSET 1088U
#define UFS_DISK_PENDINGBLOCKS_OFFSET 1104U
#define UFS_DISK_PENDINGINODES_OFFSET 1112U
#define UFS_DISK_SNAPINUM_OFFSET 1116U
#define UFS_DISK_SNAPINUM_COUNT 20U
#define UFS_DISK_METACKHASH_OFFSET 1308U
#define UFS_DISK_FLAGS_OFFSET 1312U
#define UFS_DISK_CONTIGSUMSIZE_OFFSET 1316U

#define UFS_CG_MIN_BYTES 168U
#define UFS_CG_CS_NBFREE_OFFSET 28U
#define UFS_CG_CS_NFFREE_OFFSET 36U
#define UFS_CG_IUSEDOFF_OFFSET 92U
#define UFS_CG_CLUSTERSUMOFF_OFFSET 104U
#define UFS_CG_CLUSTEROFF_OFFSET 108U
#define UFS_CG_NCLUSTERBLKS_OFFSET 112U
#define UFS_CG_MAGIC_OFFSET 4U
#define UFS_CG_INDEX_OFFSET 12U
#define UFS_CG_NDBLK_OFFSET 20U
#define UFS_CG_FREEOFF_OFFSET 96U
#define UFS_CG_MAGIC 0x00090255U

static const uint64_t UFS_CANDIDATES[] = {65536U, 8192U, 0U, 262144U};

static const uint8_t UFS1_LE_MAGIC[4] = {0x54U, 0x19U, 0x01U, 0x00U};
static const uint8_t UFS1_BE_MAGIC[4] = {0x00U, 0x01U, 0x19U, 0x54U};
static const uint8_t UFS2_LE_MAGIC[4] = {0x19U, 0x01U, 0x54U, 0x19U};
static const uint8_t UFS2_BE_MAGIC[4] = {0x19U, 0x54U, 0x01U, 0x19U};

typedef struct {
    const uint8_t *magic;
    LdUfsVariant variant;
} UfsMagic;

static const UfsMagic UFS_MAGICS[] = {
    {UFS1_LE_MAGIC, LD_UFS_VARIANT_UFS1_LE},
    {UFS1_BE_MAGIC, LD_UFS_VARIANT_UFS1_BE},
    {UFS2_LE_MAGIC, LD_UFS_VARIANT_UFS2_LE},
    {UFS2_BE_MAGIC, LD_UFS_VARIANT_UFS2_BE},
};

static void ufs_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size != 0U)
        (void)snprintf(error, error_size, "%s", message);
}

static bool ufs_little_endian(LdUfsVariant variant)
{
    return variant == LD_UFS_VARIANT_UFS1_LE ||
           variant == LD_UFS_VARIANT_UFS2_LE;
}

static uint32_t read_u32(const uint8_t *data, bool little)
{
    return little ? infiltratr_load_le32(data) : infiltratr_load_be32(data);
}

static uint64_t read_u64(const uint8_t *data, bool little)
{
    return little ? infiltratr_load_le64(data) : infiltratr_load_be64(data);
}

static bool power_of_two_u32(uint32_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static void decode_ufs_allocation(const uint8_t *window, size_t length,
                                  size_t magic_position, LdUfsSummary *summary)
{
    if (magic_position < UFS_DISK_MAGIC_OFFSET)
        return;
    const size_t base = magic_position - UFS_DISK_MAGIC_OFFSET;
    if (base > length || length - base < UFS_DISK_STRUCT_BYTES)
        return;

    const bool little = ufs_little_endian(summary->variant);
    const bool ufs1 =
        summary->variant == LD_UFS_VARIANT_UFS1_LE ||
        summary->variant == LD_UFS_VARIANT_UFS1_BE;
    const uint32_t sblkno = read_u32(window + base + UFS_DISK_SBLKNO_OFFSET, little);
    const uint32_t cblkno = read_u32(window + base + UFS_DISK_CBLKNO_OFFSET, little);
    const uint32_t iblkno = read_u32(window + base + UFS_DISK_IBLKNO_OFFSET, little);
    const uint32_t dblkno = read_u32(window + base + UFS_DISK_DBLKNO_OFFSET, little);
    const int32_t old_cgoffset = (int32_t)read_u32(
        window + base + UFS_DISK_OLD_CGOFFSET_OFFSET, little);
    const int32_t old_cgmask = (int32_t)read_u32(
        window + base + UFS_DISK_OLD_CGMASK_OFFSET, little);
    const uint32_t block_size = read_u32(window + base + UFS_DISK_BSIZE_OFFSET, little);
    const uint32_t fragment_size = read_u32(window + base + UFS_DISK_FSIZE_OFFSET, little);
    const uint32_t fragments_per_block = read_u32(window + base + UFS_DISK_FRAG_OFFSET, little);
    const uint32_t cylinder_groups = read_u32(window + base + UFS_DISK_NCG_OFFSET, little);
    const uint32_t cylinder_group_size =
        read_u32(window + base + UFS_DISK_CGSIZE_OFFSET, little);
    const uint32_t inodes_per_group =
        read_u32(window + base + UFS_DISK_IPG_OFFSET, little);
    const uint32_t fragments_per_group =
        read_u32(window + base + UFS_DISK_FPG_OFFSET, little);
    const uint32_t indirects_per_block =
        read_u32(window + base + UFS_DISK_NINDIR_OFFSET, little);
    const uint32_t inodes_per_block =
        read_u32(window + base + UFS_DISK_INOPB_OFFSET, little);
    const uint64_t free_blocks = ufs1
        ? read_u32(window + base + UFS_DISK_OLD_CSTOTAL_NBFREE_OFFSET, little)
        : read_u64(window + base + UFS_DISK_CSTOTAL_NBFREE_OFFSET, little);
    const uint64_t free_fragments = ufs1
        ? read_u32(window + base + UFS_DISK_OLD_CSTOTAL_NFFREE_OFFSET, little)
        : read_u64(window + base + UFS_DISK_CSTOTAL_NFFREE_OFFSET, little);
    const uint64_t filesystem_fragments = ufs1
        ? read_u32(window + base + UFS_DISK_OLD_SIZE_OFFSET, little)
        : read_u64(window + base + UFS_DISK_SIZE_OFFSET, little);
    const uint64_t data_fragments = ufs1
        ? read_u32(window + base + UFS_DISK_OLD_DSIZE_OFFSET, little)
        : read_u64(window + base + UFS_DISK_DSIZE_OFFSET, little);

    if (!power_of_two_u32(block_size) || !power_of_two_u32(fragment_size) ||
        block_size < 4096U || fragment_size < 512U || fragment_size > block_size ||
        fragments_per_block == 0U || fragments_per_block > 8U ||
        fragment_size > UINT32_MAX / fragments_per_block ||
        fragment_size * fragments_per_block != block_size ||
        data_fragments == 0U || filesystem_fragments < data_fragments)
        return;

    const uint32_t pointer_size = ufs1 ? 4U : 8U;
    const uint32_t inode_size = ufs1 ? 128U : 256U;
    if (indirects_per_block != block_size / pointer_size ||
        inodes_per_block != block_size / inode_size)
        return;

    uint64_t free_block_fragments = 0U;
    uint64_t free_data_fragments = 0U;
    if (!infiltratr_u64_multiply_checked(free_blocks, fragments_per_block,
                                         &free_block_fragments) ||
        !infiltratr_u64_add_checked(free_block_fragments, free_fragments,
                                    &free_data_fragments) ||
        free_data_fragments > filesystem_fragments)
        return;

    summary->allocation_totals_known = true;
    summary->block_size = block_size;
    summary->fragment_size = fragment_size;
    summary->fragments_per_block = fragments_per_block;
    summary->filesystem_fragments = filesystem_fragments;
    summary->data_fragments = data_fragments;
    summary->free_blocks = free_blocks;
    summary->free_fragments = free_fragments;
    summary->inode_block_fragment = iblkno;
    summary->first_data_fragment = dblkno;
    summary->inodes_per_block = inodes_per_block;
    summary->indirects_per_block = indirects_per_block;
    summary->old_cylinder_offset = old_cgoffset;
    summary->old_cylinder_mask = old_cgmask;
    summary->clean = window[base + UFS_DISK_CLEAN_OFFSET];
    summary->flags = read_u32(window + base + UFS_DISK_FLAGS_OFFSET, little);
    summary->metadata_check_hashes =
        read_u32(window + base + UFS_DISK_METACKHASH_OFFSET, little);
    summary->contiguous_summary_size =
        (int32_t)read_u32(window + base + UFS_DISK_CONTIGSUMSIZE_OFFSET, little);
    summary->pending_blocks =
        read_u64(window + base + UFS_DISK_PENDINGBLOCKS_OFFSET, little);
    summary->pending_inodes =
        read_u32(window + base + UFS_DISK_PENDINGINODES_OFFSET, little);
    summary->snapshot_count = 0U;
    for (uint32_t index = 0U; index < UFS_DISK_SNAPINUM_COUNT; ++index) {
        if (read_u32(window + base + UFS_DISK_SNAPINUM_OFFSET +
                     (size_t)index * 4U, little) != 0U)
            summary->snapshot_count++;
    }

    if (cylinder_groups == 0U || cylinder_group_size < UFS_CG_MIN_BYTES ||
        cylinder_group_size > block_size || fragments_per_group == 0U ||
        inodes_per_group == 0U || cblkno >= fragments_per_group ||
        iblkno >= fragments_per_group || dblkno >= fragments_per_group ||
        sblkno >= fragments_per_group ||
        (uint64_t)cylinder_groups * fragments_per_group < filesystem_fragments ||
        (uint64_t)(cylinder_groups - 1U) * fragments_per_group >= filesystem_fragments)
        return;

    summary->cylinder_geometry_known = true;
    summary->cylinder_groups = cylinder_groups;
    summary->cylinder_group_size = cylinder_group_size;
    summary->fragments_per_group = fragments_per_group;
    summary->inodes_per_group = inodes_per_group;
    summary->cylinder_block_fragment = cblkno;
}

static int ufs_size_bytes(int fd, uint64_t *bytes)
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

static int read_exact_at(int fd, void *buffer, size_t bytes, uint64_t offset,
                         char *error, size_t error_size, const char *message)
{
    const ssize_t count = ld_pread_full(fd, buffer, bytes, offset);
    if (count != (ssize_t)bytes) {
        if (count < 0 && error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "%s: %s", message, strerror(errno));
        else
            ufs_error(error, error_size, message);
        return -1;
    }
    return 0;
}

int ufs_read_summary(const char *path, LdUfsSummary *summary,
                     char *error, size_t error_size)
{
    if (path == NULL || summary == NULL) {
        ufs_error(error, error_size, "invalid UFS summary request");
        return -1;
    }
    memset(summary, 0, sizeof(*summary));

    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "open: %s", strerror(errno));
        return -1;
    }

    uint8_t window[UFS_WINDOW_BYTES];
    int result = -1;
    for (size_t candidate_index = 0U;
         candidate_index < sizeof(UFS_CANDIDATES) / sizeof(UFS_CANDIDATES[0]);
         ++candidate_index) {
        const uint64_t offset = UFS_CANDIDATES[candidate_index];
        const ssize_t count = ld_pread_full(fd, window, sizeof(window), offset);
        if (count < 0) {
            if (errno == EINVAL || errno == EIO)
                continue;
            if (error != NULL && error_size != 0U)
                (void)snprintf(error, error_size, "read: %s", strerror(errno));
            (void)close(fd);
            return -1;
        }
        if ((size_t)count < UFS_MIN_WINDOW)
            continue;

        for (size_t magic_index = 0U;
             magic_index < sizeof(UFS_MAGICS) / sizeof(UFS_MAGICS[0]);
             ++magic_index) {
            const size_t position = UFS_DISK_MAGIC_OFFSET;
            if ((size_t)count < UFS_DISK_STRUCT_BYTES ||
                memcmp(window + position, UFS_MAGICS[magic_index].magic, 4U) != 0)
                continue;
            summary->variant = UFS_MAGICS[magic_index].variant;
            summary->superblock_offset = offset;
            summary->magic_offset = offset + (uint64_t)position;
            decode_ufs_allocation(window, (size_t)count, position, summary);
            result = 0;
            break;
        }
        if (result == 0)
            break;
    }

    const int saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;

    if (result != 0) {
        ufs_error(error, error_size, "not a recognised UFS volume");
        return -1;
    }
    if (error != NULL && error_size != 0U)
        error[0] = '\0';
    return 0;
}

static void init_cells(LdUfsMapCell *cells, uint64_t cell_count,
                       uint64_t total_units, uint64_t filesystem_units)
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

static LdUfsMapCell *cell_for_fragment(LdUfsMapCell *cells, uint64_t cell_count,
                                       uint64_t fragment)
{
    if (cells == NULL || cell_count == 0U)
        return NULL;
    uint64_t lo = 0U;
    uint64_t hi = cell_count;
    while (lo < hi) {
        const uint64_t mid = lo + (hi - lo) / 2U;
        if (fragment < cells[mid].start)
            hi = mid;
        else if (fragment > cells[mid].end)
            lo = mid + 1U;
        else
            return &cells[mid];
    }
    return NULL;
}

static int ufs_scan_file_layout(const char *path,
                                LdUfsAnalysis *analysis,
                                LdUfsMapCell *cells,
                                uint64_t cell_count,
                                char *error, size_t error_size);

int ufs_analyse_allocation(const char *path, LdUfsAnalysis *analysis,
                           LdUfsMapCell *cells, uint64_t cell_count,
                           char *error, size_t error_size)
{
    if (path == NULL || analysis == NULL) {
        ufs_error(error, error_size, "invalid UFS allocation analysis request");
        return -1;
    }
    memset(analysis, 0, sizeof(*analysis));
    if (ufs_read_summary(path, &analysis->summary, error, error_size) != 0)
        return -1;

    LdUfsSummary *summary = &analysis->summary;
    if (!summary->allocation_totals_known || !summary->cylinder_geometry_known) {
        ufs_error(error, error_size,
                  "exact UFS allocation mapping requires validated cylinder-group geometry");
        return -1;
    }

    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "open: %s", strerror(errno));
        return -1;
    }
    uint64_t physical = 0U;
    if (ufs_size_bytes(fd, &physical) != 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "size: %s", strerror(errno));
        (void)close(fd);
        return -1;
    }
    if (summary->filesystem_fragments > UINT64_MAX / summary->fragment_size) {
        ufs_error(error, error_size, "UFS filesystem size overflows");
        (void)close(fd);
        return -1;
    }
    const uint64_t filesystem_bytes =
        summary->filesystem_fragments * summary->fragment_size;
    if (filesystem_bytes > physical) {
        ufs_error(error, error_size,
                  "UFS filesystem geometry exceeds the target size");
        (void)close(fd);
        return -1;
    }

    const uint64_t physical_units =
        (physical + summary->fragment_size - 1U) / summary->fragment_size;
    const uint64_t total_units =
        physical_units > summary->filesystem_fragments
            ? physical_units : summary->filesystem_fragments;
    analysis->physical_bytes = physical;
    analysis->filesystem_bytes = filesystem_bytes;
    analysis->total_units = total_units;
    init_cells(cells, cell_count, total_units, summary->filesystem_fragments);

    uint8_t *cg = malloc(summary->cylinder_group_size);
    if (cg == NULL) {
        ufs_error(error, error_size, "out of memory reading UFS cylinder group");
        (void)close(fd);
        return -1;
    }

    const bool little = ufs_little_endian(summary->variant);
    uint64_t free_fragments = 0U;
    uint64_t used_fragments = 0U;
    uint64_t covered = 0U;

    for (uint32_t group = 0U; group < summary->cylinder_groups; ++group) {
        const uint64_t group_base =
            (uint64_t)group * summary->fragments_per_group;
        if (group_base >= summary->filesystem_fragments) {
            free(cg);
            (void)close(fd);
            ufs_error(error, error_size, "UFS cylinder group begins beyond filesystem");
            return -1;
        }
        uint64_t group_start = group_base;
        if (summary->variant == LD_UFS_VARIANT_UFS1_LE ||
            summary->variant == LD_UFS_VARIANT_UFS1_BE) {
            const uint32_t mask = (uint32_t)summary->old_cylinder_mask;
            const uint64_t rotational = (uint64_t)(group & ~mask);
            const uint32_t cylinder_offset =
                (uint32_t)summary->old_cylinder_offset;
            if (summary->old_cylinder_offset < 0 ||
                (cylinder_offset != 0U &&
                 rotational > UINT64_MAX / cylinder_offset)) {
                free(cg);
                (void)close(fd);
                ufs_error(error, error_size,
                          "UFS1 cylinder-group rotational offset overflows");
                return -1;
            }
            group_start += rotational * cylinder_offset;
        }
        const uint64_t cg_fragment =
            group_start + summary->cylinder_block_fragment;
        uint64_t cg_offset = 0U;
        if (!infiltratr_u64_multiply_checked(cg_fragment,
                                             summary->fragment_size,
                                             &cg_offset)) {
            free(cg);
            (void)close(fd);
            ufs_error(error, error_size, "UFS cylinder group offset overflows");
            return -1;
        }
        if (cg_offset > physical ||
            summary->cylinder_group_size > physical - cg_offset ||
            read_exact_at(fd, cg, summary->cylinder_group_size, cg_offset,
                          error, error_size,
                          "cannot read UFS cylinder group") != 0) {
            free(cg);
            (void)close(fd);
            return -1;
        }

        if (read_u32(cg + UFS_CG_MAGIC_OFFSET, little) != UFS_CG_MAGIC ||
            read_u32(cg + UFS_CG_INDEX_OFFSET, little) != group) {
            free(cg);
            (void)close(fd);
            ufs_error(error, error_size, "invalid UFS cylinder-group header");
            return -1;
        }
        const uint32_t group_fragments =
            read_u32(cg + UFS_CG_NDBLK_OFFSET, little);
        const uint32_t free_offset =
            read_u32(cg + UFS_CG_FREEOFF_OFFSET, little);
        const uint64_t remaining =
            summary->filesystem_fragments - group_base;
        const uint64_t expected =
            remaining < summary->fragments_per_group
                ? remaining : summary->fragments_per_group;
        if (group_fragments != expected ||
            free_offset < UFS_CG_MIN_BYTES ||
            (uint64_t)free_offset + (group_fragments + 7U) / 8U >
                summary->cylinder_group_size) {
            free(cg);
            (void)close(fd);
            ufs_error(error, error_size, "invalid UFS cylinder-group allocation map");
            return -1;
        }

        const uint8_t *free_map = cg + free_offset;
        for (uint32_t local = 0U; local < group_fragments; ++local) {
            const uint64_t global = group_base + local;
            const bool is_free =
                (free_map[local >> 3U] &
                 (uint8_t)(1U << (local & 7U))) != 0U;
            LdUfsMapCell *cell =
                cell_for_fragment(cells, cell_count, global);
            if (is_free) {
                free_fragments++;
                if (cell != NULL)
                    cell->free_count++;
            } else {
                used_fragments++;
                if (cell != NULL)
                    cell->used_count++;
            }
        }
        covered += group_fragments;
    }

    free(cg);
    (void)close(fd);

    if (covered != summary->filesystem_fragments ||
        free_fragments + used_fragments != summary->filesystem_fragments) {
        ufs_error(error, error_size, "UFS cylinder groups do not cover filesystem");
        return -1;
    }
    const uint64_t recorded_free =
        summary->free_blocks * summary->fragments_per_block +
        summary->free_fragments;
    if (free_fragments != recorded_free) {
        ufs_error(error, error_size,
                  "UFS cylinder-group free map disagrees with superblock totals");
        return -1;
    }

    analysis->free_fragments_exact = free_fragments;
    analysis->used_fragments_exact = used_fragments;
    if (ufs_scan_file_layout(path, analysis, cells, cell_count,
                             error, error_size) != 0)
        return -1;
    if (error != NULL && error_size != 0U)
        error[0] = '\0';
    return 0;
}

bool ufs_probe(const char *path)
{
    LdUfsSummary summary;
    return ufs_read_summary(path, &summary, NULL, 0U) == 0;
}

const char *ufs_variant_name(const LdUfsSummary *summary)
{
    if (summary == NULL)
        return "unknown";
    switch (summary->variant) {
    case LD_UFS_VARIANT_UFS1_LE:
        return "ufs1-le";
    case LD_UFS_VARIANT_UFS1_BE:
        return "ufs1-be";
    case LD_UFS_VARIANT_UFS2_LE:
        return "ufs2-le";
    case LD_UFS_VARIANT_UFS2_BE:
        return "ufs2-be";
    default:
        return "unknown";
    }
}

const char *ufs_byte_order_name(const LdUfsSummary *summary)
{
    if (summary == NULL)
        return "unknown";
    return ufs_little_endian(summary->variant) ? "little" : "big";
}

unsigned int ufs_version(const LdUfsSummary *summary)
{
    if (summary == NULL)
        return 0U;
    return summary->variant == LD_UFS_VARIANT_UFS1_LE ||
                   summary->variant == LD_UFS_VARIANT_UFS1_BE
               ? 1U
               : 2U;
}

uint64_t ufs_recorded_data_bytes(const LdUfsSummary *summary)
{
    if (summary == NULL || !summary->allocation_totals_known)
        return 0U;
    return summary->data_fragments * summary->fragment_size;
}

uint64_t ufs_recorded_free_bytes(const LdUfsSummary *summary)
{
    if (summary == NULL || !summary->allocation_totals_known)
        return 0U;
    const uint64_t fragments =
        summary->free_blocks * summary->fragments_per_block + summary->free_fragments;
    return fragments * summary->fragment_size;
}

uint64_t ufs_recorded_used_bytes(const LdUfsSummary *summary)
{
    const uint64_t data_bytes = ufs_recorded_data_bytes(summary);
    const uint64_t free_bytes = ufs_recorded_free_bytes(summary);
    return data_bytes >= free_bytes ? data_bytes - free_bytes : 0U;
}


#define UFS_IFMT UINT16_C(0170000)
#define UFS_IFDIR UINT16_C(0040000)
#define UFS_IFREG UINT16_C(0100000)
#define UFS_NDADDR 12U
#define UFS_WRITER_STOPPED 130
#define UFS_WRITER_IO_BYTES (1024U * 1024U)
#define UFS_WRITER_MAX_BLOCK_REFS UINT64_C(16777216)
#define UFS_MAX_CONTIGSUM 16U

typedef struct {
    uint64_t logical_block;
    uint64_t physical_fragment;
    uint64_t pointer_offset;
    uint32_t span_fragments;
} UfsBlockRef;

typedef struct {
    uint64_t inode;
    uint64_t inode_offset;
    uint64_t size;
    uint32_t first_block;
    uint32_t block_count;
    uint32_t cylinder_group;
    uint16_t mode;
    bool directory;
    bool sparse;
    bool indirect;
    bool fragmented;
} UfsFileRecord;

typedef struct {
    UfsFileRecord *files;
    size_t file_count;
    size_t file_capacity;
    UfsBlockRef *blocks;
    size_t block_count;
    size_t block_capacity;
    uint8_t *free_bitmap;
    size_t bitmap_bytes;
    int fd;
    uint64_t physical_bytes;
    LdUfsSummary summary;
} UfsInventory;

static void write_u32(uint8_t *data, uint32_t value, bool little)
{
    if (little) infiltratr_store_le32(data, value);
    else infiltratr_store_be32(data, value);
}

static void write_u64(uint8_t *data, uint64_t value, bool little)
{
    if (little) infiltratr_store_le64(data, value);
    else infiltratr_store_be64(data, value);
}

static bool ufs1_variant(const LdUfsSummary *summary)
{
    return summary->variant == LD_UFS_VARIANT_UFS1_LE ||
           summary->variant == LD_UFS_VARIANT_UFS1_BE;
}

static uint64_t ufs_group_base(const LdUfsSummary *summary, uint32_t group)
{
    return (uint64_t)group * summary->fragments_per_group;
}

static int ufs_group_start(const LdUfsSummary *summary, uint32_t group,
                           uint64_t *start, char *error, size_t error_size)
{
    uint64_t value = ufs_group_base(summary, group);
    if (ufs1_variant(summary)) {
        if (summary->old_cylinder_offset < 0) {
            ufs_error(error, error_size, "UFS1 cylinder offset is negative");
            return -1;
        }
        const uint64_t rotational =
            (uint64_t)(group & ~(uint32_t)summary->old_cylinder_mask);
        uint64_t addition = 0U;
        if (!infiltratr_u64_multiply_checked(
                rotational, (uint32_t)summary->old_cylinder_offset, &addition) ||
            !infiltratr_u64_add_checked(value, addition, &value)) {
            ufs_error(error, error_size, "UFS cylinder-group start overflows");
            return -1;
        }
    }
    *start = value;
    return 0;
}

static bool bit_get(const uint8_t *bitmap, uint64_t bit)
{
    return (bitmap[bit >> 3U] &
            (uint8_t)(1U << (unsigned int)(bit & 7U))) != 0U;
}

static void bit_set(uint8_t *bitmap, uint64_t bit, bool value)
{
    const uint8_t mask = (uint8_t)(1U << (unsigned int)(bit & 7U));
    if (value) bitmap[bit >> 3U] |= mask;
    else bitmap[bit >> 3U] &= (uint8_t)~mask;
}

static int inventory_file_push(UfsInventory *inventory, UfsFileRecord file,
                               char *error, size_t error_size)
{
    if (!infiltratr_array_reserve((void **)&inventory->files,
                                  &inventory->file_capacity,
                                  sizeof(*inventory->files),
                                  inventory->file_count + 1U, 64U)) {
        ufs_error(error, error_size, "out of memory storing UFS file inventory");
        return -1;
    }
    inventory->files[inventory->file_count++] = file;
    return 0;
}

static int inventory_block_push(UfsInventory *inventory, UfsBlockRef block,
                                char *error, size_t error_size)
{
    if (inventory->block_count >= UFS_WRITER_MAX_BLOCK_REFS) {
        ufs_error(error, error_size,
                  "UFS file block inventory exceeds its bounded limit");
        return -1;
    }
    if (!infiltratr_array_reserve((void **)&inventory->blocks,
                                  &inventory->block_capacity,
                                  sizeof(*inventory->blocks),
                                  inventory->block_count + 1U, 256U)) {
        ufs_error(error, error_size, "out of memory storing UFS file block inventory");
        return -1;
    }
    inventory->blocks[inventory->block_count++] = block;
    return 0;
}

static void inventory_free(UfsInventory *inventory)
{
    if (inventory == NULL) return;
    if (inventory->fd >= 0) (void)close(inventory->fd);
    free(inventory->files);
    free(inventory->blocks);
    free(inventory->free_bitmap);
    memset(inventory, 0, sizeof(*inventory));
    inventory->fd = -1;
}

static int read_group(const UfsInventory *inventory, uint32_t group,
                      uint8_t *raw, uint32_t *group_fragments,
                      uint32_t *iused_offset, uint32_t *free_offset,
                      char *error, size_t error_size)
{
    const LdUfsSummary *summary = &inventory->summary;
    uint64_t start = 0U;
    if (ufs_group_start(summary, group, &start, error, error_size) != 0)
        return -1;
    uint64_t cg_fragment = 0U;
    if (!infiltratr_u64_add_checked(start, summary->cylinder_block_fragment,
                                    &cg_fragment) ||
        cg_fragment >= summary->filesystem_fragments) {
        ufs_error(error, error_size, "UFS cylinder-group block is out of range");
        return -1;
    }
    uint64_t offset = 0U;
    if (!infiltratr_u64_multiply_checked(cg_fragment, summary->fragment_size,
                                         &offset) ||
        offset > inventory->physical_bytes ||
        summary->cylinder_group_size > inventory->physical_bytes - offset ||
        read_exact_at(inventory->fd, raw, summary->cylinder_group_size, offset,
                      error, error_size, "cannot read UFS cylinder group") != 0)
        return -1;

    const bool little = ufs_little_endian(summary->variant);
    if (read_u32(raw + UFS_CG_MAGIC_OFFSET, little) != UFS_CG_MAGIC ||
        read_u32(raw + UFS_CG_INDEX_OFFSET, little) != group) {
        ufs_error(error, error_size, "invalid UFS cylinder-group header");
        return -1;
    }
    const uint32_t fragments = read_u32(raw + UFS_CG_NDBLK_OFFSET, little);
    const uint64_t base = ufs_group_base(summary, group);
    if (base >= summary->filesystem_fragments) {
        ufs_error(error, error_size, "UFS cylinder-group base is out of range");
        return -1;
    }
    const uint64_t remaining = summary->filesystem_fragments - base;
    const uint64_t expected = remaining < summary->fragments_per_group
        ? remaining : summary->fragments_per_group;
    const uint32_t iused = read_u32(raw + UFS_CG_IUSEDOFF_OFFSET, little);
    const uint32_t freeoff = read_u32(raw + UFS_CG_FREEOFF_OFFSET, little);
    const uint64_t inode_map_bytes =
        ((uint64_t)summary->inodes_per_group + 7U) / 8U;
    const uint64_t free_map_bytes = (expected + 7U) / 8U;
    if (fragments != expected || iused < UFS_CG_MIN_BYTES ||
        freeoff < UFS_CG_MIN_BYTES ||
        (uint64_t)iused + inode_map_bytes > summary->cylinder_group_size ||
        (uint64_t)freeoff + free_map_bytes > summary->cylinder_group_size) {
        ufs_error(error, error_size, "invalid UFS cylinder-group bitmap offsets");
        return -1;
    }
    *group_fragments = fragments;
    *iused_offset = iused;
    *free_offset = freeoff;
    return 0;
}

static int inventory_load_free_bitmap(UfsInventory *inventory,
                                      char *error, size_t error_size)
{
    if (inventory->summary.filesystem_fragments > (uint64_t)SIZE_MAX * 8U) {
        ufs_error(error, error_size, "UFS allocation bitmap is too large");
        return -1;
    }
    inventory->bitmap_bytes =
        (size_t)((inventory->summary.filesystem_fragments + 7U) / 8U);
    inventory->free_bitmap = calloc(
        inventory->bitmap_bytes == 0U ? 1U : inventory->bitmap_bytes, 1U);
    uint8_t *cg = malloc(inventory->summary.cylinder_group_size);
    if (inventory->free_bitmap == NULL || cg == NULL) {
        free(cg);
        ufs_error(error, error_size, "out of memory loading UFS allocation maps");
        return -1;
    }
    uint64_t free_fragments = 0U;
    for (uint32_t group = 0U; group < inventory->summary.cylinder_groups; ++group) {
        uint32_t fragments = 0U, iused = 0U, freeoff = 0U;
        if (read_group(inventory, group, cg, &fragments, &iused, &freeoff,
                       error, error_size) != 0) {
            free(cg);
            return -1;
        }
        (void)iused;
        const uint64_t base = ufs_group_base(&inventory->summary, group);
        for (uint32_t local = 0U; local < fragments; ++local) {
            if ((cg[freeoff + (local >> 3U)] &
                 (uint8_t)(1U << (unsigned int)(local & 7U))) != 0U) {
                bit_set(inventory->free_bitmap, base + local, true);
                free_fragments++;
            }
        }
    }
    free(cg);
    const uint64_t recorded =
        inventory->summary.free_blocks * inventory->summary.fragments_per_block +
        inventory->summary.free_fragments;
    if (free_fragments != recorded) {
        ufs_error(error, error_size,
                  "UFS cylinder-group free maps disagree with superblock totals");
        return -1;
    }
    return 0;
}

static int read_daddr_at(const UfsInventory *inventory, uint64_t offset,
                         uint64_t *value, char *error, size_t error_size)
{
    uint8_t raw[8];
    const size_t bytes = ufs1_variant(&inventory->summary) ? 4U : 8U;
    if (offset > inventory->physical_bytes ||
        bytes > inventory->physical_bytes - offset ||
        read_exact_at(inventory->fd, raw, bytes, offset, error, error_size,
                      "cannot read UFS block pointer") != 0)
        return -1;
    const bool little = ufs_little_endian(inventory->summary.variant);
    if (bytes == 4U) {
        const uint32_t parsed = read_u32(raw, little);
        if (parsed > INT32_MAX) {
            ufs_error(error, error_size,
                      "UFS1 block pointer is negative or out of range");
            return -1;
        }
        *value = parsed;
    } else {
        const uint64_t parsed = read_u64(raw, little);
        if (parsed > INT64_MAX) {
            ufs_error(error, error_size,
                      "UFS2 block pointer is negative or out of range");
            return -1;
        }
        *value = parsed;
    }
    return 0;
}

static int inode_pointer(const UfsInventory *inventory, uint64_t inode_offset,
                         uint64_t logical_block, uint64_t *physical_fragment,
                         uint64_t *pointer_offset, bool *indirect,
                         char *error, size_t error_size)
{
    const bool ufs1 = ufs1_variant(&inventory->summary);
    const uint32_t pointer_size = ufs1 ? 4U : 8U;
    const uint64_t direct_base = inode_offset + (ufs1 ? 40U : 112U);
    const uint64_t indirect_base = inode_offset + (ufs1 ? 88U : 208U);
    if (logical_block < UFS_NDADDR) {
        *pointer_offset = direct_base + logical_block * pointer_size;
        return read_daddr_at(inventory, *pointer_offset, physical_fragment,
                             error, error_size);
    }

    *indirect = true;
    uint64_t remaining = logical_block - UFS_NDADDR;
    const uint64_t nindir = inventory->summary.indirects_per_block;
    uint64_t square = 0U, cube = 0U;
    if (!infiltratr_u64_multiply_checked(nindir, nindir, &square) ||
        !infiltratr_u64_multiply_checked(square, nindir, &cube)) {
        ufs_error(error, error_size, "UFS indirect geometry overflows");
        return -1;
    }
    unsigned level = 0U;
    uint64_t indices[3] = {0U, 0U, 0U};
    if (remaining < nindir) {
        level = 1U; indices[0] = remaining;
    } else {
        remaining -= nindir;
        if (remaining < square) {
            level = 2U;
            indices[0] = remaining / nindir;
            indices[1] = remaining % nindir;
        } else {
            remaining -= square;
            if (remaining >= cube) {
                ufs_error(error, error_size,
                          "UFS logical block exceeds triple-indirect address space");
                return -1;
            }
            level = 3U;
            indices[0] = remaining / square;
            remaining %= square;
            indices[1] = remaining / nindir;
            indices[2] = remaining % nindir;
        }
    }

    uint64_t current = 0U;
    const uint64_t root_offset = indirect_base + (level - 1U) * pointer_size;
    if (read_daddr_at(inventory, root_offset, &current, error, error_size) != 0)
        return -1;
    if (current == 0U) {
        *physical_fragment = 0U;
        *pointer_offset = root_offset;
        return 0;
    }
    for (unsigned depth = 0U; depth < level; ++depth) {
        if (current % inventory->summary.fragments_per_block != 0U ||
            current >= inventory->summary.filesystem_fragments ||
            inventory->summary.fragments_per_block >
                inventory->summary.filesystem_fragments - current) {
            ufs_error(error, error_size, "UFS indirect block address is invalid");
            return -1;
        }
        for (uint32_t fragment = 0U;
             fragment < inventory->summary.fragments_per_block; ++fragment) {
            if (bit_get(inventory->free_bitmap, current + fragment)) {
                ufs_error(error, error_size, "UFS indirect block is marked free");
                return -1;
            }
        }
        uint64_t block_offset = 0U;
        if (!infiltratr_u64_multiply_checked(
                current, inventory->summary.fragment_size, &block_offset)) {
            ufs_error(error, error_size, "UFS indirect block offset overflows");
            return -1;
        }
        const uint64_t entry_offset =
            block_offset + indices[depth] * pointer_size;
        uint64_t next = 0U;
        if (read_daddr_at(inventory, entry_offset, &next, error, error_size) != 0)
            return -1;
        if (depth + 1U == level) {
            *physical_fragment = next;
            *pointer_offset = entry_offset;
            return 0;
        }
        if (next == 0U) {
            *physical_fragment = 0U;
            *pointer_offset = entry_offset;
            return 0;
        }
        current = next;
    }
    ufs_error(error, error_size, "UFS indirect traversal failed");
    return -1;
}

static int inventory_collect_files(UfsInventory *inventory,
                                   char *error, size_t error_size)
{
    const LdUfsSummary *summary = &inventory->summary;
    uint8_t *cg = malloc(summary->cylinder_group_size);
    const size_t inode_size = ufs1_variant(summary) ? 128U : 256U;
    uint8_t inode[256];
    if (cg == NULL) {
        ufs_error(error, error_size, "out of memory scanning UFS inodes");
        return -1;
    }

    for (uint32_t group = 0U; group < summary->cylinder_groups; ++group) {
        uint32_t group_fragments = 0U, iused = 0U, freeoff = 0U;
        if (read_group(inventory, group, cg, &group_fragments, &iused, &freeoff,
                       error, error_size) != 0) {
            free(cg);
            return -1;
        }
        (void)group_fragments; (void)freeoff;
        uint64_t group_start = 0U;
        if (ufs_group_start(summary, group, &group_start, error, error_size) != 0) {
            free(cg);
            return -1;
        }
        for (uint32_t local = 0U; local < summary->inodes_per_group; ++local) {
            if ((cg[iused + (local >> 3U)] &
                 (uint8_t)(1U << (unsigned int)(local & 7U))) == 0U)
                continue;
            const uint64_t inode_number =
                (uint64_t)group * summary->inodes_per_group + local;
            const uint32_t inode_block = local / summary->inodes_per_block;
            const uint32_t inode_slot = local % summary->inodes_per_block;
            uint64_t inode_fragment = 0U, inode_offset = 0U;
            if (!infiltratr_u64_add_checked(
                    group_start, summary->inode_block_fragment, &inode_fragment) ||
                !infiltratr_u64_add_checked(
                    inode_fragment,
                    (uint64_t)inode_block * summary->fragments_per_block,
                    &inode_fragment) ||
                !infiltratr_u64_multiply_checked(
                    inode_fragment, summary->fragment_size, &inode_offset) ||
                !infiltratr_u64_add_checked(
                    inode_offset, (uint64_t)inode_slot * inode_size,
                    &inode_offset) ||
                inode_offset > inventory->physical_bytes ||
                inode_size > inventory->physical_bytes - inode_offset ||
                read_exact_at(inventory->fd, inode, inode_size, inode_offset,
                              error, error_size, "cannot read UFS inode") != 0) {
                free(cg);
                return -1;
            }
            const bool little = ufs_little_endian(summary->variant);
            const uint16_t mode = little
                ? infiltratr_load_le16(inode) : infiltratr_load_be16(inode);
            if (mode == 0U) continue;
            const uint16_t type = mode & UFS_IFMT;
            if (type != UFS_IFREG && type != UFS_IFDIR) continue;
            const uint64_t size = ufs1_variant(summary)
                ? read_u64(inode + 8U, little)
                : read_u64(inode + 16U, little);
            uint64_t logical_blocks = size / summary->block_size;
            if (size % summary->block_size != 0U) logical_blocks++;
            if (logical_blocks > UFS_WRITER_MAX_BLOCK_REFS ||
                inventory->block_count >
                    UFS_WRITER_MAX_BLOCK_REFS - logical_blocks) {
                free(cg);
                ufs_error(error, error_size,
                          "UFS inode block inventory exceeds its bounded limit");
                return -1;
            }
            UfsFileRecord file = {
                .inode = inode_number, .inode_offset = inode_offset, .size = size,
                .first_block = (uint32_t)inventory->block_count, .block_count = 0U,
                .cylinder_group = UINT32_MAX, .mode = mode,
                .directory = type == UFS_IFDIR, .sparse = false,
                .indirect = false, .fragmented = false,
            };
            bool previous = false;
            uint64_t previous_end = 0U;
            for (uint64_t lbn = 0U; lbn < logical_blocks; ++lbn) {
                uint64_t physical_fragment = 0U, pointer_offset = 0U;
                bool indirect = false;
                if (inode_pointer(inventory, inode_offset, lbn,
                                  &physical_fragment, &pointer_offset, &indirect,
                                  error, error_size) != 0) {
                    free(cg); return -1;
                }
                file.indirect = file.indirect || indirect;
                if (physical_fragment == 0U) {
                    file.sparse = true; previous = false; continue;
                }
                uint32_t span = summary->fragments_per_block;
                if (lbn + 1U == logical_blocks &&
                    size % summary->block_size != 0U) {
                    span = (uint32_t)((size % summary->block_size +
                        summary->fragment_size - 1U) / summary->fragment_size);
                }
                if (span == 0U ||
                    physical_fragment >= summary->filesystem_fragments ||
                    span > summary->filesystem_fragments - physical_fragment) {
                    free(cg);
                    ufs_error(error, error_size,
                              "UFS file data block address is out of range");
                    return -1;
                }
                for (uint32_t fragment = 0U; fragment < span; ++fragment) {
                    if (bit_get(inventory->free_bitmap,
                                physical_fragment + fragment)) {
                        free(cg);
                        ufs_error(error, error_size,
                                  "UFS inode points at a fragment marked free");
                        return -1;
                    }
                }
                if (previous && physical_fragment != previous_end)
                    file.fragmented = true;
                previous = true;
                previous_end = physical_fragment + span;
                if (inventory_block_push(
                        inventory,
                        (UfsBlockRef){lbn, physical_fragment, pointer_offset, span},
                        error, error_size) != 0) {
                    free(cg); return -1;
                }
                file.block_count++;
            }
            if (inventory_file_push(inventory, file, error, error_size) != 0) {
                free(cg); return -1;
            }
        }
    }
    free(cg);
    return 0;
}

static int inventory_load(const char *path, UfsInventory *inventory,
                          char *error, size_t error_size)
{
    memset(inventory, 0, sizeof(*inventory));
    inventory->fd = -1;
    if (ufs_read_summary(path, &inventory->summary, error, error_size) != 0)
        return -1;
    if (!inventory->summary.allocation_totals_known ||
        !inventory->summary.cylinder_geometry_known) {
        ufs_error(error, error_size,
                  "UFS exact inventory requires validated cylinder-group geometry");
        return -1;
    }
    inventory->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (inventory->fd < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "open UFS inventory: %s",
                           strerror(errno));
        return -1;
    }
    if (ufs_size_bytes(inventory->fd, &inventory->physical_bytes) != 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "size UFS inventory: %s",
                           strerror(errno));
        inventory_free(inventory);
        return -1;
    }
    uint64_t filesystem_bytes = 0U;
    if (!infiltratr_u64_multiply_checked(
            inventory->summary.filesystem_fragments,
            inventory->summary.fragment_size, &filesystem_bytes) ||
        filesystem_bytes > inventory->physical_bytes) {
        ufs_error(error, error_size,
                  "UFS filesystem geometry exceeds target capacity");
        inventory_free(inventory);
        return -1;
    }
    if (inventory_load_free_bitmap(inventory, error, error_size) != 0 ||
        inventory_collect_files(inventory, error, error_size) != 0) {
        inventory_free(inventory);
        return -1;
    }
    return 0;
}

static void mark_map_range(LdUfsMapCell *cells, uint64_t cell_count,
                           uint64_t start, uint64_t count,
                           bool fragmented, bool directory)
{
    if (cells == NULL || cell_count == 0U || count == 0U) return;
    const uint64_t end = start + count;
    for (uint64_t index = 0U; index < cell_count; ++index) {
        const uint64_t overlap_start =
            start > cells[index].start ? start : cells[index].start;
        const uint64_t cell_end = cells[index].end + 1U;
        const uint64_t overlap_end = end < cell_end ? end : cell_end;
        if (overlap_end <= overlap_start) continue;
        const uint64_t overlap = overlap_end - overlap_start;
        if (fragmented) cells[index].fragmented_count += overlap;
        if (directory) cells[index].directory_count += overlap;
    }
}

static int ufs_scan_file_layout(const char *path, LdUfsAnalysis *analysis,
                                LdUfsMapCell *cells, uint64_t cell_count,
                                char *error, size_t error_size)
{
    UfsInventory inventory;
    if (inventory_load(path, &inventory, error, error_size) != 0) return -1;
    for (size_t index = 0U; index < inventory.file_count; ++index) {
        const UfsFileRecord *file = &inventory.files[index];
        if (file->directory) {
            analysis->directories++;
            if (file->fragmented) analysis->fragmented_directories++;
        } else {
            analysis->regular_files++;
            if (file->fragmented) analysis->fragmented_files++;
            if (file->sparse) analysis->sparse_files++;
            if (file->indirect) analysis->indirect_files++;
        }
        for (uint32_t block = 0U; block < file->block_count; ++block) {
            const UfsBlockRef *reference =
                &inventory.blocks[file->first_block + block];
            mark_map_range(cells, cell_count, reference->physical_fragment,
                           reference->span_fragments, file->fragmented,
                           file->directory);
        }
    }
    inventory_free(&inventory);
    return 0;
}

static int writer_supported(const UfsInventory *inventory,
                            char *error, size_t error_size)
{
    const LdUfsSummary *summary = &inventory->summary;
    if (summary->clean == 0U || summary->flags != 0U ||
        summary->metadata_check_hashes != 0U || summary->pending_blocks != 0U ||
        summary->pending_inodes != 0U || summary->snapshot_count != 0U) {
        ufs_error(error, error_size,
                  "UFS writer requires a clean, non-journalled, snapshot-free filesystem without metadata check hashes or pending frees");
        return -1;
    }
    if (summary->contiguous_summary_size < 0 ||
        summary->contiguous_summary_size > (int32_t)UFS_MAX_CONTIGSUM) {
        ufs_error(error, error_size,
                  "UFS contiguous-cluster summary geometry is unsupported");
        return -1;
    }
    uint8_t *owned = calloc(
        inventory->bitmap_bytes == 0U ? 1U : inventory->bitmap_bytes, 1U);
    if (owned == NULL) {
        ufs_error(error, error_size,
                  "out of memory validating UFS data ownership");
        return -1;
    }
    int result = 0;
    for (size_t index = 0U; index < inventory->file_count && result == 0; ++index) {
        const UfsFileRecord *file = &inventory->files[index];
        if (file->directory || file->size == 0U) continue;
        if (file->sparse || file->size % summary->block_size != 0U ||
            file->block_count == 0U) {
            ufs_error(error, error_size,
                      "UFS writer requires non-sparse regular files with whole filesystem-block allocation");
            result = -1; break;
        }
        uint32_t cylinder = UINT32_MAX;
        for (uint32_t block = 0U; block < file->block_count && result == 0; ++block) {
            const UfsBlockRef *reference =
                &inventory->blocks[file->first_block + block];
            if (reference->span_fragments != summary->fragments_per_block ||
                reference->physical_fragment % summary->fragments_per_block != 0U) {
                ufs_error(error, error_size,
                          "UFS writer requires full-block aligned regular-file data");
                result = -1; break;
            }
            const uint32_t this_cylinder = (uint32_t)(
                reference->physical_fragment / summary->fragments_per_group);
            if (cylinder == UINT32_MAX) cylinder = this_cylinder;
            else if (cylinder != this_cylinder) {
                ufs_error(error, error_size,
                          "UFS writer currently requires each regular file to reside within one cylinder group");
                result = -1; break;
            }
            for (uint32_t fragment = 0U;
                 fragment < reference->span_fragments; ++fragment) {
                const uint64_t position =
                    reference->physical_fragment + fragment;
                if (bit_get(owned, position)) {
                    ufs_error(error, error_size,
                              "UFS writer found shared or overlapping regular-file data");
                    result = -1; break;
                }
                bit_set(owned, position, true);
            }
        }
    }
    free(owned);
    return result;
}

static bool run_is_clear(const uint8_t *blocked, uint64_t start, uint64_t count)
{
    for (uint64_t fragment = 0U; fragment < count; ++fragment)
        if (bit_get(blocked, start + fragment)) return false;
    return true;
}

static int choose_run(const LdUfsSummary *summary, uint8_t *blocked,
                      uint32_t group, uint64_t data_fragments,
                      uint64_t reserve_fragments, uint64_t *start_out,
                      char *error, size_t error_size)
{
    uint64_t span = 0U;
    if (!infiltratr_u64_add_checked(data_fragments, reserve_fragments, &span)) {
        ufs_error(error, error_size, "UFS placement span overflows");
        return -1;
    }
    const uint64_t base = ufs_group_base(summary, group);
    const uint64_t remaining = summary->filesystem_fragments - base;
    const uint64_t group_fragments = remaining < summary->fragments_per_group
        ? remaining : summary->fragments_per_group;
    const uint64_t end = base + group_fragments;
    for (uint64_t start = base; start < end;
         start += summary->fragments_per_block) {
        if (span > end - start || !run_is_clear(blocked, start, span))
            continue;
        for (uint64_t fragment = 0U; fragment < span; ++fragment)
            bit_set(blocked, start + fragment, true);
        *start_out = start;
        return 0;
    }
    ufs_error(error, error_size,
              "UFS writer cannot place a file contiguously with the requested reserve inside its cylinder group");
    return -1;
}

static int copy_bytes(int source_fd, int stage_fd, uint64_t source_offset,
                      uint64_t target_offset, uint64_t bytes,
                      char *error, size_t error_size)
{
    uint8_t *buffer = malloc(UFS_WRITER_IO_BYTES);
    if (buffer == NULL) {
        ufs_error(error, error_size, "out of memory copying UFS data");
        return -1;
    }
    int result = 0;
    for (uint64_t done = 0U; done < bytes;) {
        if (ld_stop_requested()) {
            result = UFS_WRITER_STOPPED; break;
        }
        const uint64_t remaining = bytes - done;
        const size_t take = remaining > UFS_WRITER_IO_BYTES
            ? UFS_WRITER_IO_BYTES : (size_t)remaining;
        if (ld_pread_full(source_fd, buffer, take, source_offset + done) !=
                (ssize_t)take ||
            ld_pwrite_full(stage_fd, buffer, take, target_offset + done) !=
                (ssize_t)take) {
            ufs_error(error, error_size, "short I/O copying UFS transaction data");
            result = -1; break;
        }
        done += take;
    }
    free(buffer);
    return result;
}

static int write_daddr_at(int fd, const LdUfsSummary *summary,
                          uint64_t offset, uint64_t value,
                          char *error, size_t error_size)
{
    uint8_t raw[8] = {0};
    const bool little = ufs_little_endian(summary->variant);
    const size_t bytes = ufs1_variant(summary) ? 4U : 8U;
    if (bytes == 4U) {
        if (value > INT32_MAX) {
            ufs_error(error, error_size,
                      "UFS1 relocated block address exceeds its on-disk field");
            return -1;
        }
        write_u32(raw, (uint32_t)value, little);
    } else {
        if (value > INT64_MAX) {
            ufs_error(error, error_size,
                      "UFS2 relocated block address exceeds its on-disk field");
            return -1;
        }
        write_u64(raw, value, little);
    }
    if (ld_pwrite_full(fd, raw, bytes, offset) != (ssize_t)bytes) {
        ufs_error(error, error_size, "cannot rewrite UFS file block pointer");
        return -1;
    }
    return 0;
}

static int update_cluster_summaries(uint8_t *cg, const LdUfsSummary *summary,
                                    uint32_t group_fragments, uint32_t freeoff,
                                    char *error, size_t error_size)
{
    if (summary->contiguous_summary_size <= 0) return 0;
    const bool little = ufs_little_endian(summary->variant);
    const uint32_t sumoff =
        read_u32(cg + UFS_CG_CLUSTERSUMOFF_OFFSET, little);
    const uint32_t clusteroff =
        read_u32(cg + UFS_CG_CLUSTEROFF_OFFSET, little);
    const uint32_t cluster_blocks =
        read_u32(cg + UFS_CG_NCLUSTERBLKS_OFFSET, little);
    const uint32_t expected_blocks =
        group_fragments / summary->fragments_per_block;
    const uint32_t limit = (uint32_t)summary->contiguous_summary_size;
    const uint64_t sum_bytes = (uint64_t)(limit + 1U) * 4U;
    const uint64_t map_bytes = ((uint64_t)cluster_blocks + 7U) / 8U;
    if (cluster_blocks != expected_blocks || sumoff < UFS_CG_MIN_BYTES ||
        clusteroff < UFS_CG_MIN_BYTES ||
        (uint64_t)sumoff + sum_bytes > summary->cylinder_group_size ||
        (uint64_t)clusteroff + map_bytes > summary->cylinder_group_size) {
        ufs_error(error, error_size, "invalid UFS free-cluster summary offsets");
        return -1;
    }
    memset(cg + sumoff, 0, (size_t)sum_bytes);
    memset(cg + clusteroff, 0, (size_t)map_bytes);
    for (uint32_t block = 0U; block < cluster_blocks; ++block) {
        bool free_block = true;
        const uint32_t first = block * summary->fragments_per_block;
        for (uint32_t fragment = 0U;
             fragment < summary->fragments_per_block; ++fragment) {
            const uint32_t local = first + fragment;
            if ((cg[freeoff + (local >> 3U)] &
                 (uint8_t)(1U << (unsigned int)(local & 7U))) == 0U) {
                free_block = false; break;
            }
        }
        if (free_block)
            cg[clusteroff + (block >> 3U)] |=
                (uint8_t)(1U << (unsigned int)(block & 7U));
    }
    uint32_t run = 0U;
    uint32_t sums[UFS_MAX_CONTIGSUM + 1U] = {0U};
    for (uint32_t block = 0U; block < cluster_blocks; ++block) {
        const bool free_block =
            (cg[clusteroff + (block >> 3U)] &
             (uint8_t)(1U << (unsigned int)(block & 7U))) != 0U;
        if (free_block) run++;
        else if (run != 0U) {
            sums[run > limit ? limit : run]++; run = 0U;
        }
    }
    if (run != 0U) sums[run > limit ? limit : run]++;
    for (uint32_t index = 0U; index <= limit; ++index)
        write_u32(cg + sumoff + (size_t)index * 4U, sums[index], little);
    return 0;
}

static int publish_group_bitmap(int stage_fd, const UfsInventory *source,
                                const uint8_t *final_free, uint32_t group,
                                char *error, size_t error_size)
{
    uint8_t *cg = malloc(source->summary.cylinder_group_size);
    if (cg == NULL) {
        ufs_error(error, error_size, "out of memory updating UFS cylinder group");
        return -1;
    }
    uint32_t group_fragments = 0U, iused = 0U, freeoff = 0U;
    int result = -1;
    if (read_group(source, group, cg, &group_fragments, &iused, &freeoff,
                   error, error_size) != 0) goto done;
    (void)iused;
    const bool little = ufs_little_endian(source->summary.variant);
    uint32_t before_nbfree = 0U, before_nffree = 0U;
    for (uint32_t block = 0U; block < group_fragments;
         block += source->summary.fragments_per_block) {
        uint32_t free_count = 0U;
        const uint32_t take = source->summary.fragments_per_block <
                group_fragments - block
            ? source->summary.fragments_per_block : group_fragments - block;
        for (uint32_t fragment = 0U; fragment < take; ++fragment)
            if ((cg[freeoff + ((block + fragment) >> 3U)] &
                 (uint8_t)(1U << (unsigned int)((block + fragment) & 7U))) != 0U)
                free_count++;
        if (take == source->summary.fragments_per_block && free_count == take)
            before_nbfree++;
        else before_nffree += free_count;
    }
    const uint64_t base = ufs_group_base(&source->summary, group);
    for (uint32_t local = 0U; local < group_fragments; ++local) {
        const uint8_t mask = (uint8_t)(1U << (unsigned int)(local & 7U));
        if (bit_get(final_free, base + local))
            cg[freeoff + (local >> 3U)] |= mask;
        else cg[freeoff + (local >> 3U)] &= (uint8_t)~mask;
    }
    uint32_t after_nbfree = 0U, after_nffree = 0U;
    for (uint32_t block = 0U; block < group_fragments;
         block += source->summary.fragments_per_block) {
        uint32_t free_count = 0U;
        const uint32_t take = source->summary.fragments_per_block <
                group_fragments - block
            ? source->summary.fragments_per_block : group_fragments - block;
        for (uint32_t fragment = 0U; fragment < take; ++fragment)
            if ((cg[freeoff + ((block + fragment) >> 3U)] &
                 (uint8_t)(1U << (unsigned int)((block + fragment) & 7U))) != 0U)
                free_count++;
        if (take == source->summary.fragments_per_block && free_count == take)
            after_nbfree++;
        else after_nffree += free_count;
    }
    if (before_nbfree != after_nbfree || before_nffree != after_nffree ||
        read_u32(cg + UFS_CG_CS_NBFREE_OFFSET, little) != after_nbfree ||
        read_u32(cg + UFS_CG_CS_NFFREE_OFFSET, little) != after_nffree) {
        ufs_error(error, error_size,
                  "UFS relocation would change cylinder-group free-block/fragment accounting");
        goto done;
    }
    if (update_cluster_summaries(cg, &source->summary, group_fragments, freeoff,
                                 error, error_size) != 0) goto done;
    uint64_t group_start = 0U, cg_fragment = 0U, offset = 0U;
    if (ufs_group_start(&source->summary, group, &group_start,
                        error, error_size) != 0 ||
        !infiltratr_u64_add_checked(group_start,
            source->summary.cylinder_block_fragment, &cg_fragment) ||
        !infiltratr_u64_multiply_checked(cg_fragment,
            source->summary.fragment_size, &offset) ||
        ld_pwrite_full(stage_fd, cg, source->summary.cylinder_group_size,
                       offset) != (ssize_t)source->summary.cylinder_group_size) {
        if (error != NULL && error[0] == '\0')
            ufs_error(error, error_size,
                      "cannot publish staged UFS cylinder-group bitmap");
        goto done;
    }
    result = 0;
done:
    free(cg);
    return result;
}

static int payload_compare(const UfsInventory *before,
                           const UfsInventory *after,
                           char *error, size_t error_size)
{
    if (before->file_count != after->file_count) {
        ufs_error(error, error_size, "UFS staged inode inventory changed");
        return -1;
    }
    uint8_t *left = malloc(before->summary.block_size);
    uint8_t *right = malloc(after->summary.block_size);
    if (left == NULL || right == NULL) {
        free(left); free(right);
        ufs_error(error, error_size, "out of memory verifying UFS file payloads");
        return -1;
    }
    int result = 0;
    for (size_t file_index = 0U;
         file_index < before->file_count && result == 0; ++file_index) {
        const UfsFileRecord *a = &before->files[file_index];
        const UfsFileRecord *b = &after->files[file_index];
        if (a->inode != b->inode || a->size != b->size ||
            a->directory != b->directory || a->block_count != b->block_count) {
            ufs_error(error, error_size, "UFS staged file identity changed");
            result = -1; break;
        }
        if (a->directory) continue;
        for (uint32_t block = 0U; block < a->block_count; ++block) {
            const UfsBlockRef *lr = &before->blocks[a->first_block + block];
            const UfsBlockRef *rr = &after->blocks[b->first_block + block];
            if (lr->logical_block != rr->logical_block ||
                lr->span_fragments != rr->span_fragments) {
                ufs_error(error, error_size,
                          "UFS staged logical file mapping changed");
                result = -1; break;
            }
            const uint64_t lo = lr->physical_fragment *
                (uint64_t)before->summary.fragment_size;
            const uint64_t ro = rr->physical_fragment *
                (uint64_t)after->summary.fragment_size;
            const size_t bytes =
                (size_t)lr->span_fragments * before->summary.fragment_size;
            if (ld_pread_full(before->fd, left, bytes, lo) != (ssize_t)bytes ||
                ld_pread_full(after->fd, right, bytes, ro) != (ssize_t)bytes ||
                memcmp(left, right, bytes) != 0) {
                ufs_error(error, error_size,
                          "UFS file payload changed during staging");
                result = -1; break;
            }
        }
    }
    free(left); free(right);
    return result;
}

int ufs_verify_layout(const char *path, bool growth, unsigned growth_percent,
                      char *error, size_t error_size)
{
    if (growth && growth_percent != 10U) {
        ufs_error(error, error_size,
                  "UFS Growth Defrag requires exactly 10 percent reserve");
        return -1;
    }
    UfsInventory inventory;
    if (inventory_load(path, &inventory, error, error_size) != 0) return -1;
    int result = 0;
    for (size_t index = 0U; index < inventory.file_count && result == 0; ++index) {
        const UfsFileRecord *file = &inventory.files[index];
        if (file->directory || file->block_count == 0U) continue;
        bool previous = false;
        uint64_t end = 0U, allocated = 0U;
        for (uint32_t block = 0U; block < file->block_count; ++block) {
            const UfsBlockRef *reference =
                &inventory.blocks[file->first_block + block];
            if (previous && reference->physical_fragment != end) {
                if (error != NULL && error_size != 0U)
                    (void)snprintf(error, error_size,
                        "UFS inode %" PRIu64 " remains fragmented", file->inode);
                result = -1; break;
            }
            previous = true;
            end = reference->physical_fragment + reference->span_fragments;
            allocated += reference->span_fragments;
        }
        if (result != 0 || !growth) continue;
        const uint64_t reserve = (allocated * growth_percent + 99U) / 100U;
        if (end > inventory.summary.filesystem_fragments ||
            reserve > inventory.summary.filesystem_fragments - end) {
            ufs_error(error, error_size,
                      "UFS growth reserve lies outside the filesystem");
            result = -1; break;
        }
        for (uint64_t fragment = 0U; fragment < reserve; ++fragment) {
            if (!bit_get(inventory.free_bitmap, end + fragment)) {
                if (error != NULL && error_size != 0U)
                    (void)snprintf(error, error_size,
                        "UFS inode %" PRIu64
                        " lacks its exact 10 percent post-file reserve", file->inode);
                result = -1; break;
            }
        }
    }
    if (result == 0) {
        LdUfsAnalysis analysis;
        if (ufs_analyse_allocation(path, &analysis, NULL, 0U,
                                   error, error_size) != 0)
            result = -1;
        else if (analysis.fragmented_files != 0U) {
            ufs_error(error, error_size,
                      "UFS independent analyser still reports fragmented regular files");
            result = -1;
        }
    }
    inventory_free(&inventory);
    return result;
}

int ufs_build_stage(const char *source_path, const char *stage_path,
                    bool growth, unsigned growth_percent, bool live_updates,
                    uint64_t *commit_bytes, char *error, size_t error_size)
{
    if (growth && growth_percent != 10U) {
        ufs_error(error, error_size,
                  "UFS Growth Defrag requires exactly 10 percent reserve");
        return -1;
    }
    UfsInventory source;
    if (inventory_load(source_path, &source, error, error_size) != 0) return -1;
    if (writer_supported(&source, error, error_size) != 0) {
        inventory_free(&source); return -1;
    }
    uint64_t filesystem_bytes = 0U;
    if (!infiltratr_u64_multiply_checked(source.summary.filesystem_fragments,
        source.summary.fragment_size, &filesystem_bytes)) {
        inventory_free(&source);
        ufs_error(error, error_size, "UFS filesystem byte count overflows");
        return -1;
    }
    int flags = O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int stage_fd = open(stage_path, flags, 0600);
    if (stage_fd < 0 || ftruncate(stage_fd, (off_t)filesystem_bytes) != 0) {
        if (stage_fd >= 0) (void)close(stage_fd);
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size,
                           "cannot create UFS recovery stage: %s", strerror(errno));
        inventory_free(&source); return -1;
    }
    int result = copy_bytes(source.fd, stage_fd, 0U, 0U, filesystem_bytes,
                            error, error_size);
    if (result != 0) goto done;
    uint8_t *final_free = malloc(source.bitmap_bytes);
    uint8_t *blocked = calloc(
        source.bitmap_bytes == 0U ? 1U : source.bitmap_bytes, 1U);
    if (final_free == NULL || blocked == NULL) {
        free(final_free); free(blocked);
        ufs_error(error, error_size, "out of memory planning UFS relocation");
        result = -1; goto done;
    }
    memcpy(final_free, source.free_bitmap, source.bitmap_bytes);
    for (uint64_t fragment = 0U;
         fragment < source.summary.filesystem_fragments; ++fragment)
        bit_set(blocked, fragment, !bit_get(source.free_bitmap, fragment));
    for (size_t fi = 0U; fi < source.file_count; ++fi) {
        const UfsFileRecord *file = &source.files[fi];
        if (file->directory) continue;
        for (uint32_t b = 0U; b < file->block_count; ++b) {
            const UfsBlockRef *ref = &source.blocks[file->first_block + b];
            for (uint32_t fr = 0U; fr < ref->span_fragments; ++fr) {
                bit_set(final_free, ref->physical_fragment + fr, true);
                bit_set(blocked, ref->physical_fragment + fr, false);
            }
        }
    }
    size_t live_sequence = 0U;
    for (size_t fi = 0U; fi < source.file_count && result == 0; ++fi) {
        UfsFileRecord *file = &source.files[fi];
        if (file->directory || file->block_count == 0U) continue;
        const uint64_t data_fragments =
            (uint64_t)file->block_count * source.summary.fragments_per_block;
        const uint64_t reserve = growth
            ? (data_fragments * growth_percent + 99U) / 100U : 0U;
        const UfsBlockRef *first = &source.blocks[file->first_block];
        const uint32_t group = (uint32_t)(
            first->physical_fragment / source.summary.fragments_per_group);
        uint64_t destination = 0U;
        if (choose_run(&source.summary, blocked, group, data_fragments, reserve,
                       &destination, error, error_size) != 0) {
            result = -1; break;
        }
        uint64_t cursor = destination;
        for (uint32_t b = 0U; b < file->block_count; ++b) {
            UfsBlockRef *ref = &source.blocks[file->first_block + b];
            const uint64_t so = ref->physical_fragment *
                (uint64_t)source.summary.fragment_size;
            const uint64_t to = cursor *
                (uint64_t)source.summary.fragment_size;
            const int copied = copy_bytes(source.fd, stage_fd, so, to,
                source.summary.block_size, error, error_size);
            if (copied != 0) { result = copied; break; }
            for (uint32_t fr = 0U; fr < source.summary.fragments_per_block; ++fr)
                bit_set(final_free, cursor + fr, false);
            if (write_daddr_at(stage_fd, &source.summary, ref->pointer_offset,
                               cursor, error, error_size) != 0) {
                result = -1; break;
            }
            cursor += source.summary.fragments_per_block;
        }
        if (result == 0 && live_updates) {
            (void)printf(
                "@@LIVE_RANGES {\"ranges\":[[%" PRIu64 ",%" PRIu64
                ",1]],\"sequence\":%zu}\n",
                destination, destination + data_fragments, ++live_sequence);
            (void)fflush(stdout);
        }
    }
    if (result == 0) {
        for (uint32_t group = 0U;
             group < source.summary.cylinder_groups; ++group) {
            if (publish_group_bitmap(stage_fd, &source, final_free, group,
                                     error, error_size) != 0) {
                result = -1; break;
            }
        }
    }
    free(final_free); free(blocked);
    if (result == 0 && fsync(stage_fd) != 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size,
                           "cannot sync UFS recovery stage: %s", strerror(errno));
        result = -1;
    }
done:
    (void)close(stage_fd);
    if (result == 0) {
        UfsInventory after;
        if (inventory_load(stage_path, &after, error, error_size) != 0)
            result = -1;
        else {
            if (payload_compare(&source, &after, error, error_size) != 0 ||
                ufs_verify_layout(stage_path, growth, growth_percent,
                                  error, error_size) != 0)
                result = -1;
            inventory_free(&after);
        }
    }
    if (result == 0 && commit_bytes != NULL) *commit_bytes = filesystem_bytes;
    inventory_free(&source);
    return result;
}
