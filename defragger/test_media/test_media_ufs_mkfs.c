// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * First-party bounded UFS2 Test Media creator.
 *
 * This is deliberately not a general replacement for BSD makefs.  It owns the
 * exact UFS2 fixture contract used by Defragmenter Test Media: one little-endian
 * cylinder group, 64 KiB blocks, 8 KiB fragments, a flat root directory, and
 * deterministic regular-file payload copied byte-for-byte from the generated
 * source tree.  Large regular files use one single-indirect block; the 64 KiB
 * block size keeps the 200 MiB qualification corpus inside that bounded address
 * model.  Data blocks are separated by free blocks so the production UFS
 * analyser must observe real fragmentation rather than a synthetic status bit.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define UFS_BLOCK_SIZE UINT32_C(65536)
#define UFS_FRAGMENT_SIZE UINT32_C(8192)
#define UFS_FRAGMENTS_PER_BLOCK UINT32_C(8)
#define UFS_CG_SIZE UFS_BLOCK_SIZE
#define UFS_INODE_SIZE UINT32_C(256)
#define UFS_INODES_PER_GROUP UINT32_C(256)
#define UFS_INODES_PER_BLOCK (UFS_BLOCK_SIZE / UFS_INODE_SIZE)
#define UFS_INDIRECTS_PER_BLOCK (UFS_BLOCK_SIZE / UINT32_C(8))
#define UFS_SBLKNO UINT32_C(8)
#define UFS_CBLKNO UINT32_C(16)
#define UFS_IBLKNO UINT32_C(24)
#define UFS_DBLKNO UINT32_C(32)
#define UFS_CG_IUSED_OFFSET UINT32_C(168)
#define UFS_CG_FREE_OFFSET UINT32_C(200)
#define UFS_CG_MAGIC UINT32_C(0x00090255)
#define UFS2_MAGIC UINT32_C(0x19540119)
#define UFS2_MAGIC_OFFSET UINT32_C(1372)
#define UFS_IFDIR UINT16_C(0040000)
#define UFS_IFREG UINT16_C(0100000)
#define UFS_ROOT_INODE UINT32_C(2)
#define UFS_FIRST_FILE_INODE UINT32_C(3)
#define UFS_NDADDR UINT32_C(12)
#define UFS_DIR_TYPE_DIR UINT8_C(4)
#define UFS_DIR_TYPE_REG UINT8_C(8)

typedef struct {
    char *path;
    uint64_t size;
} SourceFile;

typedef struct {
    SourceFile *items;
    size_t count;
    size_t capacity;
} SourceList;

typedef struct {
    int fd;
    uint64_t image_bytes;
    uint32_t fragments;
    uint8_t *free_map;
    uint32_t next_fragment;
} UfsImage;

static void put_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void put_le64(uint8_t *p, uint64_t value)
{
    for (unsigned int index = 0U; index < 8U; ++index)
        p[index] = (uint8_t)(value >> (index * 8U));
}

static int write_full_at(int fd, const void *buffer, size_t bytes, uint64_t offset)
{
    const uint8_t *cursor = buffer;
    size_t done = 0U;
    while (done < bytes) {
        if (offset + done > (uint64_t)INT64_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
        const ssize_t wrote = pwrite(fd, cursor + done, bytes - done,
                                     (off_t)(offset + done));
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (wrote == 0) {
            errno = EIO;
            return -1;
        }
        done += (size_t)wrote;
    }
    return 0;
}

static void bit_set(uint8_t *map, uint32_t bit, int value)
{
    const uint8_t mask = (uint8_t)(1U << (bit & 7U));
    if (value)
        map[bit >> 3U] |= mask;
    else
        map[bit >> 3U] &= (uint8_t)~mask;
}

static int bit_get(const uint8_t *map, uint32_t bit)
{
    return (map[bit >> 3U] & (uint8_t)(1U << (bit & 7U))) != 0U;
}

static int source_list_push(SourceList *list, const char *path, uint64_t size)
{
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0U ? 32U : list->capacity * 2U;
        if (capacity < list->capacity ||
            capacity > SIZE_MAX / sizeof(*list->items)) {
            errno = EOVERFLOW;
            return -1;
        }
        SourceFile *items = realloc(list->items, capacity * sizeof(*items));
        if (items == NULL)
            return -1;
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count].path = strdup(path);
    if (list->items[list->count].path == NULL)
        return -1;
    list->items[list->count].size = size;
    list->count++;
    return 0;
}

