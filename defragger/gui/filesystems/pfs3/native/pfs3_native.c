// SPDX-License-Identifier: GPL-3.0-or-later
#include "pfs3_native.h"

#include "ld_device.h"
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

#define PFS1_DISK_ID UINT32_C(0x50465301)
#define PFS2_DISK_ID UINT32_C(0x50465302)
#define PFS_SECTOR_SIZE 512U
#define PFS_ROOT_SECTOR 2U
#define PFS_RESBLOCK_SIZE 1024U
#define PFS_RESCLUSTER 2U
#define PFS_BM_ID UINT16_C(0x424d)
#define PFS_MI_ID UINT16_C(0x4d49)
#define PFS_IB_ID UINT16_C(0x4942)
#define PFS_AB_ID UINT16_C(0x4142)
#define PFS_DB_ID UINT16_C(0x4442)
#define PFS_EX_ID UINT16_C(0x4558)
#define PFS_MODE_HARDDISK UINT32_C(1)
#define PFS_MODE_SPLIT_ANODES UINT32_C(2)
#define PFS_MODE_DIR_EXTENSION UINT32_C(4)
#define PFS_MODE_DELDIR UINT32_C(8)
#define PFS_MODE_SIZEFIELD UINT32_C(16)
#define PFS_MODE_EXTENSION UINT32_C(32)
#define PFS_MODE_DATESTAMP UINT32_C(64)
#define PFS_MODE_SUPERINDEX UINT32_C(128)
#define PFS_MODE_SUPERDELDIR UINT32_C(256)
#define PFS_MODE_EXTROVING UINT32_C(512)
#define PFS_MODE_LONGFN UINT32_C(1024)
#define PFS_MODE_LARGEFILE UINT32_C(2048)
#define PFS_MODE_STORED_GEOM UINT32_C(4096)
#define PFS_REQUIRED_OPTIONS (PFS_MODE_HARDDISK | PFS_MODE_SPLIT_ANODES | PFS_MODE_SIZEFIELD)
#define PFS_ALLOWED_OPTIONS (PFS_MODE_HARDDISK | PFS_MODE_SPLIT_ANODES | PFS_MODE_DIR_EXTENSION | PFS_MODE_DELDIR | PFS_MODE_SIZEFIELD | PFS_MODE_EXTENSION | PFS_MODE_DATESTAMP | PFS_MODE_EXTROVING | PFS_MODE_LONGFN | PFS_MODE_STORED_GEOM)
#define PFS_ROOT_ANODE 5U
#define PFS_USER_FIRST_ANODE 6U
#define PFS_ST_FILE (-3)
#define PFS_INDEX_HEADER 12U
#define PFS_ANODE_HEADER 16U
#define PFS_DIR_HEADER 20U
#define PFS_ANODE_BYTES 12U
#define PFS_INDEX_PER_BLOCK ((PFS_RESBLOCK_SIZE - PFS_INDEX_HEADER) / 4U)
#define PFS_ANODES_PER_BLOCK ((PFS_RESBLOCK_SIZE - PFS_ANODE_HEADER) / PFS_ANODE_BYTES)
#define PFS_BITMAP_LONGS ((PFS_RESBLOCK_SIZE / 4U) - 3U)
#define PFS_BITMAP_BITS (PFS_BITMAP_LONGS * 32U)
#define PFS_ROOT_BITMAP_BITS ((PFS_SECTOR_SIZE - 12U) * 8U)
#define PFS_DEFAULT_FILENAME_SIZE 32U
#define PFS_MIN_FILENAME_SIZE 30U
#define PFS_MAX_FILENAME_SIZE 107U
#define PFS_MAX_DELDIR_BLOCKS 32U
#define PFS_DELDIR_ENTRIES_PER_BLOCK 31U
#define PFS_COPY_BATCH (1024U * 1024U)

typedef struct {
    uint32_t disk_type;
    uint32_t options;
    uint32_t datestamp;
    uint32_t last_reserved;
    uint32_t first_reserved;
    uint32_t reserved_free;
    uint16_t reserved_block_size;
    uint16_t root_cluster;
    uint32_t blocks_free;
    uint32_t always_free;
    uint32_t roving_ptr;
    uint32_t disksize;
    uint32_t extension;
    uint16_t filename_size;
    uint32_t bitmap_index[5];
    uint32_t index_block[99];
} PfsRoot;

typedef struct {
    uint32_t clusters;
    uint32_t block;
    uint32_t next;
    uint32_t number;
    uint64_t record_offset;
} PfsAnode;

typedef struct {
    uint32_t first_anode;
    uint64_t size_bytes;
    PfsAnode *extents;
    size_t extent_count;
    size_t extent_capacity;
    uint64_t blocks;
    bool fragmented;
    uint32_t target;
} PfsFile;

typedef struct {
    int fd;
    PfsRoot root;
    uint64_t physical_bytes;
    uint64_t filesystem_bytes;
    uint8_t *free_map;
    uint8_t *fragmented_map;
    PfsFile *files;
    size_t file_count;
    size_t file_capacity;
    uint32_t bitmap_blocks;
    bool transaction_pending;
} PfsModel;

static void set_error(char *error, size_t size, const char *message)
{
    if (error != NULL && size != 0U)
        (void)snprintf(error, size, "%s", message);
}

static void set_errno_error(char *error, size_t size, const char *prefix)
{
    if (error != NULL && size != 0U)
        (void)snprintf(error, size, "%s: %s", prefix, strerror(errno));
}

static int reserve_array(void **items, size_t *capacity, size_t need,
                         size_t item_size, char *error, size_t error_size)
{
    if (infiltratr_array_reserve(items, capacity, item_size, need, 16U))
        return 0;
    set_error(error, error_size, "cannot grow PFS3 catalogue");
    return -1;
}