static void source_list_destroy(SourceList *list)
{
    if (list == NULL)
        return;
    for (size_t index = 0U; index < list->count; ++index)
        free(list->items[index].path);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int collect_source_tree(const char *path, SourceList *list)
{
    struct stat status;
    if (lstat(path, &status) != 0)
        return -1;
    if (S_ISREG(status.st_mode)) {
        if (status.st_size < 0) {
            errno = EINVAL;
            return -1;
        }
        return source_list_push(list, path, (uint64_t)status.st_size);
    }
    if (!S_ISDIR(status.st_mode)) {
        errno = EINVAL;
        return -1;
    }

    DIR *directory = opendir(path);
    if (directory == NULL)
        return -1;
    int result = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0)
                result = -1;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        const size_t base = strlen(path);
        const size_t name = strlen(entry->d_name);
        if (base > SIZE_MAX - name - 2U) {
            errno = EOVERFLOW;
            result = -1;
            break;
        }
        char *child = malloc(base + 1U + name + 1U);
        if (child == NULL) {
            result = -1;
            break;
        }
        memcpy(child, path, base);
        child[base] = '/';
        memcpy(child + base + 1U, entry->d_name, name + 1U);
        if (collect_source_tree(child, list) != 0)
            result = -1;
        free(child);
        if (result != 0)
            break;
    }
    const int saved = errno;
    (void)closedir(directory);
    errno = saved;
    return result;
}

static int compare_source_files(const void *left, const void *right)
{
    const SourceFile *a = left;
    const SourceFile *b = right;
    return strcmp(a->path, b->path);
}

static int parse_size_mib(const char *text, uint32_t *mib)
{
    if (text == NULL || mib == NULL || *text == '\0')
        return -1;
    char *end = NULL;
    errno = 0;
    const unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || value == 0ULL || value > UINT32_MAX)
        return -1;
    if (*end != 'm' && *end != 'M')
        return -1;
    ++end;
    if (*end != '\0')
        return -1;
    *mib = (uint32_t)value;
    return 0;
}

static int parse_arguments(int argc, char **argv, uint32_t *size_mib,
                           const char **image, const char **source)
{
    int index = 1;
    int saw_type = 0;
    int saw_order = 0;
    int saw_size = 0;
    int saw_options = 0;
    while (index < argc - 2) {
        if (strcmp(argv[index], "-t") == 0 && index + 1 < argc - 2) {
            if (strcmp(argv[index + 1], "ffs") != 0)
                return -1;
            saw_type = 1;
            index += 2;
        } else if (strcmp(argv[index], "-B") == 0 && index + 1 < argc - 2) {
            if (strcmp(argv[index + 1], "little") != 0)
                return -1;
            saw_order = 1;
            index += 2;
        } else if (strcmp(argv[index], "-s") == 0 && index + 1 < argc - 2) {
            if (parse_size_mib(argv[index + 1], size_mib) != 0)
                return -1;
            saw_size = 1;
            index += 2;
        } else if (strcmp(argv[index], "-o") == 0 && index + 1 < argc - 2) {
            /*
             * Defragmenter owns the geometry, so this compatibility argument is
             * accepted only for the UFS2 option set emitted by Test Media.
             */
            const char *value = argv[index + 1];
            if (strstr(value, "version=2") == NULL)
                return -1;
            saw_options = 1;
            index += 2;
        } else {
            return -1;
        }
    }
    if (index + 2 != argc || !saw_type || !saw_order || !saw_size ||
        !saw_options)
        return -1;
    *image = argv[index];
    *source = argv[index + 1];
    return 0;
}

static int image_init(UfsImage *image, const char *path, uint32_t size_mib)
{
    memset(image, 0, sizeof(*image));
    image->fd = -1;

    const uint64_t bytes = (uint64_t)size_mib * UINT64_C(1024) * UINT64_C(1024);
    if (bytes < UINT64_C(16) * 1024U * 1024U ||
        bytes % UFS_FRAGMENT_SIZE != 0U)
        return -1;
    const uint64_t fragments = bytes / UFS_FRAGMENT_SIZE;
    if (fragments <= UFS_DBLKNO || fragments > UINT32_MAX)
        return -1;
    const uint64_t free_bytes = (fragments + 7U) / 8U;
    if ((uint64_t)UFS_CG_FREE_OFFSET + free_bytes > UFS_CG_SIZE)
        return -1;

    const int fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (bytes > (uint64_t)INT64_MAX ||
        ftruncate(fd, (off_t)bytes) != 0) {
        const int saved = errno;
        (void)close(fd);
        errno = saved;
        return -1;
    }

    image->free_map = calloc((size_t)free_bytes, 1U);
    if (image->free_map == NULL) {
        const int saved = errno;
        (void)close(fd);
        errno = saved;
        return -1;
    }
    image->fd = fd;
    image->image_bytes = bytes;
    image->fragments = (uint32_t)fragments;
    image->next_fragment = UFS_DBLKNO;
    for (uint32_t fragment = UFS_DBLKNO; fragment < image->fragments; ++fragment)
        bit_set(image->free_map, fragment, 1);
    return 0;
}

static void image_destroy(UfsImage *image)
{
    if (image == NULL)
        return;
    if (image->fd >= 0)
        (void)close(image->fd);
    free(image->free_map);
    memset(image, 0, sizeof(*image));
    image->fd = -1;
}

static int allocate_fragments(UfsImage *image, uint32_t span, int leave_gap,
                              uint32_t *start)
{
    if (span == 0U || span > UFS_FRAGMENTS_PER_BLOCK)
        return -1;
    uint32_t candidate = image->next_fragment;
    const uint32_t remainder = candidate % UFS_FRAGMENTS_PER_BLOCK;
    if (remainder != 0U) {
        const uint32_t addition = UFS_FRAGMENTS_PER_BLOCK - remainder;
        if (candidate > UINT32_MAX - addition)
            return -1;
        candidate += addition;
    }
    if (candidate >= image->fragments ||
        span > image->fragments - candidate)
        return -1;
    for (uint32_t fragment = 0U; fragment < span; ++fragment) {
        if (!bit_get(image->free_map, candidate + fragment))
            return -1;
    }
    for (uint32_t fragment = 0U; fragment < span; ++fragment)
        bit_set(image->free_map, candidate + fragment, 0);
    *start = candidate;

    uint32_t advance = UFS_FRAGMENTS_PER_BLOCK;
    if (leave_gap != 0)
        advance += UFS_FRAGMENTS_PER_BLOCK;
    if (candidate > UINT32_MAX - advance)
        return -1;
    image->next_fragment = candidate + advance;
    return 0;
}

static int write_inode(UfsImage *image, uint32_t inode_number, uint16_t mode,
                       uint16_t links, uint64_t size, uint64_t sectors,
                       const uint64_t direct[UFS_NDADDR], uint64_t indirect)
{
    if (inode_number >= UFS_INODES_PER_GROUP)
        return -1;
    uint8_t inode[UFS_INODE_SIZE];
    memset(inode, 0, sizeof(inode));
    put_le16(inode + 0U, mode);
    put_le16(inode + 2U, links);
    put_le64(inode + 16U, size);
    put_le64(inode + 24U, sectors);
    for (uint32_t index = 0U; index < UFS_NDADDR; ++index)
        put_le64(inode + 112U + index * 8U, direct[index]);
    put_le64(inode + 208U, indirect);

    const uint64_t offset =
        (uint64_t)UFS_IBLKNO * UFS_FRAGMENT_SIZE +
        (uint64_t)inode_number * UFS_INODE_SIZE;
    return write_full_at(image->fd, inode, sizeof(inode), offset);
}