static int fd_size(int fd, uint64_t *bytes)
{
    struct stat st;
    if (fstat(fd, &st) != 0)
        return -1;
    if (S_ISREG(st.st_mode)) {
        if (st.st_size < 0) {
            errno = EINVAL;
            return -1;
        }
        *bytes = (uint64_t)st.st_size;
        return 0;
    }
    if (!S_ISBLK(st.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(fd, BLKGETSIZE64, bytes) == 0 ? 0 : -1;
}

static int read_exact(int fd, void *buffer, size_t bytes, uint64_t offset,
                      char *error, size_t error_size, const char *what)
{
    if (ld_pread_full(fd, buffer, bytes, offset) == (ssize_t)bytes)
        return 0;
    set_errno_error(error, error_size, what);
    return -1;
}

static int write_exact(int fd, const void *buffer, size_t bytes, uint64_t offset,
                       char *error, size_t error_size, const char *what)
{
    if (ld_pwrite_full(fd, buffer, bytes, offset) == (ssize_t)bytes)
        return 0;
    set_errno_error(error, error_size, what);
    return -1;
}

static bool reserved_pointer_valid(const PfsRoot *root, uint32_t block)
{
    return block >= root->first_reserved && block <= root->last_reserved &&
           block + PFS_RESCLUSTER - 1U <= root->last_reserved &&
           (block - root->first_reserved) % PFS_RESCLUSTER == 0U;
}

static int parse_root(int fd, uint64_t physical_bytes, PfsRoot *root,
                      char *error, size_t error_size)
{
    uint8_t block[PFS_SECTOR_SIZE];
    if (read_exact(fd, block, sizeof(block),
                   (uint64_t)PFS_ROOT_SECTOR * PFS_SECTOR_SIZE,
                   error, error_size, "cannot read PFS3 root block") != 0)
        return -1;

    memset(root, 0, sizeof(*root));
    root->disk_type = infiltratr_load_be32(block);
    root->options = infiltratr_load_be32(block + 4U);
    root->datestamp = infiltratr_load_be32(block + 8U);
    root->last_reserved = infiltratr_load_be32(block + 52U);
    root->first_reserved = infiltratr_load_be32(block + 56U);
    root->reserved_free = infiltratr_load_be32(block + 60U);
    root->reserved_block_size = infiltratr_load_be16(block + 64U);
    root->root_cluster = infiltratr_load_be16(block + 66U);
    root->blocks_free = infiltratr_load_be32(block + 68U);
    root->always_free = infiltratr_load_be32(block + 72U);
    root->roving_ptr = infiltratr_load_be32(block + 76U);
    root->disksize = infiltratr_load_be32(block + 84U);
    root->extension = infiltratr_load_be32(block + 88U);
    root->filename_size = PFS_DEFAULT_FILENAME_SIZE;
    for (size_t index = 0U; index < 5U; ++index)
        root->bitmap_index[index] = infiltratr_load_be32(block + 96U + index * 4U);
    for (size_t index = 0U; index < 99U; ++index)
        root->index_block[index] = infiltratr_load_be32(block + 116U + index * 4U);

    if (root->disk_type != PFS1_DISK_ID) {
        if (root->disk_type == PFS2_DISK_ID)
            set_error(error, error_size,
                      "PFS\2 media is identified but outside the validated 512-byte small-disk writer subset");
        else
            set_error(error, error_size, "PFS3 root signature is not present");
        return -1;
    }
    if ((root->options & PFS_REQUIRED_OPTIONS) != PFS_REQUIRED_OPTIONS ||
        (root->options & ~PFS_ALLOWED_OPTIONS) != 0U ||
        (root->options & (PFS_MODE_SUPERINDEX | PFS_MODE_LARGEFILE)) != 0U) {
        set_error(error, error_size,
                  "PFS3 root options are outside the validated small-disk subset");
        return -1;
    }
    if (root->reserved_block_size != PFS_RESBLOCK_SIZE ||
        root->root_cluster != PFS_RESCLUSTER ||
        root->first_reserved != PFS_ROOT_SECTOR ||
        root->last_reserved < root->first_reserved ||
        ((root->last_reserved - root->first_reserved + 1U) % PFS_RESCLUSTER) != 0U ||
        root->disksize <= root->last_reserved + 1U) {
        set_error(error, error_size, "PFS3 reserved-area geometry is invalid or unsupported");
        return -1;
    }
    const uint64_t filesystem_bytes =
        (uint64_t)root->disksize * PFS_SECTOR_SIZE;
    if (filesystem_bytes > physical_bytes || physical_bytes < 4U * PFS_SECTOR_SIZE) {
        set_error(error, error_size, "PFS3 filesystem exceeds the selected target");
        return -1;
    }
    const uint32_t reserved_slots =
        (root->last_reserved - root->first_reserved + 1U) / PFS_RESCLUSTER;
    if (reserved_slots == 0U || reserved_slots > PFS_ROOT_BITMAP_BITS) {
        set_error(error, error_size,
                  "PFS3 reserved bitmap requires an unsupported multi-cluster root");
        return -1;
    }
    return 0;
}

static int read_reserved_block(int fd, const PfsRoot *root, uint32_t block,
                               uint8_t buffer[PFS_RESBLOCK_SIZE],
                               char *error, size_t error_size,
                               const char *what)
{
    if (!reserved_pointer_valid(root, block)) {
        set_error(error, error_size, "PFS3 metadata points outside the reserved area");
        return -1;
    }
    return read_exact(fd, buffer, PFS_RESBLOCK_SIZE,
                      (uint64_t)block * PFS_SECTOR_SIZE,
                      error, error_size, what);
}

static bool root_bitmap_free(const uint8_t sector[PFS_SECTOR_SIZE],
                             uint32_t slot)
{
    const uint32_t word = slot / 32U;
    const uint32_t bit = slot % 32U;
    if (12U + (word + 1U) * 4U > PFS_SECTOR_SIZE)
        return false;
    return (infiltratr_load_be32(sector + 12U + word * 4U) &
            (UINT32_C(0x80000000) >> bit)) != 0U;
}

static int validate_reserved_area(int fd, PfsRoot *root,
                                  bool *pending,
                                  char *error, size_t error_size)
{
    uint8_t bitmap_sector[PFS_SECTOR_SIZE];
    if (read_exact(fd, bitmap_sector, sizeof(bitmap_sector),
                   (uint64_t)(PFS_ROOT_SECTOR + 1U) * PFS_SECTOR_SIZE,
                   error, error_size,
                   "cannot read PFS3 reserved bitmap") != 0)
        return -1;
    if (infiltratr_load_be16(bitmap_sector) != PFS_BM_ID ||
        infiltratr_load_be32(bitmap_sector + 8U) != 0U) {
        set_error(error, error_size, "PFS3 reserved bitmap header is invalid");
        return -1;
    }
    if (root_bitmap_free(bitmap_sector, 0U)) {
        set_error(error, error_size, "PFS3 root cluster is marked free");
        return -1;
    }

    uint32_t pointers[7] = {
        root->extension, root->bitmap_index[0], root->index_block[0],
        0U, 0U, 0U, 0U
    };
    for (size_t index = 0U; index < 3U; ++index) {
        if (pointers[index] == 0U)
            continue;
        if (!reserved_pointer_valid(root, pointers[index])) {
            set_error(error, error_size, "PFS3 root metadata pointer is invalid");
            return -1;
        }
        const uint32_t slot =
            (pointers[index] - root->first_reserved) / PFS_RESCLUSTER;
        if (root_bitmap_free(bitmap_sector, slot)) {
            set_error(error, error_size,
                      "PFS3 referenced metadata is marked free in the reserved bitmap");
            return -1;
        }
    }

    *pending = false;
    if ((root->options & PFS_MODE_EXTENSION) != 0U) {
        if (root->extension == 0U) {
            set_error(error, error_size, "PFS3 extension option has no extension block");
            return -1;
        }
        uint8_t extension[PFS_RESBLOCK_SIZE];
        if (read_reserved_block(fd, root, root->extension, extension,
                                error, error_size,
                                "cannot read PFS3 root extension") != 0)
            return -1;
        if (infiltratr_load_be16(extension) != PFS_EX_ID) {
            set_error(error, error_size, "PFS3 root extension ID is invalid");
            return -1;
        }

        const uint32_t reserved_slots =
            (root->last_reserved - root->first_reserved + 1U) /
            PFS_RESCLUSTER;
        const uint32_t reserved_roving =
            infiltratr_load_be32(extension + 44U);
        const uint16_t roving_bit =
            infiltratr_load_be16(extension + 48U);
        const uint16_t delete_directory_roving =
            infiltratr_load_be16(extension + 52U);
        const uint16_t delete_directory_size =
            infiltratr_load_be16(extension + 54U);
        const uint16_t stored_filename_size =
            infiltratr_load_be16(extension + 56U);
        const uint32_t delete_directory_entries =
            (uint32_t)delete_directory_size *
            PFS_DELDIR_ENTRIES_PER_BLOCK;

        if (roving_bit > 31U ||
            (reserved_slots != 0U && reserved_roving >= reserved_slots) ||
            delete_directory_size > PFS_MAX_DELDIR_BLOCKS ||
            (delete_directory_entries == 0U
                 ? delete_directory_roving != 0U
                 : delete_directory_roving >= delete_directory_entries)) {
            set_error(error, error_size,
                      "PFS3 root extension contains invalid roving or delete-directory geometry");
            return -1;
        }
        if (stored_filename_size == 0U) {
            root->filename_size = PFS_DEFAULT_FILENAME_SIZE;
        } else if (stored_filename_size < PFS_MIN_FILENAME_SIZE ||
                   stored_filename_size > PFS_MAX_FILENAME_SIZE) {
            set_error(error, error_size,
                      "PFS3 root extension contains an invalid filename-size limit");
            return -1;
        } else {
            root->filename_size = stored_filename_size;
        }

        *pending = infiltratr_load_be32(extension + 28U) != 0U;
    } else if (root->extension != 0U) {
        set_error(error, error_size,
                  "PFS3 root references an extension without enabling it");
        return -1;
    }
    return 0;
}

static int load_free_map(int fd, const PfsRoot *root,
                         uint8_t **free_map_out, uint32_t *bitmap_blocks_out,
                         char *error, size_t error_size)
{
    const uint32_t first_data = root->last_reserved + 1U;
    const uint32_t normal_blocks = root->disksize - first_data;
    const uint32_t bitmap_blocks =
        (normal_blocks + PFS_BITMAP_BITS - 1U) / PFS_BITMAP_BITS;
    const uint32_t bitmap_indexes =
        (bitmap_blocks + PFS_INDEX_PER_BLOCK - 1U) / PFS_INDEX_PER_BLOCK;
    if (bitmap_indexes == 0U || bitmap_indexes > 5U) {
        set_error(error, error_size,
                  "PFS3 bitmap fanout is outside the validated small-disk subset");
        return -1;
    }

    uint8_t *free_map = calloc(root->disksize, 1U);
    if (free_map == NULL) {
        set_error(error, error_size, "out of memory building PFS3 allocation map");
        return -1;
    }

    uint32_t counted_free = 0U;
    uint32_t bitmap_sequence = 0U;
    for (uint32_t group = 0U; group < bitmap_indexes; ++group) {
        const uint32_t index_pointer = root->bitmap_index[group];
        uint8_t index_block[PFS_RESBLOCK_SIZE];
        if (index_pointer == 0U ||
            read_reserved_block(fd, root, index_pointer, index_block,
                                error, error_size,
                                "cannot read PFS3 bitmap index") != 0) {
            free(free_map);
            return -1;
        }
        if (infiltratr_load_be16(index_block) != PFS_MI_ID ||
            infiltratr_load_be32(index_block + 8U) != group) {
            free(free_map);
            set_error(error, error_size, "PFS3 bitmap index header is invalid");
            return -1;
        }
        for (uint32_t slot = 0U;
             slot < PFS_INDEX_PER_BLOCK && bitmap_sequence < bitmap_blocks;
             ++slot, ++bitmap_sequence) {
            const uint32_t bitmap_pointer =
                infiltratr_load_be32(index_block + PFS_INDEX_HEADER + slot * 4U);
            uint8_t bitmap[PFS_RESBLOCK_SIZE];
            if (bitmap_pointer == 0U ||
                read_reserved_block(fd, root, bitmap_pointer, bitmap,
                                    error, error_size,
                                    "cannot read PFS3 allocation bitmap") != 0) {
                free(free_map);
                return -1;
            }
            if (infiltratr_load_be16(bitmap) != PFS_BM_ID ||
                infiltratr_load_be32(bitmap + 8U) != bitmap_sequence) {
                free(free_map);
                set_error(error, error_size, "PFS3 allocation bitmap header is invalid");
                return -1;
            }
            const uint32_t base =
                first_data + bitmap_sequence * PFS_BITMAP_BITS;
            for (uint32_t bit = 0U; bit < PFS_BITMAP_BITS; ++bit) {
                const uint32_t block = base + bit;
                if (block >= root->disksize)
                    break;
                const uint32_t word = bit / 32U;
                const uint32_t within = bit % 32U;
                const bool is_free =
                    (infiltratr_load_be32(bitmap + 12U + word * 4U) &
                     (UINT32_C(0x80000000) >> within)) != 0U;
                free_map[block] = is_free ? 1U : 0U;
                if (is_free)
                    counted_free++;
            }
        }
    }

    if (counted_free != root->blocks_free) {
        free(free_map);
        set_error(error, error_size,
                  "PFS3 root free-block count disagrees with its allocation bitmap");
        return -1;
    }
    *free_map_out = free_map;
    *bitmap_blocks_out = bitmap_blocks;
    return 0;
}

static int read_anode(int fd, const PfsRoot *root, uint32_t number,
                      PfsAnode *anode, char *error, size_t error_size)
{
    if (number == 0U) {
        set_error(error, error_size, "PFS3 anode number zero is not a file anode");
        return -1;
    }
    const uint32_t sequence = number >> 16U;
    const uint32_t offset = number & UINT32_C(0xffff);
    if (offset >= PFS_ANODES_PER_BLOCK) {
        set_error(error, error_size, "PFS3 anode offset exceeds its anode block");
        return -1;
    }
    const uint32_t index_sequence = sequence / PFS_INDEX_PER_BLOCK;
    const uint32_t index_offset = sequence % PFS_INDEX_PER_BLOCK;
    if (index_sequence >= 99U || root->index_block[index_sequence] == 0U) {
        set_error(error, error_size, "PFS3 anode index is absent");
        return -1;
    }

    uint8_t index_block[PFS_RESBLOCK_SIZE];
    if (read_reserved_block(fd, root, root->index_block[index_sequence],
                            index_block, error, error_size,
                            "cannot read PFS3 anode index") != 0)
        return -1;
    if (infiltratr_load_be16(index_block) != PFS_IB_ID ||
        infiltratr_load_be32(index_block + 8U) != index_sequence) {
        set_error(error, error_size, "PFS3 anode index header is invalid");
        return -1;
    }
    const uint32_t anode_block_pointer =
        infiltratr_load_be32(index_block + PFS_INDEX_HEADER + index_offset * 4U);
    uint8_t anode_block[PFS_RESBLOCK_SIZE];
    if (anode_block_pointer == 0U ||
        read_reserved_block(fd, root, anode_block_pointer, anode_block,
                            error, error_size,
                            "cannot read PFS3 anode block") != 0)
        return -1;
    if (infiltratr_load_be16(anode_block) != PFS_AB_ID ||
        infiltratr_load_be32(anode_block + 8U) != sequence) {
        set_error(error, error_size, "PFS3 anode block header is invalid");
        return -1;
    }
    const size_t record = PFS_ANODE_HEADER + (size_t)offset * PFS_ANODE_BYTES;
    anode->clusters = infiltratr_load_be32(anode_block + record);
    anode->block = infiltratr_load_be32(anode_block + record + 4U);
    anode->next = infiltratr_load_be32(anode_block + record + 8U);
    anode->number = number;
    anode->record_offset =
        (uint64_t)anode_block_pointer * PFS_SECTOR_SIZE + record;
    return 0;
}

static int file_push_extent(PfsFile *file, PfsAnode anode,
                            char *error, size_t error_size)
{
    if (reserve_array((void **)&file->extents, &file->extent_capacity,
                      file->extent_count + 1U, sizeof(*file->extents),
                      error, error_size) != 0)
        return -1;
    file->extents[file->extent_count++] = anode;
    return 0;
}

static int model_push_file(PfsModel *model, uint32_t anode, uint64_t size,
                           char *error, size_t error_size)
{
    if (reserve_array((void **)&model->files, &model->file_capacity,
                      model->file_count + 1U, sizeof(*model->files),
                      error, error_size) != 0)
        return -1;
    PfsFile *file = &model->files[model->file_count++];
    memset(file, 0, sizeof(*file));
    file->first_anode = anode;
    file->size_bytes = size;
    return 0;
}

static int parse_root_directory(PfsModel *model, char *error, size_t error_size)
{
    PfsAnode root_dir;
    if (read_anode(model->fd, &model->root, PFS_ROOT_ANODE,
                   &root_dir, error, error_size) != 0)
        return -1;
    if (root_dir.clusters != 1U || root_dir.next != 0U ||
        !reserved_pointer_valid(&model->root, root_dir.block)) {
        set_error(error, error_size,
                  "PFS3 root directory is outside the validated single-block subset");
        return -1;
    }

    uint8_t directory[PFS_RESBLOCK_SIZE];
    if (read_reserved_block(model->fd, &model->root, root_dir.block,
                            directory, error, error_size,
                            "cannot read PFS3 root directory") != 0)
        return -1;
    if (infiltratr_load_be16(directory) != PFS_DB_ID ||
        infiltratr_load_be32(directory + 12U) != PFS_ROOT_ANODE ||
        infiltratr_load_be32(directory + 16U) != 0U) {
        set_error(error, error_size, "PFS3 root directory header is invalid");
        return -1;
    }

    size_t offset = PFS_DIR_HEADER;
    while (offset < PFS_RESBLOCK_SIZE) {
        const uint8_t next = directory[offset];
        if (next == 0U)
            break;
        if ((next & 1U) != 0U || next < 20U ||
            offset + next > PFS_RESBLOCK_SIZE) {
            set_error(error, error_size, "PFS3 directory entry length is invalid");
            return -1;
        }
        const int type = (int)(int8_t)directory[offset + 1U];
        const uint32_t anode = infiltratr_load_be32(directory + offset + 2U);
        const uint32_t size = infiltratr_load_be32(directory + offset + 6U);
        const uint8_t name_length = directory[offset + 17U];
        if (name_length > model->root.filename_size) {
            set_error(error, error_size,
                      "PFS3 directory entry name exceeds the volume filename limit");
            return -1;
        }
        const size_t comment_offset = 18U + (size_t)name_length;
        if (comment_offset >= next) {
            set_error(error, error_size,
                      "PFS3 directory entry name exceeds its record");
            return -1;
        }

        size_t payload_end = next;
        if ((model->root.options & PFS_MODE_DIR_EXTENSION) != 0U) {
            const uint16_t flags =
                infiltratr_load_be16(directory + offset + next - 2U);
            if ((flags >> 11U) != 0U) {
                set_error(error, error_size,
                          "PFS3 directory entry contains unknown extension fields");
                return -1;
            }
            uint16_t bits = flags;
            size_t extra_words = 0U;
            while (bits != 0U) {
                extra_words += (size_t)(bits & 1U);
                bits >>= 1U;
            }
            const size_t extension_bytes = 2U + extra_words * 2U;
            if (extension_bytes > next) {
                set_error(error, error_size,
                          "PFS3 directory entry extension fields overrun the record");
                return -1;
            }
            payload_end = next - extension_bytes;
        }

        if (payload_end <= comment_offset ||
            (size_t)directory[offset + comment_offset] + 1U >
                payload_end - comment_offset) {
            set_error(error, error_size,
                      "PFS3 directory entry comment exceeds its record");
            return -1;
        }
        if (type != PFS_ST_FILE) {
            set_error(error, error_size,
                      "PFS3 nested directories, links or special entries are outside the validated writer subset");
            return -1;
        }
        if (anode < PFS_USER_FIRST_ANODE ||
            model_push_file(model, anode, size, error, error_size) != 0)
            return -1;
        offset += next;
    }
    return 0;
}

static int load_file_extents(PfsModel *model, PfsFile *file,
                             uint8_t *claimed,
                             char *error, size_t error_size)
{
    const uint64_t needed =
        (file->size_bytes + PFS_SECTOR_SIZE - 1U) / PFS_SECTOR_SIZE;
    if (needed == 0U) {
        PfsAnode empty;
        if (read_anode(model->fd, &model->root, file->first_anode,
                       &empty, error, error_size) != 0)
            return -1;
        if (empty.clusters != 0U || empty.next != 0U ||
            !(empty.block == 0U || empty.block == UINT32_MAX)) {
            set_error(error, error_size, "PFS3 empty file has allocated data");
            return -1;
        }
        return 0;
    }

    uint64_t total = 0U;
    uint32_t current = file->first_anode;
    uint32_t previous_end = 0U;
    bool have_previous = false;
    size_t guard = 0U;
    while (current != 0U) {
        if (++guard > 65536U) {
            set_error(error, error_size, "PFS3 anode chain is cyclic or unbounded");
            return -1;
        }
        PfsAnode anode;
        if (read_anode(model->fd, &model->root, current,
                       &anode, error, error_size) != 0)
            return -1;
        if (anode.clusters == 0U ||
            anode.block < model->root.last_reserved + 1U ||
            anode.block >= model->root.disksize ||
            anode.clusters > model->root.disksize - anode.block) {
            set_error(error, error_size, "PFS3 file anode has invalid data geometry");
            return -1;
        }
        if (file_push_extent(file, anode, error, error_size) != 0)
            return -1;
        if (have_previous && previous_end != anode.block)
            file->fragmented = true;
        previous_end = anode.block + anode.clusters;
        have_previous = true;

        for (uint32_t block = anode.block;
             block < anode.block + anode.clusters; ++block) {
            if (model->free_map[block] != 0U) {
                set_error(error, error_size,
                          "PFS3 file references a block marked free");
                return -1;
            }
            if (claimed[block] != 0U) {
                set_error(error, error_size,
                          "PFS3 file allocation overlaps another file");
                return -1;
            }
            claimed[block] = 1U;
        }
        total += anode.clusters;
        if (total > needed) {
            set_error(error, error_size,
                      "PFS3 anode chain allocates more blocks than the file size");
            return -1;
        }
        current = anode.next;
    }
    if (total != needed) {
        set_error(error, error_size,
                  "PFS3 anode chain does not cover the complete file size");
        return -1;
    }
    file->blocks = total;
    return 0;
}

static bool file_growth_satisfied(const PfsModel *model, const PfsFile *file,
                                  unsigned percent)
{
    if (file->blocks == 0U)
        return true;
    if (file->fragmented || file->extent_count == 0U)
        return false;
    uint64_t end = file->extents[0].block;
    for (size_t index = 0U; index < file->extent_count; ++index) {
        if ((uint64_t)file->extents[index].block != end)
            return false;
        end += file->extents[index].clusters;
    }
    const uint64_t reserve =
        (file->blocks * percent + 99U) / 100U;
    if (end + reserve > model->root.disksize)
        return false;
    for (uint64_t block = end; block < end + reserve; ++block)
        if (model->free_map[block] == 0U)
            return false;
    return true;
}

static void model_free(PfsModel *model)
{
    if (model == NULL)
        return;
    if (model->fd >= 0)
        (void)close(model->fd);
    for (size_t index = 0U; index < model->file_count; ++index)
        free(model->files[index].extents);
    free(model->files);
    free(model->free_map);
    free(model->fragmented_map);
    memset(model, 0, sizeof(*model));
    model->fd = -1;
}

static int model_load(const char *path, PfsModel *model,
                      char *error, size_t error_size)
{
    memset(model, 0, sizeof(*model));
    model->fd = -1;
    model->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (model->fd < 0) {
        set_errno_error(error, error_size, "cannot open PFS3 target");
        return -1;
    }
    if (fd_size(model->fd, &model->physical_bytes) != 0 ||
        parse_root(model->fd, model->physical_bytes, &model->root,
                   error, error_size) != 0)
        goto failure;
    model->filesystem_bytes =
        (uint64_t)model->root.disksize * PFS_SECTOR_SIZE;

    if (validate_reserved_area(model->fd, &model->root,
                               &model->transaction_pending,
                               error, error_size) != 0 ||
        load_free_map(model->fd, &model->root,
                      &model->free_map, &model->bitmap_blocks,
                      error, error_size) != 0 ||
        parse_root_directory(model, error, error_size) != 0)
        goto failure;

    uint8_t *claimed = calloc(model->root.disksize, 1U);
    model->fragmented_map = calloc(model->root.disksize, 1U);
    if (claimed == NULL || model->fragmented_map == NULL) {
        free(claimed);
        set_error(error, error_size, "out of memory validating PFS3 allocations");
        goto failure;
    }

    uint64_t data_blocks = 0U;
    for (size_t index = 0U; index < model->file_count; ++index) {
        PfsFile *file = &model->files[index];
        if (load_file_extents(model, file, claimed, error, error_size) != 0) {
            free(claimed);
            goto failure;
        }
        data_blocks += file->blocks;
        if (file->fragmented) {
            for (size_t extent = 0U; extent < file->extent_count; ++extent) {
                const PfsAnode *anode = &file->extents[extent];
                memset(model->fragmented_map + anode->block, 1,
                       anode->clusters);
            }
        }
    }

    const uint32_t first_data = model->root.last_reserved + 1U;
    for (uint32_t block = first_data; block < model->root.disksize; ++block) {
        const bool used = model->free_map[block] == 0U;
        if (used != (claimed[block] != 0U)) {
            free(claimed);
            set_error(error, error_size,
                      "PFS3 contains allocated normal blocks not owned by the validated root-file subset");
            goto failure;
        }
    }
    free(claimed);

    (void)data_blocks;
    return 0;

failure:
    model_free(model);
    return -1;
}

static void fill_analysis(const PfsModel *model, Pfs3Analysis *analysis)
{
    memset(analysis, 0, sizeof(*analysis));
    const uint32_t first_data = model->root.last_reserved + 1U;
    uint64_t free_blocks = 0U;
    uint64_t fragmented_files = 0U;
    uint64_t data_blocks = 0U;
    bool growth_ok = true;
    for (uint32_t block = first_data; block < model->root.disksize; ++block)
        if (model->free_map[block] != 0U)
            free_blocks++;
    for (size_t index = 0U; index < model->file_count; ++index) {
        const PfsFile *file = &model->files[index];
        data_blocks += file->blocks;
        if (file->fragmented)
            fragmented_files++;
        if (!file_growth_satisfied(model, file, 10U))
            growth_ok = false;
    }
    analysis->block_size = PFS_SECTOR_SIZE;
    analysis->total_blocks = model->root.disksize;
    analysis->bitmap_base = first_data;
    analysis->bitmap_blocks = model->bitmap_blocks;
    analysis->root_object_container = model->root.extension;
    analysis->admin_space_container = model->root.index_block[0];
    analysis->extent_bnode_root = model->root.bitmap_index[0];
    analysis->object_node_root = PFS_ROOT_ANODE;
    analysis->structure_version = 3U;
    analysis->sequence_number = (uint16_t)(model->root.datestamp & UINT16_MAX);
    analysis->root_bits = 0U;
    analysis->filesystem_bytes = model->filesystem_bytes;
    analysis->physical_bytes = model->physical_bytes;
    analysis->free_blocks = free_blocks;
    analysis->used_blocks = model->root.disksize - free_blocks;
    analysis->data_blocks = data_blocks;
    analysis->regular_files = model->file_count;
    analysis->directories = 1U;
    analysis->fragmented_files = fragmented_files;
    analysis->growth_10_satisfied = growth_ok;
    analysis->primary_root_valid = true;
    analysis->backup_root_valid = false;
    analysis->transaction_pending = model->transaction_pending;
    analysis->options = model->root.options;
    analysis->disk_type = model->root.disk_type;
    analysis->reserved_block_size = model->root.reserved_block_size;
}

static void fill_cells(const PfsModel *model, Pfs3MapCell *cells,
                       uint64_t cell_count)
{
    const uint64_t physical_blocks =
        (model->physical_bytes + PFS_SECTOR_SIZE - 1U) / PFS_SECTOR_SIZE;
    const uint64_t total = physical_blocks > model->root.disksize
                         ? physical_blocks : model->root.disksize;
    for (uint64_t index = 0U; index < cell_count; ++index) {
        Pfs3MapCell *cell = &cells[index];
        memset(cell, 0, sizeof(*cell));
        cell->start = (total * index) / cell_count;
        cell->end = (total * (index + 1U)) / cell_count;
        for (uint64_t block = cell->start; block < cell->end; ++block) {
            if (block >= model->root.disksize) {
                cell->outside_count++;
            } else if (model->free_map[block] != 0U) {
                cell->free_count++;
            } else {
                cell->used_count++;
                if (model->fragmented_map[block] != 0U)
                    cell->fragmented_count++;
            }
        }
    }
}

int pfs3_analyse(const char *path, Pfs3Analysis *analysis,
                 Pfs3MapCell *cells, uint64_t cell_count,
                 char *error, size_t error_size)
{
    if (path == NULL || analysis == NULL ||
        (cell_count != 0U && cells == NULL)) {
        set_error(error, error_size, "invalid PFS3 analysis arguments");
        return -1;
    }
    PfsModel model;
    if (model_load(path, &model, error, error_size) != 0)
        return -1;
    fill_analysis(&model, analysis);
    if (cell_count != 0U)
        fill_cells(&model, cells, cell_count);
    model_free(&model);
    return 0;
}

bool pfs3_probe(const char *path)
{
    if (path == NULL)
        return false;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    uint8_t root[PFS_SECTOR_SIZE];
    const bool ok =
        ld_pread_full(fd, root, sizeof(root),
                      (uint64_t)PFS_ROOT_SECTOR * PFS_SECTOR_SIZE) ==
            (ssize_t)sizeof(root) &&
        (infiltratr_load_be32(root) == PFS1_DISK_ID ||
         infiltratr_load_be32(root) == PFS2_DISK_ID);
    (void)close(fd);
    return ok;
}

static int copy_bytes(int source_fd, int target_fd, uint64_t bytes,
                      uint64_t *written, char *error, size_t error_size)
{
    uint8_t *buffer = malloc(PFS_COPY_BATCH);
    if (buffer == NULL) {
        set_error(error, error_size, "out of memory copying PFS3 image");
        return -1;
    }
    uint64_t offset = 0U;
    while (offset < bytes) {
        const size_t count =
            bytes - offset > PFS_COPY_BATCH ? PFS_COPY_BATCH
                                            : (size_t)(bytes - offset);
        if (ld_pread_full(source_fd, buffer, count, offset) != (ssize_t)count ||
            ld_pwrite_full(target_fd, buffer, count, offset) != (ssize_t)count) {
            free(buffer);
            set_errno_error(error, error_size, "PFS3 image copy failed");
            return -1;
        }
        offset += count;
    }
    free(buffer);
    if (written != NULL)
        *written = offset;
    return 0;
}

static int copy_file_payload(const PfsModel *model, int stage_fd,
                             const PfsFile *file, uint32_t target,
                             char *error, size_t error_size)
{
    uint8_t *buffer = malloc(PFS_COPY_BATCH);
    if (buffer == NULL) {
        set_error(error, error_size, "out of memory relocating PFS3 file");
        return -1;
    }
    uint64_t logical_blocks = 0U;
    for (size_t index = 0U; index < file->extent_count; ++index) {
        const PfsAnode *extent = &file->extents[index];
        uint64_t remaining = extent->clusters;
        uint64_t source_block = extent->block;
        while (remaining != 0U) {
            const uint64_t batch_blocks =
                remaining > PFS_COPY_BATCH / PFS_SECTOR_SIZE
                    ? PFS_COPY_BATCH / PFS_SECTOR_SIZE : remaining;
            const size_t bytes = (size_t)batch_blocks * PFS_SECTOR_SIZE;
            if (ld_pread_full(model->fd, buffer, bytes,
                              source_block * PFS_SECTOR_SIZE) != (ssize_t)bytes ||
                ld_pwrite_full(stage_fd, buffer, bytes,
                               ((uint64_t)target + logical_blocks) *
                                   PFS_SECTOR_SIZE) != (ssize_t)bytes) {
                free(buffer);
                set_errno_error(error, error_size,
                                "cannot relocate PFS3 file payload");
                return -1;
            }
            source_block += batch_blocks;
            logical_blocks += batch_blocks;
            remaining -= batch_blocks;
        }
    }
    free(buffer);
    return logical_blocks == file->blocks ? 0 : -1;
}

static int write_anode_record(int fd, const PfsAnode *where,
                              uint32_t clusters, uint32_t block, uint32_t next,
                              char *error, size_t error_size)
{
    uint8_t record[PFS_ANODE_BYTES];
    infiltratr_store_be32(record, clusters);
    infiltratr_store_be32(record + 4U, block);
    infiltratr_store_be32(record + 8U, next);
    return write_exact(fd, record, sizeof(record), where->record_offset,
                       error, error_size, "cannot update PFS3 anode");
}

static int rewrite_file_anodes(int fd, const PfsFile *file,
                               uint32_t target, char *error, size_t error_size)
{
    if (file->extent_count == 0U)
        return 0;
    if (file->blocks > UINT32_MAX) {
        set_error(error, error_size,
                  "PFS3 file is too large for the validated 32-bit anode subset");
        return -1;
    }
    if (write_anode_record(fd, &file->extents[0],
                           (uint32_t)file->blocks, target, 0U,
                           error, error_size) != 0)
        return -1;
    for (size_t index = 1U; index < file->extent_count; ++index)
        if (write_anode_record(fd, &file->extents[index],
                               0U, 0U, 0U, error, error_size) != 0)
            return -1;
    return 0;
}

static int rewrite_bitmap_and_root(int fd, const PfsModel *model,
                                   const uint8_t *final_free,
                                   char *error, size_t error_size)
{
    uint64_t free_count = 0U;
    const uint32_t first_data = model->root.last_reserved + 1U;
    for (uint32_t block = first_data; block < model->root.disksize; ++block)
        if (final_free[block] != 0U)
            free_count++;
    if (free_count > UINT32_MAX) {
        set_error(error, error_size, "PFS3 free-block count exceeds the root field");
        return -1;
    }

    uint32_t bitmap_sequence = 0U;
    const uint32_t bitmap_indexes =
        (model->bitmap_blocks + PFS_INDEX_PER_BLOCK - 1U) / PFS_INDEX_PER_BLOCK;
    for (uint32_t group = 0U; group < bitmap_indexes; ++group) {
        uint8_t index_block[PFS_RESBLOCK_SIZE];
        if (read_reserved_block(fd, &model->root,
                                model->root.bitmap_index[group], index_block,
                                error, error_size,
                                "cannot reread PFS3 bitmap index") != 0)
            return -1;
        for (uint32_t slot = 0U;
             slot < PFS_INDEX_PER_BLOCK && bitmap_sequence < model->bitmap_blocks;
             ++slot, ++bitmap_sequence) {
            const uint32_t pointer =
                infiltratr_load_be32(index_block + PFS_INDEX_HEADER + slot * 4U);
            uint8_t bitmap[PFS_RESBLOCK_SIZE];
            if (read_reserved_block(fd, &model->root, pointer, bitmap,
                                    error, error_size,
                                    "cannot reread PFS3 allocation bitmap") != 0)
                return -1;
            memset(bitmap + 12U, 0, PFS_RESBLOCK_SIZE - 12U);
            const uint32_t base =
                first_data + bitmap_sequence * PFS_BITMAP_BITS;
            for (uint32_t bit = 0U; bit < PFS_BITMAP_BITS; ++bit) {
                const uint32_t block = base + bit;
                if (block >= model->root.disksize)
                    break;
                if (final_free[block] != 0U) {
                    const uint32_t word = bit / 32U;
                    const uint32_t within = bit % 32U;
                    uint32_t value =
                        infiltratr_load_be32(bitmap + 12U + word * 4U);
                    value |= UINT32_C(0x80000000) >> within;
                    infiltratr_store_be32(bitmap + 12U + word * 4U, value);
                }
            }
            if (write_exact(fd, bitmap, sizeof(bitmap),
                            (uint64_t)pointer * PFS_SECTOR_SIZE,
                            error, error_size,
                            "cannot update PFS3 allocation bitmap") != 0)
                return -1;
        }
    }

    uint8_t count[4];
    infiltratr_store_be32(count, (uint32_t)free_count);
    return write_exact(fd, count, sizeof(count),
                       (uint64_t)PFS_ROOT_SECTOR * PFS_SECTOR_SIZE + 68U,
                       error, error_size,
                       "cannot update PFS3 root free-block count");
}

int pfs3_verify_layout(const char *path, bool growth, unsigned growth_percent,
                       char *error, size_t error_size)
{
    if (growth_percent == 0U || growth_percent > 100U) {
        set_error(error, error_size, "invalid PFS3 Growth Defrag percentage");
        return -1;
    }
    PfsModel model;
    if (model_load(path, &model, error, error_size) != 0)
        return -1;
    uint64_t cursor = model.root.last_reserved + 1U;
    int result = 0;
    for (size_t index = 0U; index < model.file_count; ++index) {
        const PfsFile *file = &model.files[index];
        if (file->blocks != 0U) {
            if (file->fragmented || file->extent_count == 0U ||
                file->extents[0].block != cursor) {
                set_error(error, error_size,
                          "PFS3 file placement is not canonical");
                result = -1;
                break;
            }
            uint64_t position = cursor;
            for (size_t extent = 0U; extent < file->extent_count; ++extent) {
                if (file->extents[extent].block != position) {
                    set_error(error, error_size,
                              "PFS3 canonical file contains an internal gap");
                    result = -1;
                    break;
                }
                position += file->extents[extent].clusters;
            }
            if (result != 0)
                break;
            cursor += file->blocks;
        }
        if (growth) {
            const uint64_t reserve =
                (file->blocks * growth_percent + 99U) / 100U;
            if (cursor + reserve > model.root.disksize) {
                set_error(error, error_size, "PFS3 Growth Defrag reserve exceeds media");
                result = -1;
                break;
            }
            for (uint64_t block = cursor; block < cursor + reserve; ++block)
                if (model.free_map[block] == 0U) {
                    set_error(error, error_size,
                              "PFS3 Growth Defrag reserve is not free");
                    result = -1;
                    break;
                }
            if (result != 0)
                break;
            cursor += reserve;
        }
    }
    model_free(&model);
    return result;
}

int pfs3_build_stage(const char *source, const char *stage, bool growth,
                     unsigned growth_percent, bool live_updates,
                     uint64_t *commit_bytes, char *error, size_t error_size)
{
    (void)live_updates;
    if (source == NULL || stage == NULL ||
        growth_percent == 0U || growth_percent > 100U) {
        set_error(error, error_size, "invalid PFS3 staging arguments");
        return -1;
    }
    PfsModel model;
    if (model_load(source, &model, error, error_size) != 0)
        return -1;
    if (model.transaction_pending) {
        model_free(&model);
        set_error(error, error_size,
                  "PFS3 root extension contains a postponed operation; Recover/repair that state first");
        return -1;
    }

    uint8_t *final_free = calloc(model.root.disksize, 1U);
    if (final_free == NULL) {
        model_free(&model);
        set_error(error, error_size, "out of memory planning PFS3 relayout");
        return -1;
    }
    const uint32_t first_data = model.root.last_reserved + 1U;
    memset(final_free + first_data, 1, model.root.disksize - first_data);

    uint64_t cursor = first_data;
    for (size_t index = 0U; index < model.file_count; ++index) {
        PfsFile *file = &model.files[index];
        const uint64_t reserve = growth
            ? (file->blocks * growth_percent + 99U) / 100U : 0U;
        if (cursor + file->blocks + reserve > model.root.disksize ||
            cursor > UINT32_MAX) {
            free(final_free);
            model_free(&model);
            set_error(error, error_size,
                      "PFS3 has insufficient space for the requested canonical layout");
            return -1;
        }
        file->target = (uint32_t)cursor;
        for (uint64_t block = 0U; block < file->blocks; ++block)
            final_free[cursor + block] = 0U;
        cursor += file->blocks + reserve;
    }

    (void)unlink(stage);
    int stage_flags = O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
    stage_flags |= O_NOFOLLOW;
#endif
    int stage_fd = open(stage, stage_flags, 0600);
    if (stage_fd < 0) {
        free(final_free);
        model_free(&model);
        set_errno_error(error, error_size, "cannot create PFS3 stage");
        return -1;
    }
    int result = -1;
    if (ftruncate(stage_fd, (off_t)model.filesystem_bytes) != 0 ||
        copy_bytes(model.fd, stage_fd, model.filesystem_bytes,
                   NULL, error, error_size) != 0)
        goto cleanup;

    for (size_t index = 0U; index < model.file_count; ++index) {
        const PfsFile *file = &model.files[index];
        if (file->blocks != 0U &&
            copy_file_payload(&model, stage_fd, file, file->target,
                              error, error_size) != 0)
            goto cleanup;
        if (rewrite_file_anodes(stage_fd, file, file->target,
                                error, error_size) != 0)
            goto cleanup;
    }
    if (rewrite_bitmap_and_root(stage_fd, &model, final_free,
                                error, error_size) != 0 ||
        fsync(stage_fd) != 0) {
        if (error == NULL || *error == '\0')
            set_errno_error(error, error_size, "cannot persist PFS3 stage");
        goto cleanup;
    }
    if (close(stage_fd) != 0) {
        stage_fd = -1;
        set_errno_error(error, error_size, "cannot close PFS3 stage");
        goto cleanup;
    }
    stage_fd = -1;
    if (pfs3_verify_layout(stage, growth, growth_percent,
                           error, error_size) != 0)
        goto cleanup;
    if (commit_bytes != NULL)
        *commit_bytes = model.filesystem_bytes;
    result = 0;

cleanup:
    if (stage_fd >= 0)
        (void)close(stage_fd);
    if (result != 0)
        (void)unlink(stage);
    free(final_free);
    model_free(&model);
    return result;
}

int pfs3_commit_stage(const char *stage, const char *target, uint64_t *written,
                      char *error, size_t error_size)
{
    Pfs3Analysis analysis;
    if (pfs3_analyse(stage, &analysis, NULL, 0U, error, error_size) != 0)
        return -1;
    int source_fd = open(stage, O_RDONLY | O_CLOEXEC);
    if (source_fd < 0) {
        set_errno_error(error, error_size, "cannot open PFS3 recovery stage");
        return -1;
    }
    int target_fd = ld_device_open_verified_fd(target, true, NULL, 0U);
    if (target_fd < 0) {
        (void)close(source_fd);
        set_errno_error(error, error_size, "cannot open verified PFS3 target");
        return -1;
    }
    int result = copy_bytes(source_fd, target_fd, analysis.filesystem_bytes,
                            written, error, error_size);
    if (result == 0 && fsync(target_fd) != 0) {
        set_errno_error(error, error_size, "cannot persist PFS3 target");
        result = -1;
    }
    (void)close(target_fd);
    (void)close(source_fd);
    return result;
}