static int copy_file_block(int input, UfsImage *image, uint32_t fragment,
                           size_t bytes)
{
    uint8_t *buffer = malloc(UFS_BLOCK_SIZE);
    if (buffer == NULL)
        return -1;
    memset(buffer, 0, UFS_BLOCK_SIZE);
    size_t done = 0U;
    while (done < bytes) {
        const ssize_t got = read(input, buffer + done, bytes - done);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            free(buffer);
            return -1;
        }
        if (got == 0) {
            free(buffer);
            errno = EIO;
            return -1;
        }
        done += (size_t)got;
    }
    const int result = write_full_at(
        image->fd, buffer, UFS_BLOCK_SIZE,
        (uint64_t)fragment * UFS_FRAGMENT_SIZE);
    free(buffer);
    return result;
}

static int write_regular_file(UfsImage *image, const SourceFile *source,
                              uint32_t inode_number)
{
    const uint64_t blocks =
        (source->size + UFS_BLOCK_SIZE - 1U) / UFS_BLOCK_SIZE;
    if (blocks > UFS_NDADDR + UFS_INDIRECTS_PER_BLOCK) {
        errno = EFBIG;
        return -1;
    }

    uint64_t direct[UFS_NDADDR] = {0};
    uint8_t *indirect_block = NULL;
    uint32_t indirect_fragment = 0U;
    uint64_t sectors = 0U;
    if (blocks > UFS_NDADDR) {
        indirect_block = calloc(UFS_BLOCK_SIZE, 1U);
        if (indirect_block == NULL ||
            allocate_fragments(image, UFS_FRAGMENTS_PER_BLOCK, 0,
                               &indirect_fragment) != 0) {
            free(indirect_block);
            return -1;
        }
        sectors += UFS_BLOCK_SIZE / 512U;
    }

    const int input = open(source->path, O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        free(indirect_block);
        return -1;
    }

    uint64_t remaining = source->size;
    int result = 0;
    for (uint64_t logical = 0U; logical < blocks; ++logical) {
        const size_t take = remaining < UFS_BLOCK_SIZE
            ? (size_t)remaining : (size_t)UFS_BLOCK_SIZE;
        const uint32_t span = take == UFS_BLOCK_SIZE
            ? UFS_FRAGMENTS_PER_BLOCK
            : (uint32_t)((take + UFS_FRAGMENT_SIZE - 1U) /
                         UFS_FRAGMENT_SIZE);
        uint32_t fragment = 0U;
        if (allocate_fragments(image, span, 1, &fragment) != 0 ||
            copy_file_block(input, image, fragment, take) != 0) {
            result = -1;
            break;
        }
        sectors += (uint64_t)span * (UFS_FRAGMENT_SIZE / 512U);
        if (logical < UFS_NDADDR)
            direct[logical] = fragment;
        else
            put_le64(indirect_block + (logical - UFS_NDADDR) * 8U,
                     fragment);
        remaining -= take;
    }
    const int saved = errno;
    (void)close(input);
    errno = saved;

    if (result == 0 && indirect_block != NULL)
        result = write_full_at(
            image->fd, indirect_block, UFS_BLOCK_SIZE,
            (uint64_t)indirect_fragment * UFS_FRAGMENT_SIZE);
    free(indirect_block);
    if (result != 0)
        return -1;

    return write_inode(image, inode_number, (uint16_t)(UFS_IFREG | 0644U),
                       1U, source->size, sectors, direct,
                       blocks > UFS_NDADDR ? indirect_fragment : 0U);
}

static size_t dirent_minimum(size_t name_bytes)
{
    const size_t raw = 8U + name_bytes;
    return (raw + 3U) & ~(size_t)3U;
}

static int write_root_directory(UfsImage *image, size_t file_count,
                                uint32_t *root_fragment)
{
    uint8_t *block = calloc(UFS_BLOCK_SIZE, 1U);
    if (block == NULL)
        return -1;
    if (allocate_fragments(image, UFS_FRAGMENTS_PER_BLOCK, 0,
                           root_fragment) != 0) {
        free(block);
        return -1;
    }

    const size_t entries = file_count + 2U;
    size_t cursor = 0U;
    for (size_t index = 0U; index < entries; ++index) {
        char name[32];
        uint32_t inode;
        uint8_t type;
        if (index == 0U) {
            (void)snprintf(name, sizeof(name), ".");
            inode = UFS_ROOT_INODE;
            type = UFS_DIR_TYPE_DIR;
        } else if (index == 1U) {
            (void)snprintf(name, sizeof(name), "..");
            inode = UFS_ROOT_INODE;
            type = UFS_DIR_TYPE_DIR;
        } else {
            (void)snprintf(name, sizeof(name), "file%03zu", index - 2U);
            inode = UFS_FIRST_FILE_INODE + (uint32_t)(index - 2U);
            type = UFS_DIR_TYPE_REG;
        }
        const size_t name_bytes = strlen(name);
        size_t record = dirent_minimum(name_bytes);
        if (index + 1U == entries)
            record = UFS_BLOCK_SIZE - cursor;
        if (record > UINT16_MAX || cursor > UFS_BLOCK_SIZE - record) {
            free(block);
            errno = EOVERFLOW;
            return -1;
        }
        put_le32(block + cursor, inode);
        put_le16(block + cursor + 4U, (uint16_t)record);
        block[cursor + 6U] = type;
        block[cursor + 7U] = (uint8_t)name_bytes;
        memcpy(block + cursor + 8U, name, name_bytes);
        cursor += record;
    }

    const int result = write_full_at(
        image->fd, block, UFS_BLOCK_SIZE,
        (uint64_t)*root_fragment * UFS_FRAGMENT_SIZE);
    free(block);
    return result;
}

static void count_free(const UfsImage *image, uint64_t *blocks,
                       uint64_t *fragments)
{
    uint64_t full = 0U;
    uint64_t tails = 0U;
    for (uint32_t start = 0U; start < image->fragments;
         start += UFS_FRAGMENTS_PER_BLOCK) {
        const uint32_t take =
            image->fragments - start < UFS_FRAGMENTS_PER_BLOCK
                ? image->fragments - start
                : UFS_FRAGMENTS_PER_BLOCK;
        uint32_t free_count = 0U;
        for (uint32_t index = 0U; index < take; ++index)
            free_count += (uint32_t)bit_get(image->free_map, start + index);
        if (take == UFS_FRAGMENTS_PER_BLOCK &&
            free_count == UFS_FRAGMENTS_PER_BLOCK)
            full++;
        else
            tails += free_count;
    }
    *blocks = full;
    *fragments = tails;
}

static int write_metadata(UfsImage *image, size_t file_count,
                          uint32_t root_fragment)
{
    uint64_t nbfree = 0U;
    uint64_t nffree = 0U;
    count_free(image, &nbfree, &nffree);
    if (nbfree > UINT32_MAX || nffree > UINT32_MAX)
        return -1;

    uint8_t *cg = calloc(UFS_CG_SIZE, 1U);
    uint8_t superblock[8192];
    if (cg == NULL)
        return -1;
    memset(superblock, 0, sizeof(superblock));

    put_le32(cg + 4U, UFS_CG_MAGIC);
    put_le32(cg + 12U, 0U);
    put_le32(cg + 20U, image->fragments);
    put_le32(cg + 24U, 1U);
    put_le32(cg + 28U, (uint32_t)nbfree);
    put_le32(cg + 32U,
             UFS_INODES_PER_GROUP -
                 (UFS_FIRST_FILE_INODE + (uint32_t)file_count));
    put_le32(cg + 36U, (uint32_t)nffree);
    put_le32(cg + 92U, UFS_CG_IUSED_OFFSET);
    put_le32(cg + 96U, UFS_CG_FREE_OFFSET);

    for (uint32_t inode = 0U;
         inode < UFS_FIRST_FILE_INODE + (uint32_t)file_count; ++inode)
        bit_set(cg + UFS_CG_IUSED_OFFSET, inode, 1);
    const size_t free_map_bytes = ((size_t)image->fragments + 7U) / 8U;
    memcpy(cg + UFS_CG_FREE_OFFSET, image->free_map, free_map_bytes);

    put_le32(superblock + 8U, UFS_SBLKNO);
    put_le32(superblock + 12U, UFS_CBLKNO);
    put_le32(superblock + 16U, UFS_IBLKNO);
    put_le32(superblock + 20U, UFS_DBLKNO);
    put_le32(superblock + 44U, 1U);
    put_le32(superblock + 48U, UFS_BLOCK_SIZE);
    put_le32(superblock + 52U, UFS_FRAGMENT_SIZE);
    put_le32(superblock + 56U, UFS_FRAGMENTS_PER_BLOCK);
    put_le32(superblock + 116U, UFS_INDIRECTS_PER_BLOCK);
    put_le32(superblock + 120U, UFS_INODES_PER_BLOCK);
    put_le32(superblock + 160U, UFS_CG_SIZE);
    put_le32(superblock + 184U, UFS_INODES_PER_GROUP);
    put_le32(superblock + 188U, image->fragments);
    superblock[209U] = 1U;
    put_le64(superblock + 1016U, nbfree);
    put_le64(superblock + 1032U, nffree);
    put_le64(superblock + 1080U, image->fragments);
    put_le64(superblock + 1088U, image->fragments - UFS_DBLKNO);
    put_le32(superblock + 1316U, 0U);
    put_le32(superblock + UFS2_MAGIC_OFFSET, UFS2_MAGIC);

    uint64_t root_direct[UFS_NDADDR] = {0};
    root_direct[0] = root_fragment;
    if (write_inode(image, UFS_ROOT_INODE,
                    (uint16_t)(UFS_IFDIR | 0755U), 2U,
                    UFS_BLOCK_SIZE, UFS_BLOCK_SIZE / 512U,
                    root_direct, 0U) != 0 ||
        write_full_at(image->fd, superblock, sizeof(superblock),
                      (uint64_t)UFS_SBLKNO * UFS_FRAGMENT_SIZE) != 0 ||
        write_full_at(image->fd, cg, UFS_CG_SIZE,
                      (uint64_t)UFS_CBLKNO * UFS_FRAGMENT_SIZE) != 0) {
        free(cg);
        return -1;
    }
    free(cg);
    return 0;
}

static int build_ufs2(const char *image_path, const char *source_path,
                      uint32_t size_mib)
{
    SourceList sources = {0};
    UfsImage image;
    memset(&image, 0, sizeof(image));
    image.fd = -1;

    if (collect_source_tree(source_path, &sources) != 0)
        goto failure;
    qsort(sources.items, sources.count, sizeof(*sources.items),
          compare_source_files);
    if (sources.count >
        (size_t)(UFS_INODES_PER_GROUP - UFS_FIRST_FILE_INODE)) {
        errno = E2BIG;
        goto failure;
    }
    if (image_init(&image, image_path, size_mib) != 0)
        goto failure;

    uint32_t root_fragment = 0U;
    if (write_root_directory(&image, sources.count, &root_fragment) != 0)
        goto failure;
    for (size_t index = 0U; index < sources.count; ++index) {
        if (write_regular_file(
                &image, &sources.items[index],
                UFS_FIRST_FILE_INODE + (uint32_t)index) != 0)
            goto failure;
    }
    if (write_metadata(&image, sources.count, root_fragment) != 0 ||
        fsync(image.fd) != 0)
        goto failure;

    image_destroy(&image);
    source_list_destroy(&sources);
    return 0;

failure:
    {
        const int saved = errno != 0 ? errno : EIO;
        image_destroy(&image);
        source_list_destroy(&sources);
        errno = saved;
        return -1;
    }
}

int main(int argc, char **argv)
{
    uint32_t size_mib = 0U;
    const char *image = NULL;
    const char *source = NULL;
    if (parse_arguments(argc, argv, &size_mib, &image, &source) != 0) {
        (void)fprintf(
            stderr,
            "usage: %s -t ffs -B little -s SIZEm -o version=2,... IMAGE SOURCE\n",
            argc > 0 ? argv[0] : "makefs");
        return 2;
    }
    if (build_ufs2(image, source, size_mib) != 0) {
        (void)fprintf(stderr, "%s: UFS2 fixture creation failed: %s\n",
                      argv[0], strerror(errno));
        return 1;
    }
    return 0;
}
