// SPDX-License-Identifier: GPL-3.0-or-later
#include "btrfs_native.h"

#include "infiltratr/endian.h"
#include "infiltratr/arithmetic.h"
#include "infiltratr/posix_io.h"
#include "ld_io.h"
#include "ld_stop.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define BTRFS_SUPER_OFFSET (64ULL * 1024ULL)
#define BTRFS_SUPER_SIZE 4096U
#define BTRFS_HEADER_SIZE 101U
#define BTRFS_ITEM_SIZE 25U
#define BTRFS_KEY_PTR_SIZE 33U
#define BTRFS_DISK_KEY_SIZE 17U
#define BTRFS_CHUNK_FIXED_SIZE 48U
#define BTRFS_STRIPE_SIZE 32U
#define BTRFS_MAX_TREE_LEVEL 8U
#define BTRFS_MAX_TREE_BLOCKS 8000000U
#define BTRFS_MAX_SYSTEM_ARRAY 2048U

#define BTRFS_INODE_ITEM 1U
#define BTRFS_EXTENT_DATA 108U
#define BTRFS_ROOT_ITEM 132U
#define BTRFS_EXTENT_ITEM 168U
#define BTRFS_METADATA_ITEM 169U
#define BTRFS_CHUNK_ITEM 228U
#define BTRFS_EXTENT_TREE_OBJECTID 2ULL
#define BTRFS_FS_TREE_OBJECTID 5ULL
#define BTRFS_FIRST_FREE_OBJECTID 256ULL

#define BTRFS_FILE_EXTENT_INLINE 0U
#define BTRFS_FILE_EXTENT_REG 1U
#define BTRFS_FILE_EXTENT_PREALLOC 2U

#define BTRFS_BLOCK_GROUP_RAID0 8ULL
#define BTRFS_BLOCK_GROUP_RAID10 64ULL
#define BTRFS_BLOCK_GROUP_RAID5 128ULL
#define BTRFS_BLOCK_GROUP_RAID6 256ULL
#define BTRFS_STRIPED_PROFILES \
    (BTRFS_BLOCK_GROUP_RAID0 | BTRFS_BLOCK_GROUP_RAID10 | \
     BTRFS_BLOCK_GROUP_RAID5 | BTRFS_BLOCK_GROUP_RAID6)

typedef struct {
    int fd;
    uint64_t size;
} Reader;

typedef struct {
    uint64_t objectid;
    uint8_t type;
    uint64_t offset;
} Key;

typedef struct {
    uint64_t devid;
    uint64_t physical;
} Stripe;

typedef struct {
    uint64_t logical;
    uint64_t length;
    uint64_t chunk_type;
    uint64_t stripe_len;
    Stripe *stripes;
    uint16_t stripe_count;
} Chunk;

typedef struct {
    Chunk *items;
    size_t count;
    size_t capacity;
} ChunkVec;

typedef struct {
    Key key;
    uint8_t *data;
    uint32_t size;
} TreeItem;

typedef struct {
    TreeItem *items;
    size_t count;
    size_t capacity;
} ItemVec;

typedef struct {
    BtrfsRange *items;
    size_t count;
    size_t capacity;
} RangeVec;

typedef struct {
    uint64_t *items;
    size_t count;
    size_t capacity;
} U64Vec;

typedef struct {
    uint64_t logical;
    uint8_t level;
} TreeRef;

typedef struct {
    TreeRef *items;
    size_t count;
    size_t capacity;
} RefVec;

typedef struct {
    uint64_t *slots;
    size_t capacity;
    size_t count;
} U64Set;

typedef struct {
    uint64_t objectid;
    uint64_t bytenr;
    uint64_t key_offset;
    uint32_t refs;
    uint8_t level;
} RootRecord;

typedef struct {
    RootRecord *items;
    size_t count;
    size_t capacity;
} RootVec;

typedef struct {
    uint64_t logical;
    uint64_t physical;
    uint64_t length;
    uint64_t disk_start;
    uint64_t disk_length;
    bool encoded;
} FileRun;

typedef struct {
    FileRun *items;
    size_t count;
    size_t capacity;
} RunVec;

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    if (error == NULL || error_size == 0U)
        return;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(error, error_size, format, args);
    va_end(args);
}

static int open_reader(const char *path, Reader *reader, char *error, size_t error_size)
{
    struct stat status;
    if (stat(path, &status) != 0) {
        set_error(error, error_size, "cannot stat Btrfs target: %s", strerror(errno));
        return -1;
    }
    if (!S_ISREG(status.st_mode) && !S_ISBLK(status.st_mode)) {
        set_error(error, error_size, "Btrfs target must be a block device or regular image");
        return -1;
    }
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        set_error(error, error_size, "cannot open Btrfs target: %s", strerror(errno));
        return -1;
    }
    uint64_t size = 0U;
    if (S_ISBLK(status.st_mode)) {
        if (ioctl(fd, BLKGETSIZE64, &size) != 0) {
            set_error(error, error_size, "cannot determine Btrfs device size: %s", strerror(errno));
            (void)close(fd);
            return -1;
        }
    } else {
        if (status.st_size < 0) {
            set_error(error, error_size, "invalid Btrfs image size");
            (void)close(fd);
            return -1;
        }
        size = (uint64_t)status.st_size;
    }
    reader->fd = fd;
    reader->size = size;
    return 0;
}

static void close_reader(Reader *reader)
{
    if (reader->fd >= 0)
        (void)close(reader->fd);
    reader->fd = -1;
    reader->size = 0U;
}

static int read_exact(const Reader *reader, uint64_t offset, void *buffer, size_t length,
                      char *error, size_t error_size)
{
    if (offset > reader->size || (uint64_t)length > reader->size - offset) {
        set_error(error, error_size, "Btrfs read lies outside the device");
        return -1;
    }
    if (infiltratr_pread_full(reader->fd, buffer, length, offset) != 0) {
        set_error(error, error_size, "cannot read Btrfs metadata: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int grow_array(void **items, size_t *capacity, size_t count, size_t item_size,
                      char *error, size_t error_size)
{
    if (count == SIZE_MAX) {
        set_error(error, error_size, "Btrfs metadata collection is too large");
        return -1;
    }
    if (!infiltratr_array_reserve(items, capacity, item_size, count + 1U, 16U)) {
        set_error(error, error_size, "out of memory or size range analysing Btrfs metadata");
        return -1;
    }
    return 0;
}

static int range_push(RangeVec *vec, uint64_t start, uint64_t end,
                      char *error, size_t error_size)
{
    if (end <= start)
        return 0;
    if (grow_array((void **)&vec->items, &vec->capacity, vec->count,
                   sizeof(*vec->items), error, error_size) != 0)
        return -1;
    vec->items[vec->count++] = (BtrfsRange){start, end};
    return 0;
}

static int u64_push(U64Vec *vec, uint64_t value, char *error, size_t error_size)
{
    if (grow_array((void **)&vec->items, &vec->capacity, vec->count,
                   sizeof(*vec->items), error, error_size) != 0)
        return -1;
    vec->items[vec->count++] = value;
    return 0;
}

static int ref_push(RefVec *vec, uint64_t logical, uint8_t level,
                    char *error, size_t error_size)
{
    if (grow_array((void **)&vec->items, &vec->capacity, vec->count,
                   sizeof(*vec->items), error, error_size) != 0)
        return -1;
    vec->items[vec->count++] = (TreeRef){logical, level};
    return 0;
}

static int item_push(ItemVec *vec, Key key, const uint8_t *data, uint32_t size,
                     char *error, size_t error_size)
{
    if (grow_array((void **)&vec->items, &vec->capacity, vec->count,
                   sizeof(*vec->items), error, error_size) != 0)
        return -1;
    uint8_t *copy = NULL;
    if (size != 0U) {
        copy = malloc(size);
        if (copy == NULL) {
            set_error(error, error_size, "out of memory copying Btrfs tree item");
            return -1;
        }
        memcpy(copy, data, size);
    }
    vec->items[vec->count++] = (TreeItem){key, copy, size};
    return 0;
}

static void item_vec_free(ItemVec *vec)
{
    for (size_t i = 0U; i < vec->count; ++i)
        free(vec->items[i].data);
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static int range_compare(const void *left, const void *right)
{
    const BtrfsRange *a = left;
    const BtrfsRange *b = right;
    if (a->start < b->start) return -1;
    if (a->start > b->start) return 1;
    if (a->end < b->end) return -1;
    if (a->end > b->end) return 1;
    return 0;
}

static void range_merge(RangeVec *vec)
{
    if (vec->count < 2U)
        return;
    qsort(vec->items, vec->count, sizeof(*vec->items), range_compare);
    size_t output = 0U;
    for (size_t i = 0U; i < vec->count; ++i) {
        const BtrfsRange current = vec->items[i];
        if (current.end <= current.start)
            continue;
        if (output != 0U && current.start <= vec->items[output - 1U].end) {
            if (current.end > vec->items[output - 1U].end)
                vec->items[output - 1U].end = current.end;
        } else {
            vec->items[output++] = current;
        }
    }
    vec->count = output;
}

static Key parse_key(const uint8_t *data)
{
    return (Key){infiltratr_load_le64(data), data[8], infiltratr_load_le64(data + 9U)};
}

static bool chunk_equal(const Chunk *a, const Chunk *b)
{
    if (a->logical != b->logical || a->length != b->length ||
        a->chunk_type != b->chunk_type || a->stripe_len != b->stripe_len ||
        a->stripe_count != b->stripe_count)
        return false;
    for (uint16_t i = 0U; i < a->stripe_count; ++i) {
        if (a->stripes[i].devid != b->stripes[i].devid ||
            a->stripes[i].physical != b->stripes[i].physical)
            return false;
    }
    return true;
}

static void chunk_free(Chunk *chunk)
{
    free(chunk->stripes);
    memset(chunk, 0, sizeof(*chunk));
}

static void chunk_vec_free(ChunkVec *vec)
{
    for (size_t i = 0U; i < vec->count; ++i)
        chunk_free(&vec->items[i]);
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static int parse_chunk(const uint8_t *data, size_t size, uint64_t logical,
                       Chunk *chunk, char *error, size_t error_size)
{
    if (size < BTRFS_CHUNK_FIXED_SIZE) {
        set_error(error, error_size, "truncated Btrfs chunk item");
        return -1;
    }
    const uint64_t length = infiltratr_load_le64(data);
    const uint64_t stripe_len = infiltratr_load_le64(data + 16U);
    const uint64_t chunk_type = infiltratr_load_le64(data + 24U);
    const uint16_t stripe_count = infiltratr_load_le16(data + 44U);
    if (length == 0U || stripe_len == 0U || stripe_count == 0U) {
        set_error(error, error_size, "invalid Btrfs chunk geometry");
        return -1;
    }
    const size_t stripe_bytes = BTRFS_CHUNK_FIXED_SIZE +
                                (size_t)stripe_count * BTRFS_STRIPE_SIZE;
    if (stripe_bytes > size) {
        set_error(error, error_size, "truncated Btrfs chunk stripes");
        return -1;
    }
    Stripe *stripes = calloc((size_t)stripe_count, sizeof(*stripes));
    if (stripes == NULL) {
        set_error(error, error_size, "out of memory reading Btrfs chunk stripes");
        return -1;
    }
    for (uint16_t i = 0U; i < stripe_count; ++i) {
        const size_t pos = BTRFS_CHUNK_FIXED_SIZE + (size_t)i * BTRFS_STRIPE_SIZE;
        stripes[i].devid = infiltratr_load_le64(data + pos);
        stripes[i].physical = infiltratr_load_le64(data + pos + 8U);
    }
    *chunk = (Chunk){logical, length, chunk_type, stripe_len, stripes, stripe_count};
    return 0;
}

static int chunk_add_unique(ChunkVec *vec, Chunk *chunk, char *error, size_t error_size)
{
    for (size_t i = 0U; i < vec->count; ++i) {
        if (vec->items[i].logical == chunk->logical) {
            if (!chunk_equal(&vec->items[i], chunk)) {
                set_error(error, error_size, "conflicting Btrfs chunk mappings");
                return -1;
            }
            chunk_free(chunk);
            return 0;
        }
    }
    if (grow_array((void **)&vec->items, &vec->capacity, vec->count,
                   sizeof(*vec->items), error, error_size) != 0)
        return -1;
    vec->items[vec->count++] = *chunk;
    memset(chunk, 0, sizeof(*chunk));
    return 0;
}

static int chunk_compare(const void *left, const void *right)
{
    const Chunk *a = left;
    const Chunk *b = right;
    if (a->logical < b->logical) return -1;
    if (a->logical > b->logical) return 1;
    return 0;
}

static int mapper_prepare(ChunkVec *chunks, uint64_t devid, uint64_t device_size,
                          char *error, size_t error_size)
{
    qsort(chunks->items, chunks->count, sizeof(*chunks->items), chunk_compare);
    uint64_t previous_end = 0U;
    bool have_previous = false;
    for (size_t i = 0U; i < chunks->count; ++i) {
        const Chunk *chunk = &chunks->items[i];
        if (chunk->logical > UINT64_MAX - chunk->length) {
            set_error(error, error_size, "Btrfs chunk address overflows");
            return -1;
        }
        if (have_previous && chunk->logical < previous_end) {
            set_error(error, error_size, "overlapping Btrfs chunks");
            return -1;
        }
        if ((chunk->chunk_type & BTRFS_STRIPED_PROFILES) != 0U) {
            set_error(error, error_size,
                      "striped Btrfs profiles are not yet supported by the native analyser");
            return -1;
        }
        size_t local = 0U;
        for (uint16_t s = 0U; s < chunk->stripe_count; ++s) {
            if (chunk->stripes[s].devid != devid)
                continue;
            const uint64_t physical = chunk->stripes[s].physical;
            if (physical > device_size || chunk->length > device_size - physical) {
                set_error(error, error_size, "Btrfs chunk stripe points outside the device");
                return -1;
            }
            local++;
        }
        if (local == 0U) {
            set_error(error, error_size, "Btrfs chunk has no stripe on this device");
            return -1;
        }
        previous_end = chunk->logical + chunk->length;
        have_previous = true;
    }
    return 0;
}

static const Chunk *mapper_find(const ChunkVec *chunks, uint64_t logical)
{
    size_t low = 0U;
    size_t high = chunks->count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2U;
        if (chunks->items[middle].logical <= logical)
            low = middle + 1U;
        else
            high = middle;
    }
    if (low == 0U)
        return NULL;
    const Chunk *chunk = &chunks->items[low - 1U];
    if (logical < chunk->logical || logical - chunk->logical >= chunk->length)
        return NULL;
    return chunk;
}

static int mapper_read_physical(const ChunkVec *chunks, uint64_t devid,
                                uint64_t logical, uint64_t length, uint64_t *physical,
                                char *error, size_t error_size)
{
    const Chunk *chunk = mapper_find(chunks, logical);
    if (chunk == NULL) {
        set_error(error, error_size, "Btrfs logical address has no chunk mapping");
        return -1;
    }
    const uint64_t delta = logical - chunk->logical;
    if (length > chunk->length - delta) {
        set_error(error, error_size, "Btrfs tree block crosses a chunk boundary");
        return -1;
    }
    for (uint16_t i = 0U; i < chunk->stripe_count; ++i) {
        if (chunk->stripes[i].devid == devid) {
            *physical = chunk->stripes[i].physical + delta;
            return 0;
        }
    }
    set_error(error, error_size, "Btrfs logical address is not stored on this device");
    return -1;
}

static int mapper_ranges(const ChunkVec *chunks, uint64_t devid, uint64_t device_size,
                         uint64_t logical, uint64_t length, RangeVec *output,
                         char *error, size_t error_size)
{
    if (length == 0U)
        return 0;
    uint64_t cursor = logical;
    uint64_t remaining = length;
    while (remaining != 0U) {
        const Chunk *chunk = mapper_find(chunks, cursor);
        if (chunk == NULL) {
            set_error(error, error_size, "Btrfs logical extent has no chunk mapping");
            return -1;
        }
        const uint64_t delta = cursor - chunk->logical;
        const uint64_t available = chunk->length - delta;
        const uint64_t take = remaining < available ? remaining : available;
        size_t local = 0U;
        for (uint16_t i = 0U; i < chunk->stripe_count; ++i) {
            if (chunk->stripes[i].devid != devid)
                continue;
            const uint64_t start = chunk->stripes[i].physical + delta;
            if (start > device_size || take > device_size - start) {
                set_error(error, error_size, "Btrfs physical extent points outside the device");
                return -1;
            }
            if (range_push(output, start, start + take, error, error_size) != 0)
                return -1;
            local++;
        }
        if (local == 0U) {
            set_error(error, error_size, "Btrfs extent has no local physical mirror");
            return -1;
        }
        cursor += take;
        remaining -= take;
    }
    return 0;
}

static uint64_t hash_u64(uint64_t value)
{
    value ^= value >> 33U;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33U;
    value *= UINT64_C(0xc4ceb9fe1a85ec53);
    value ^= value >> 33U;
    return value;
}

static int set_rehash(U64Set *set, size_t new_capacity, char *error, size_t error_size)
{
    uint64_t *slots = calloc(new_capacity, sizeof(*slots));
    if (slots == NULL) {
        set_error(error, error_size, "out of memory tracking Btrfs tree blocks");
        return -1;
    }
    for (size_t i = 0U; i < set->capacity; ++i) {
        const uint64_t value = set->slots[i];
        if (value == 0U)
            continue;
        size_t pos = (size_t)(hash_u64(value) & (uint64_t)(new_capacity - 1U));
        while (slots[pos] != 0U)
            pos = (pos + 1U) & (new_capacity - 1U);
        slots[pos] = value;
    }
    free(set->slots);
    set->slots = slots;
    set->capacity = new_capacity;
    return 0;
}

static int set_insert(U64Set *set, uint64_t value, bool *inserted,
                      char *error, size_t error_size)
{
    if (value == 0U) {
        set_error(error, error_size, "invalid zero Btrfs tree pointer");
        return -1;
    }
    if (set->capacity == 0U && set_rehash(set, 1024U, error, error_size) != 0)
        return -1;
    if ((set->count + 1U) * 10U >= set->capacity * 7U) {
        if (set->capacity > SIZE_MAX / 2U ||
            set_rehash(set, set->capacity * 2U, error, error_size) != 0)
            return -1;
    }
    size_t pos = (size_t)(hash_u64(value) & (uint64_t)(set->capacity - 1U));
    while (set->slots[pos] != 0U) {
        if (set->slots[pos] == value) {
            *inserted = false;
            return 0;
        }
        pos = (pos + 1U) & (set->capacity - 1U);
    }
    set->slots[pos] = value;
    set->count++;
    *inserted = true;
    return 0;
}

static int tree_walk(const Reader *reader, const ChunkVec *chunks, uint64_t devid,
                     uint32_t node_size, uint64_t root, uint8_t root_level,
                     ItemVec *items, U64Vec *blocks, char *error, size_t error_size)
{
    RefVec stack = {0};
    U64Set visited = {0};
    uint8_t *raw = malloc(node_size);
    if (raw == NULL) {
        set_error(error, error_size, "out of memory reading Btrfs tree block");
        return -1;
    }
    if (ref_push(&stack, root, root_level, error, error_size) != 0) {
        free(raw);
        return -1;
    }
    int result = -1;
    while (stack.count != 0U) {
        const TreeRef current = stack.items[--stack.count];
        bool inserted = false;
        if (set_insert(&visited, current.logical, &inserted, error, error_size) != 0)
            goto done;
        if (!inserted)
            continue;
        if (visited.count > BTRFS_MAX_TREE_BLOCKS) {
            set_error(error, error_size, "Btrfs tree traversal exceeded the safety limit");
            goto done;
        }
        if (current.logical % node_size != 0U) {
            set_error(error, error_size, "invalid Btrfs tree-block address");
            goto done;
        }
        uint64_t physical = 0U;
        if (mapper_read_physical(chunks, devid, current.logical, node_size, &physical,
                                 error, error_size) != 0 ||
            read_exact(reader, physical, raw, node_size, error, error_size) != 0)
            goto done;
        if (infiltratr_load_le64(raw + 48U) != current.logical) {
            set_error(error, error_size, "Btrfs tree-block bytenr mismatch");
            goto done;
        }
        const uint8_t level = raw[100U];
        if (level > BTRFS_MAX_TREE_LEVEL || level != current.level) {
            set_error(error, error_size, "invalid Btrfs tree level");
            goto done;
        }
        const uint32_t nritems = infiltratr_load_le32(raw + 96U);
        const size_t element_size = level == 0U ? BTRFS_ITEM_SIZE : BTRFS_KEY_PTR_SIZE;
        if ((size_t)nritems > (SIZE_MAX - BTRFS_HEADER_SIZE) / element_size ||
            BTRFS_HEADER_SIZE + (size_t)nritems * element_size > node_size) {
            set_error(error, error_size, "invalid Btrfs tree item count");
            goto done;
        }
        if (u64_push(blocks, current.logical, error, error_size) != 0)
            goto done;
        if (level == 0U) {
            const size_t table_end = BTRFS_HEADER_SIZE + (size_t)nritems * BTRFS_ITEM_SIZE;
            for (uint32_t index = 0U; index < nritems; ++index) {
                const size_t pos = BTRFS_HEADER_SIZE + (size_t)index * BTRFS_ITEM_SIZE;
                const Key key = parse_key(raw + pos);
                const uint32_t relative = infiltratr_load_le32(raw + pos + 17U);
                const uint32_t data_size = infiltratr_load_le32(raw + pos + 21U);
                if ((uint64_t)BTRFS_HEADER_SIZE + relative > node_size) {
                    set_error(error, error_size, "Btrfs leaf item lies outside the tree block");
                    goto done;
                }
                const size_t data_offset = BTRFS_HEADER_SIZE + (size_t)relative;
                if (data_offset < table_end || data_size > node_size - data_offset) {
                    set_error(error, error_size, data_offset < table_end ?
                              "Btrfs leaf item overlaps its item table" :
                              "Btrfs leaf item lies outside the tree block");
                    goto done;
                }
                if (item_push(items, key, raw + data_offset, data_size, error, error_size) != 0)
                    goto done;
            }
        } else {
            for (uint32_t reverse = nritems; reverse != 0U; --reverse) {
                const uint32_t index = reverse - 1U;
                const size_t pos = BTRFS_HEADER_SIZE + (size_t)index * BTRFS_KEY_PTR_SIZE;
                const uint64_t child = infiltratr_load_le64(raw + pos + 17U);
                if (child == 0U) {
                    set_error(error, error_size, "invalid Btrfs child pointer");
                    goto done;
                }
                if (ref_push(&stack, child, (uint8_t)(level - 1U), error, error_size) != 0)
                    goto done;
            }
        }
    }
    result = 0;
done:
    free(raw);
    free(stack.items);
    free(visited.slots);
    return result;
}

static int parse_system_chunks(const uint8_t *superblock, ChunkVec *chunks,
                               char *error, size_t error_size)
{
    const uint32_t size = infiltratr_load_le32(superblock + 160U);
    if (size == 0U || size > BTRFS_MAX_SYSTEM_ARRAY || 811U + size > BTRFS_SUPER_SIZE) {
        set_error(error, error_size, "invalid Btrfs system chunk array size");
        return -1;
    }
    const uint8_t *data = superblock + 811U;
    size_t pos = 0U;
    while (pos < size) {
        if (size - pos < BTRFS_DISK_KEY_SIZE + BTRFS_CHUNK_FIXED_SIZE) {
            set_error(error, error_size, "truncated Btrfs system chunk array");
            return -1;
        }
        const Key key = parse_key(data + pos);
        if (key.type != BTRFS_CHUNK_ITEM) {
            set_error(error, error_size, "unexpected key in Btrfs system chunk array");
            return -1;
        }
        const uint16_t stripes = infiltratr_load_le16(data + pos + BTRFS_DISK_KEY_SIZE + 44U);
        const size_t item_size = BTRFS_CHUNK_FIXED_SIZE + (size_t)stripes * BTRFS_STRIPE_SIZE;
        if (item_size > size - pos - BTRFS_DISK_KEY_SIZE) {
            set_error(error, error_size, "truncated Btrfs system chunk item");
            return -1;
        }
        Chunk chunk = {0};
        if (parse_chunk(data + pos + BTRFS_DISK_KEY_SIZE, item_size, key.offset,
                        &chunk, error, error_size) != 0)
            return -1;
        if (chunk_add_unique(chunks, &chunk, error, error_size) != 0) {
            chunk_free(&chunk);
            return -1;
        }
        pos += BTRFS_DISK_KEY_SIZE + item_size;
    }
    return 0;
}

static int root_push_or_update(RootVec *roots, uint64_t objectid, uint64_t bytenr,
                               uint32_t refs, uint8_t level, uint64_t key_offset,
                               char *error, size_t error_size)
{
    for (size_t i = 0U; i < roots->count; ++i) {
        if (roots->items[i].objectid != objectid)
            continue;
        if (key_offset >= roots->items[i].key_offset)
            roots->items[i] = (RootRecord){objectid, bytenr, key_offset, refs, level};
        return 0;
    }
    if (grow_array((void **)&roots->items, &roots->capacity, roots->count,
                   sizeof(*roots->items), error, error_size) != 0)
        return -1;
    roots->items[roots->count++] = (RootRecord){objectid, bytenr, key_offset, refs, level};
    return 0;
}

static int collect_roots(const ItemVec *items, RootVec *roots,
                         char *error, size_t error_size)
{
    for (size_t i = 0U; i < items->count; ++i) {
        const TreeItem *item = &items->items[i];
        if (item->key.type != BTRFS_ROOT_ITEM || item->size < 239U)
            continue;
        const uint64_t bytenr = infiltratr_load_le64(item->data + 176U);
        const uint32_t refs = infiltratr_load_le32(item->data + 216U);
        const uint8_t level = item->data[238U];
        if (bytenr != 0U && level <= BTRFS_MAX_TREE_LEVEL &&
            root_push_or_update(roots, item->key.objectid, bytenr, refs, level,
                                item->key.offset, error, error_size) != 0)
            return -1;
    }
    return 0;
}

static const RootRecord *find_root(const RootVec *roots, uint64_t objectid)
{
    for (size_t i = 0U; i < roots->count; ++i)
        if (roots->items[i].objectid == objectid)
            return &roots->items[i];
    return NULL;
}

static int run_push_coalesced(RunVec *runs, FileRun run,
                              char *error, size_t error_size)
{
    if (run.length == 0U)
        return 0;
    if (runs->count != 0U) {
        FileRun *last = &runs->items[runs->count - 1U];
        if (!last->encoded && !run.encoded &&
            last->logical <= UINT64_MAX - last->length &&
            last->physical <= UINT64_MAX - last->length &&
            last->logical + last->length == run.logical &&
            last->physical + last->length == run.physical) {
            if (last->length > UINT64_MAX - run.length ||
                last->disk_length > UINT64_MAX - run.disk_length) {
                set_error(error, error_size, "Btrfs file extent length overflows");
                return -1;
            }
            last->length += run.length;
            last->disk_length += run.disk_length;
            return 0;
        }
    }
    if (grow_array((void **)&runs->items, &runs->capacity, runs->count,
                   sizeof(*runs->items), error, error_size) != 0)
        return -1;
    runs->items[runs->count++] = run;
    return 0;
}

static int finish_inode(uint32_t mode, bool have_inode, RunVec *runs,
                        BtrfsAnalysis *analysis, RangeVec *fragmented,
                        char *error, size_t error_size)
{
    if (!have_inode) {
        runs->count = 0U;
        return 0;
    }
    const uint32_t type = mode & (uint32_t)S_IFMT;
    if (type == (uint32_t)S_IFREG) {
        analysis->regular_files++;
        if (runs->count > 1U) {
            analysis->fragmented_files++;
            for (size_t i = 0U; i < runs->count; ++i) {
                const FileRun *run = &runs->items[i];
                if (run->disk_start > UINT64_MAX - run->disk_length) {
                    set_error(error, error_size, "Btrfs fragmented range overflows");
                    return -1;
                }
                if (range_push(fragmented, run->disk_start,
                               run->disk_start + run->disk_length,
                               error, error_size) != 0)
                    return -1;
            }
        }
    } else if (type == (uint32_t)S_IFDIR) {
        analysis->directories++;
    }
    runs->count = 0U;
    return 0;
}

static int scan_filesystem_tree(const ItemVec *items, const ChunkVec *chunks,
                                uint64_t devid, uint64_t device_size,
                                BtrfsAnalysis *analysis, RangeVec *fragmented,
                                char *error, size_t error_size)
{
    uint64_t current_inode = UINT64_MAX;
    uint32_t inode_mode = 0U;
    bool have_inode = false;
    RunVec runs = {0};
    int result = -1;
    for (size_t i = 0U; i < items->count; ++i) {
        const TreeItem *item = &items->items[i];
        if (item->key.objectid != current_inode) {
            if (current_inode != UINT64_MAX &&
                finish_inode(inode_mode, have_inode, &runs, analysis, fragmented,
                             error, error_size) != 0)
                goto done;
            current_inode = item->key.objectid;
            inode_mode = 0U;
            have_inode = false;
        }
        if (item->key.type == BTRFS_INODE_ITEM && item->size >= 56U) {
            inode_mode = infiltratr_load_le32(item->data + 52U);
            have_inode = true;
            continue;
        }
        if (item->key.type != BTRFS_EXTENT_DATA || item->size < 21U)
            continue;
        const uint8_t extent_type = item->data[20U];
        if (extent_type == BTRFS_FILE_EXTENT_INLINE)
            continue;
        if ((extent_type != BTRFS_FILE_EXTENT_REG &&
             extent_type != BTRFS_FILE_EXTENT_PREALLOC) || item->size < 53U) {
            analysis->malformed_items++;
            continue;
        }
        const uint64_t disk_bytenr = infiltratr_load_le64(item->data + 21U);
        const uint64_t disk_num_bytes = infiltratr_load_le64(item->data + 29U);
        const uint64_t extent_offset = infiltratr_load_le64(item->data + 37U);
        const uint64_t num_bytes = infiltratr_load_le64(item->data + 45U);
        if (disk_bytenr == 0U || num_bytes == 0U)
            continue;
        const bool encoded = item->data[16U] != 0U || item->data[17U] != 0U ||
                             infiltratr_load_le16(item->data + 18U) != 0U;
        uint64_t logical = disk_bytenr;
        uint64_t length = encoded ? disk_num_bytes : num_bytes;
        if (!encoded) {
            if (disk_bytenr > UINT64_MAX - extent_offset) {
                analysis->malformed_items++;
                continue;
            }
            logical += extent_offset;
        }
        if (length == 0U) {
            analysis->malformed_items++;
            continue;
        }
        RangeVec physical = {0};
        char local_error[256] = {0};
        if (mapper_ranges(chunks, devid, device_size, logical, length, &physical,
                          local_error, sizeof(local_error)) != 0 || physical.count == 0U) {
            free(physical.items);
            analysis->malformed_items++;
            continue;
        }
        const uint64_t disk_start = physical.items[0].start;
        const uint64_t disk_length = physical.items[0].end - physical.items[0].start;
        free(physical.items);
        const FileRun run = {
            item->key.offset,
            disk_start,
            num_bytes,
            disk_start,
            encoded ? disk_length : num_bytes,
            encoded,
        };
        if (run_push_coalesced(&runs, run, error, error_size) != 0)
            goto done;
    }
    if (current_inode != UINT64_MAX &&
        finish_inode(inode_mode, have_inode, &runs, analysis, fragmented,
                     error, error_size) != 0)
        goto done;
    result = 0;
done:
    free(runs.items);
    return result;
}

static int scan_filesystems(const Reader *reader, const ChunkVec *chunks, uint64_t devid,
                            uint32_t node_size, const RootVec *roots,
                            BtrfsAnalysis *analysis, RangeVec *fragmented,
                            char *error, size_t error_size)
{
    for (size_t i = 0U; i < roots->count; ++i) {
        const RootRecord *record = &roots->items[i];
        if (record->refs == 0U ||
            (record->objectid != BTRFS_FS_TREE_OBJECTID &&
             record->objectid < BTRFS_FIRST_FREE_OBJECTID))
            continue;
        analysis->filesystem_roots_scanned++;
        ItemVec items = {0};
        U64Vec blocks = {0};
        char local_error[256] = {0};
        if (tree_walk(reader, chunks, devid, node_size, record->bytenr, record->level,
                      &items, &blocks, local_error, sizeof(local_error)) != 0) {
            analysis->malformed_items++;
            item_vec_free(&items);
            free(blocks.items);
            continue;
        }
        analysis->filesystem_tree_blocks += (uint64_t)blocks.count;
        if (scan_filesystem_tree(&items, chunks, devid, analysis->total_bytes,
                                 analysis, fragmented, error, error_size) != 0) {
            item_vec_free(&items);
            free(blocks.items);
            return -1;
        }
        item_vec_free(&items);
        free(blocks.items);
    }
    return 0;
}

bool btrfs_probe(const char *path)
{
    Reader reader = {.fd = -1, .size = 0U};
    char error[128];
    if (open_reader(path, &reader, error, sizeof(error)) != 0)
        return false;
    uint8_t magic[8];
    const bool matched =
        read_exact(&reader, BTRFS_SUPER_OFFSET + 0x40U, magic, sizeof(magic),
                   error, sizeof(error)) == 0 &&
        memcmp(magic, "_BHRfS_M", sizeof(magic)) == 0;
    close_reader(&reader);
    return matched;
}

void btrfs_analysis_free(BtrfsAnalysis *analysis)
{
    if (analysis == NULL)
        return;
    free(analysis->used_ranges);
    free(analysis->fragmented_ranges);
    memset(analysis, 0, sizeof(*analysis));
}

int btrfs_analyse(const char *path, BtrfsAnalysis *analysis,
                  char *error, size_t error_size)
{
    if (analysis == NULL) {
        set_error(error, error_size, "missing Btrfs analysis output");
        return -1;
    }
    memset(analysis, 0, sizeof(*analysis));
    Reader reader = {.fd = -1, .size = 0U};
    ChunkVec chunks = {0};
    ItemVec chunk_items = {0};
    U64Vec chunk_blocks = {0};
    ItemVec root_items = {0};
    U64Vec root_blocks = {0};
    ItemVec extent_items = {0};
    U64Vec extent_blocks = {0};
    RootVec roots = {0};
    RangeVec used = {0};
    RangeVec fragmented = {0};
    int result = -1;

    if (open_reader(path, &reader, error, error_size) != 0)
        goto done;
    if (reader.size < BTRFS_SUPER_OFFSET + BTRFS_SUPER_SIZE) {
        set_error(error, error_size, "Btrfs target is too small for a superblock");
        goto done;
    }
    uint8_t superblock[BTRFS_SUPER_SIZE];
    if (read_exact(&reader, BTRFS_SUPER_OFFSET, superblock, sizeof(superblock),
                   error, error_size) != 0)
        goto done;
    if (memcmp(superblock + 0x40U, "_BHRfS_M", 8U) != 0) {
        set_error(error, error_size, "not a Btrfs volume");
        goto done;
    }

    const uint64_t root = infiltratr_load_le64(superblock + 80U);
    const uint64_t chunk_root = infiltratr_load_le64(superblock + 88U);
    const uint64_t total_bytes = infiltratr_load_le64(superblock + 112U);
    const uint64_t bytes_used = infiltratr_load_le64(superblock + 120U);
    const uint64_t num_devices = infiltratr_load_le64(superblock + 136U);
    const uint32_t sector_size = infiltratr_load_le32(superblock + 144U);
    const uint32_t node_size = infiltratr_load_le32(superblock + 148U);
    const uint8_t root_level = superblock[198U];
    const uint8_t chunk_root_level = superblock[199U];
    const uint64_t devid = infiltratr_load_le64(superblock + 201U);

    if (num_devices != 1U) {
        set_error(error, error_size,
                  "native exact Btrfs analysis currently supports single-device filesystems only");
        goto done;
    }
    if (total_bytes == 0U || total_bytes > reader.size) {
        set_error(error, error_size, "invalid Btrfs device size");
        goto done;
    }
    if (sector_size < 4096U || (sector_size & (sector_size - 1U)) != 0U) {
        set_error(error, error_size, "unsupported Btrfs sector size");
        goto done;
    }
    if (node_size < sector_size || node_size > 65536U ||
        (node_size & (node_size - 1U)) != 0U) {
        set_error(error, error_size, "unsupported Btrfs node size");
        goto done;
    }
    if (root == 0U || chunk_root == 0U || root_level > BTRFS_MAX_TREE_LEVEL ||
        chunk_root_level > BTRFS_MAX_TREE_LEVEL) {
        set_error(error, error_size, "invalid Btrfs tree roots");
        goto done;
    }

    analysis->total_bytes = total_bytes;
    analysis->physical_bytes = reader.size;
    analysis->logical_bytes_used = bytes_used;
    analysis->generation = infiltratr_load_le64(superblock + 72U);
    analysis->compat_ro_flags = infiltratr_load_le64(superblock + 180U);
    analysis->incompat_flags = infiltratr_load_le64(superblock + 188U);
    analysis->sector_size = sector_size;
    analysis->node_size = node_size;
    analysis->device_id = devid;
    analysis->fragmentation_available = true;

    if (parse_system_chunks(superblock, &chunks, error, error_size) != 0 ||
        mapper_prepare(&chunks, devid, total_bytes, error, error_size) != 0)
        goto done;
    if (tree_walk(&reader, &chunks, devid, node_size, chunk_root, chunk_root_level,
                  &chunk_items, &chunk_blocks, error, error_size) != 0)
        goto done;
    for (size_t i = 0U; i < chunk_items.count; ++i) {
        const TreeItem *item = &chunk_items.items[i];
        if (item->key.type != BTRFS_CHUNK_ITEM)
            continue;
        Chunk chunk = {0};
        if (parse_chunk(item->data, item->size, item->key.offset,
                        &chunk, error, error_size) != 0)
            goto done;
        if (chunk_add_unique(&chunks, &chunk, error, error_size) != 0) {
            chunk_free(&chunk);
            goto done;
        }
    }
    if (mapper_prepare(&chunks, devid, total_bytes, error, error_size) != 0)
        goto done;

    if (tree_walk(&reader, &chunks, devid, node_size, root, root_level,
                  &root_items, &root_blocks, error, error_size) != 0 ||
        collect_roots(&root_items, &roots, error, error_size) != 0)
        goto done;
    const RootRecord *extent_record = find_root(&roots, BTRFS_EXTENT_TREE_OBJECTID);
    if (extent_record == NULL) {
        set_error(error, error_size, "Btrfs root tree does not contain the extent tree");
        goto done;
    }
    if (tree_walk(&reader, &chunks, devid, node_size, extent_record->bytenr,
                  extent_record->level, &extent_items, &extent_blocks,
                  error, error_size) != 0)
        goto done;

    for (size_t i = 0U; i < extent_items.count; ++i) {
        const TreeItem *item = &extent_items.items[i];
        uint64_t length = 0U;
        if (item->key.type == BTRFS_EXTENT_ITEM)
            length = item->key.offset;
        else if (item->key.type == BTRFS_METADATA_ITEM)
            length = node_size;
        if (item->key.objectid != 0U && length != 0U &&
            mapper_ranges(&chunks, devid, total_bytes, item->key.objectid, length,
                          &used, error, error_size) != 0)
            goto done;
    }

    const uint64_t mirrors[] = {
        64ULL * 1024ULL,
        64ULL * 1024ULL * 1024ULL,
        256ULL * 1024ULL * 1024ULL * 1024ULL,
    };
    for (size_t i = 0U; i < sizeof(mirrors) / sizeof(mirrors[0]); ++i) {
        if (mirrors[i] <= total_bytes && BTRFS_SUPER_SIZE <= total_bytes - mirrors[i] &&
            range_push(&used, mirrors[i], mirrors[i] + BTRFS_SUPER_SIZE,
                       error, error_size) != 0)
            goto done;
    }
    range_merge(&used);

    if (scan_filesystems(&reader, &chunks, devid, node_size, &roots,
                         analysis, &fragmented, error, error_size) != 0)
        goto done;
    range_merge(&fragmented);

    analysis->chunk_count = chunks.count;
    analysis->chunk_tree_blocks = chunk_blocks.count;
    analysis->root_tree_blocks = root_blocks.count;
    analysis->extent_tree_blocks = extent_blocks.count;
    analysis->used_ranges = used.items;
    analysis->used_range_count = used.count;
    used.items = NULL;
    used.count = used.capacity = 0U;
    analysis->fragmented_ranges = fragmented.items;
    analysis->fragmented_range_count = fragmented.count;
    fragmented.items = NULL;
    fragmented.count = fragmented.capacity = 0U;
    result = 0;

done:
    if (result != 0)
        btrfs_analysis_free(analysis);
    free(used.items);
    free(fragmented.items);
    free(roots.items);
    item_vec_free(&extent_items);
    free(extent_blocks.items);
    item_vec_free(&root_items);
    free(root_blocks.items);
    item_vec_free(&chunk_items);
    free(chunk_blocks.items);
    chunk_vec_free(&chunks);
    if (reader.fd >= 0)
        close_reader(&reader);
    return result;
}


/* ------------------------------------------------------------------------- */
/* Bounded offline writer                                                    */
/* ------------------------------------------------------------------------- */

#define BTRFS_WRITER_STOPPED 130
#define BTRFS_ROOT_TREE_OBJECTID 1ULL
#define BTRFS_CHUNK_TREE_OBJECTID 3ULL
#define BTRFS_DEV_TREE_OBJECTID 4ULL
#define BTRFS_CSUM_TREE_OBJECTID 7ULL
#define BTRFS_QUOTA_TREE_OBJECTID 8ULL
#define BTRFS_BLOCK_GROUP_DATA (1ULL << 0U)
#define BTRFS_BLOCK_GROUP_SYSTEM (1ULL << 1U)
#define BTRFS_BLOCK_GROUP_METADATA (1ULL << 2U)
#define BTRFS_BLOCK_GROUP_PROFILE_MASK UINT64_C(0x7f8)
#define BTRFS_INODE_NODATASUM (1ULL << 0U)
#define BTRFS_EXTENT_FLAG_DATA (1ULL << 0U)
#define BTRFS_EXTENT_FLAG_TREE_BLOCK (1ULL << 1U)
#define BTRFS_TREE_BLOCK_REF_KEY 176U
#define BTRFS_EXTENT_DATA_REF_KEY 178U
#define BTRFS_BLOCK_GROUP_ITEM 192U
#define BTRFS_DEV_REPLACE_KEY 250U
#define BTRFS_EXTENT_CSUM_KEY 128U
#define BTRFS_COMPAT_RO_FREE_SPACE_TREE (1ULL << 0U)
#define BTRFS_COMPAT_RO_FREE_SPACE_TREE_VALID (1ULL << 1U)
#define BTRFS_COMPAT_RO_ALLOWED \
    (BTRFS_COMPAT_RO_FREE_SPACE_TREE | BTRFS_COMPAT_RO_FREE_SPACE_TREE_VALID)
#define BTRFS_INCOMPAT_MIXED_BACKREF (1ULL << 0U)
#define BTRFS_INCOMPAT_DEFAULT_SUBVOL (1ULL << 1U)
#define BTRFS_INCOMPAT_MIXED_GROUPS (1ULL << 2U)
#define BTRFS_INCOMPAT_EXTENDED_IREF (1ULL << 6U)
#define BTRFS_INCOMPAT_SKINNY_METADATA (1ULL << 8U)
#define BTRFS_INCOMPAT_NO_HOLES (1ULL << 9U)
#define BTRFS_INCOMPAT_WRITER_ALLOWED \
    (BTRFS_INCOMPAT_MIXED_BACKREF | BTRFS_INCOMPAT_DEFAULT_SUBVOL | \
     BTRFS_INCOMPAT_MIXED_GROUPS | BTRFS_INCOMPAT_EXTENDED_IREF | \
     BTRFS_INCOMPAT_SKINNY_METADATA | BTRFS_INCOMPAT_NO_HOLES)
#define BTRFS_SUPER_COMPAT_RO_OFFSET 180U
#define BTRFS_SUPER_INCOMPAT_OFFSET 188U
#define BTRFS_SUPER_CSUM_TYPE_OFFSET 196U
#define BTRFS_SUPER_CACHE_GENERATION_OFFSET 555U
#define BTRFS_SUPER_BACKUP_ROOTS_OFFSET 2859U
#define BTRFS_SUPER_BACKUP_ROOTS_BYTES 672U
#define BTRFS_WRITER_IO_BYTES (1024U * 1024U)

typedef struct {
    uint8_t header[BTRFS_HEADER_SIZE];
    ItemVec items;
    uint64_t logical;
    uint64_t physical;
    uint64_t owner;
} WriterLeaf;

typedef struct {
    uint64_t inode;
    uint64_t file_offset;
    uint64_t old_bytenr;
    uint64_t new_bytenr;
    uint64_t length;
    size_t fs_item_index;
    size_t extent_item_index;
} WriterMove;

typedef struct {
    WriterMove *items;
    size_t count;
    size_t capacity;
} WriterMoveVec;

typedef struct {
    uint64_t inode;
    size_t first_move;
    size_t move_count;
    uint64_t total_bytes;
} WriterFile;

typedef struct {
    WriterFile *items;
    size_t count;
    size_t capacity;
} WriterFileVec;

typedef struct {
    Reader reader;
    uint8_t superblock[BTRFS_SUPER_SIZE];
    uint64_t generation;
    uint64_t total_bytes;
    uint64_t compat_ro_flags;
    uint64_t incompat_flags;
    uint64_t devid;
    uint32_t sector_size;
    uint32_t node_size;
    ChunkVec chunks;
    WriterLeaf chunk_leaf;
    WriterLeaf root_leaf;
    WriterLeaf extent_leaf;
    WriterLeaf fs_leaf;
    WriterLeaf dev_leaf;
    WriterLeaf csum_leaf;
    bool have_dev_leaf;
    bool have_csum_leaf;
    const Chunk *mixed_chunk;
    RootVec roots;
    WriterMoveVec moves;
    WriterFileVec files;
} WriterModel;

static uint32_t writer_crc32c(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0U; index < length; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)(0U - (crc & 1U));
            crc = (crc >> 1U) ^ (UINT32_C(0x82f63b78) & mask);
        }
    }
    return ~crc;
}

static bool writer_checksum_valid(const uint8_t *block, size_t length)
{
    if (length < 32U)
        return false;
    return infiltratr_load_le32(block) ==
           writer_crc32c(block + 32U, length - 32U);
}

static void writer_checksum_store(uint8_t *block, size_t length)
{
    memset(block, 0, 32U);
    infiltratr_store_le32(block,
                          writer_crc32c(block + 32U, length - 32U));
}

static int writer_key_compare_value(const Key *left, const Key *right)
{
    if (left->objectid < right->objectid) return -1;
    if (left->objectid > right->objectid) return 1;
    if (left->type < right->type) return -1;
    if (left->type > right->type) return 1;
    if (left->offset < right->offset) return -1;
    if (left->offset > right->offset) return 1;
    return 0;
}

static int writer_item_compare(const void *left, const void *right)
{
    const TreeItem *a = left;
    const TreeItem *b = right;
    return writer_key_compare_value(&a->key, &b->key);
}

static void writer_leaf_free(WriterLeaf *leaf)
{
    item_vec_free(&leaf->items);
    memset(leaf, 0, sizeof(*leaf));
}

static void writer_model_free(WriterModel *model)
{
    writer_leaf_free(&model->chunk_leaf);
    writer_leaf_free(&model->root_leaf);
    writer_leaf_free(&model->extent_leaf);
    writer_leaf_free(&model->fs_leaf);
    writer_leaf_free(&model->dev_leaf);
    writer_leaf_free(&model->csum_leaf);
    free(model->roots.items);
    free(model->moves.items);
    free(model->files.items);
    chunk_vec_free(&model->chunks);
    if (model->reader.fd >= 0)
        close_reader(&model->reader);
    memset(model, 0, sizeof(*model));
    model->reader.fd = -1;
}

static int writer_leaf_load(const Reader *reader, const ChunkVec *chunks,
                            uint64_t devid, uint32_t node_size,
                            uint64_t logical, uint64_t expected_owner,
                            const uint8_t superblock[BTRFS_SUPER_SIZE],
                            WriterLeaf *leaf, char *error, size_t error_size)
{
    memset(leaf, 0, sizeof(*leaf));
    uint64_t physical = 0U;
    if (mapper_read_physical(chunks, devid, logical, node_size, &physical,
                             error, error_size) != 0)
        return -1;
    uint8_t *raw = malloc(node_size);
    if (raw == NULL) {
        set_error(error, error_size, "out of memory loading Btrfs writer leaf");
        return -1;
    }
    int result = -1;
    if (read_exact(reader, physical, raw, node_size, error, error_size) != 0)
        goto done;
    if (!writer_checksum_valid(raw, node_size)) {
        set_error(error, error_size,
                  "Btrfs writer refuses a tree block with an invalid CRC32C checksum");
        goto done;
    }
    if (memcmp(raw + 32U, superblock + 32U, 16U) != 0 ||
        infiltratr_load_le64(raw + 48U) != logical ||
        raw[100U] != 0U ||
        infiltratr_load_le64(raw + 88U) != expected_owner) {
        set_error(error, error_size,
                  "Btrfs writer requires checksum-valid level-0 tree roots with matching FSID/owner");
        goto done;
    }
    const uint32_t nritems = infiltratr_load_le32(raw + 96U);
    if ((uint64_t)BTRFS_HEADER_SIZE +
            (uint64_t)nritems * (uint64_t)BTRFS_ITEM_SIZE > node_size) {
        set_error(error, error_size, "invalid Btrfs writer leaf item count");
        goto done;
    }
    memcpy(leaf->header, raw, BTRFS_HEADER_SIZE);
    leaf->logical = logical;
    leaf->physical = physical;
    leaf->owner = expected_owner;
    Key previous = {0};
    bool have_previous = false;
    const size_t table_end =
        BTRFS_HEADER_SIZE + (size_t)nritems * BTRFS_ITEM_SIZE;
    for (uint32_t index = 0U; index < nritems; ++index) {
        const size_t pos =
            BTRFS_HEADER_SIZE + (size_t)index * BTRFS_ITEM_SIZE;
        const Key key = parse_key(raw + pos);
        const uint32_t relative = infiltratr_load_le32(raw + pos + 17U);
        const uint32_t data_size = infiltratr_load_le32(raw + pos + 21U);
        if (have_previous && writer_key_compare_value(&previous, &key) >= 0) {
            set_error(error, error_size,
                      "Btrfs writer requires strictly ordered leaf keys");
            goto done;
        }
        if ((uint64_t)BTRFS_HEADER_SIZE + relative > node_size) {
            set_error(error, error_size, "Btrfs writer leaf item lies outside block");
            goto done;
        }
        const size_t data_offset =
            BTRFS_HEADER_SIZE + (size_t)relative;
        if (data_offset < table_end || data_size > node_size - data_offset) {
            set_error(error, error_size,
                      "Btrfs writer leaf item overlaps its item table");
            goto done;
        }
        if (item_push(&leaf->items, key, raw + data_offset, data_size,
                      error, error_size) != 0)
            goto done;
        previous = key;
        have_previous = true;
    }
    result = 0;
done:
    free(raw);
    if (result != 0)
        writer_leaf_free(leaf);
    return result;
}

static int writer_leaf_write(const WriterModel *model, int fd,
                             WriterLeaf *leaf, uint64_t generation,
                             char *error, size_t error_size)
{
    if (leaf->items.count > UINT32_MAX) {
        set_error(error, error_size, "Btrfs writer leaf contains too many items");
        return -1;
    }
    if (leaf->items.count > 1U)
        qsort(leaf->items.items, leaf->items.count,
              sizeof(*leaf->items.items), writer_item_compare);
    uint8_t *raw = calloc(model->node_size, 1U);
    if (raw == NULL) {
        set_error(error, error_size, "out of memory rebuilding Btrfs writer leaf");
        return -1;
    }
    memcpy(raw, leaf->header, BTRFS_HEADER_SIZE);
    infiltratr_store_le64(raw + 48U, leaf->logical);
    infiltratr_store_le64(raw + 80U, generation);
    infiltratr_store_le64(raw + 88U, leaf->owner);
    infiltratr_store_le32(raw + 96U, (uint32_t)leaf->items.count);
    raw[100U] = 0U;
    const size_t table_end =
        BTRFS_HEADER_SIZE + leaf->items.count * BTRFS_ITEM_SIZE;
    size_t cursor = model->node_size;
    for (size_t index = 0U; index < leaf->items.count; ++index) {
        const TreeItem *item = &leaf->items.items[index];
        if ((size_t)item->size > cursor ||
            cursor - (size_t)item->size < table_end) {
            free(raw);
            set_error(error, error_size,
                      "Btrfs writer leaf no longer fits its fixed tree block");
            return -1;
        }
        cursor -= (size_t)item->size;
        if (item->size != 0U)
            memcpy(raw + cursor, item->data, item->size);
        const size_t pos =
            BTRFS_HEADER_SIZE + index * BTRFS_ITEM_SIZE;
        infiltratr_store_le64(raw + pos, item->key.objectid);
        raw[pos + 8U] = item->key.type;
        infiltratr_store_le64(raw + pos + 9U, item->key.offset);
        infiltratr_store_le32(raw + pos + 17U,
                              (uint32_t)(cursor - BTRFS_HEADER_SIZE));
        infiltratr_store_le32(raw + pos + 21U, item->size);
    }
    writer_checksum_store(raw, model->node_size);
    const int rc =
        infiltratr_pwrite_full(fd, raw, model->node_size, leaf->physical);
    free(raw);
    if (rc != 0) {
        set_error(error, error_size, "cannot write rebuilt Btrfs tree leaf");
        return -1;
    }
    return 0;
}

static int writer_move_push(WriterMoveVec *moves, WriterMove move,
                            char *error, size_t error_size)
{
    if (grow_array((void **)&moves->items, &moves->capacity, moves->count,
                   sizeof(*moves->items), error, error_size) != 0)
        return -1;
    moves->items[moves->count++] = move;
    return 0;
}

static int writer_file_push(WriterFileVec *files, WriterFile file,
                            char *error, size_t error_size)
{
    if (grow_array((void **)&files->items, &files->capacity, files->count,
                   sizeof(*files->items), error, error_size) != 0)
        return -1;
    files->items[files->count++] = file;
    return 0;
}

static ssize_t writer_find_extent_item(const WriterLeaf *extent_leaf,
                                       uint64_t bytenr, uint64_t length)
{
    for (size_t index = 0U; index < extent_leaf->items.count; ++index) {
        const TreeItem *item = &extent_leaf->items.items[index];
        if (item->key.objectid == bytenr &&
            item->key.type == BTRFS_EXTENT_ITEM &&
            item->key.offset == length)
            return (ssize_t)index;
    }
    return -1;
}

static int writer_validate_data_extent(const TreeItem *extent_item,
                                       uint64_t inode, char *error,
                                       size_t error_size)
{
    if (extent_item->size != 53U ||
        infiltratr_load_le64(extent_item->data) != 1U ||
        infiltratr_load_le64(extent_item->data + 16U) !=
            BTRFS_EXTENT_FLAG_DATA ||
        extent_item->data[24U] != BTRFS_EXTENT_DATA_REF_KEY ||
        infiltratr_load_le64(extent_item->data + 25U) !=
            BTRFS_FS_TREE_OBJECTID ||
        infiltratr_load_le64(extent_item->data + 33U) != inode ||
        infiltratr_load_le32(extent_item->data + 49U) != 1U) {
        set_error(error, error_size,
                  "Btrfs writer requires unshared data extents with one inline data back-reference");
        return -1;
    }
    return 0;
}

static int writer_validate_metadata_extent(const WriterModel *model,
                                           uint64_t bytenr, uint64_t owner,
                                           char *error, size_t error_size)
{
    for (size_t index = 0U; index < model->extent_leaf.items.count; ++index) {
        const TreeItem *item = &model->extent_leaf.items.items[index];
        if (item->key.objectid != bytenr ||
            item->key.type != BTRFS_METADATA_ITEM ||
            item->key.offset != 0U)
            continue;
        if (item->size != 33U ||
            infiltratr_load_le64(item->data) != 1U ||
            infiltratr_load_le64(item->data + 16U) !=
                BTRFS_EXTENT_FLAG_TREE_BLOCK ||
            item->data[24U] != BTRFS_TREE_BLOCK_REF_KEY ||
            infiltratr_load_le64(item->data + 25U) != owner) {
            set_error(error, error_size,
                      "Btrfs writer requires one-owner skinny metadata extents");
            return -1;
        }
        return 0;
    }
    set_error(error, error_size,
              "Btrfs writer could not find a required metadata extent item");
    return -1;
}

static int writer_scan_files(WriterModel *model, char *error,
                             size_t error_size)
{
    uint64_t current = UINT64_MAX;
    uint32_t mode = 0U;
    uint64_t inode_flags = 0U;
    bool have_inode = false;
    size_t current_file = SIZE_MAX;
    uint64_t expected_offset = 0U;

    for (size_t index = 0U; index < model->fs_leaf.items.count; ++index) {
        TreeItem *item = &model->fs_leaf.items.items[index];
        if (item->key.objectid != current) {
            current = item->key.objectid;
            mode = 0U;
            inode_flags = 0U;
            have_inode = false;
            current_file = SIZE_MAX;
            expected_offset = 0U;
        }
        if (item->key.type == BTRFS_INODE_ITEM) {
            if (have_inode || item->size < 72U) {
                set_error(error, error_size,
                          "Btrfs writer requires one complete inode item per object");
                return -1;
            }
            mode = infiltratr_load_le32(item->data + 52U);
            inode_flags = infiltratr_load_le64(item->data + 64U);
            have_inode = true;
            continue;
        }
        if (item->key.type != BTRFS_EXTENT_DATA)
            continue;
        if (!have_inode || (mode & (uint32_t)S_IFMT) != (uint32_t)S_IFREG) {
            set_error(error, error_size,
                      "Btrfs writer found extent data without a regular-file inode");
            return -1;
        }
        if (item->size < 21U) {
            set_error(error, error_size, "truncated Btrfs file extent item");
            return -1;
        }
        if (item->data[20U] == BTRFS_FILE_EXTENT_INLINE)
            continue;
        if ((inode_flags & BTRFS_INODE_NODATASUM) == 0U) {
            set_error(error, error_size,
                      "Btrfs writer supports external extents only on NODATASUM files");
            return -1;
        }
        if (item->data[20U] != BTRFS_FILE_EXTENT_REG || item->size < 53U ||
            item->data[16U] != 0U || item->data[17U] != 0U ||
            infiltratr_load_le16(item->data + 18U) != 0U) {
            set_error(error, error_size,
                      "Btrfs writer supports only unencoded regular file extents");
            return -1;
        }
        const uint64_t disk_bytenr = infiltratr_load_le64(item->data + 21U);
        const uint64_t disk_bytes = infiltratr_load_le64(item->data + 29U);
        const uint64_t extent_offset = infiltratr_load_le64(item->data + 37U);
        const uint64_t num_bytes = infiltratr_load_le64(item->data + 45U);
        if (disk_bytenr == 0U || disk_bytes == 0U || num_bytes == 0U ||
            disk_bytes != num_bytes || extent_offset != 0U ||
            disk_bytenr % model->sector_size != 0U ||
            num_bytes % model->sector_size != 0U ||
            item->key.offset != expected_offset) {
            set_error(error, error_size,
                      "Btrfs writer requires sector-aligned full extents for a non-sparse file");
            return -1;
        }
        const Chunk *chunk = mapper_find(&model->chunks, disk_bytenr);
        if (chunk == NULL || chunk != model->mixed_chunk ||
            num_bytes > chunk->length - (disk_bytenr - chunk->logical)) {
            set_error(error, error_size,
                      "Btrfs writer requires file extents inside the supported mixed block group");
            return -1;
        }
        const ssize_t extent_index =
            writer_find_extent_item(&model->extent_leaf, disk_bytenr,
                                    disk_bytes);
        if (extent_index < 0 ||
            writer_validate_data_extent(
                &model->extent_leaf.items.items[(size_t)extent_index],
                current, error, error_size) != 0)
            return -1;

        if (current_file == SIZE_MAX) {
            WriterFile file = {
                current, model->moves.count, 0U, 0U
            };
            if (writer_file_push(&model->files, file,
                                 error, error_size) != 0)
                return -1;
            current_file = model->files.count - 1U;
        }
        WriterMove move = {
            current, item->key.offset, disk_bytenr, 0U, num_bytes,
            index, (size_t)extent_index
        };
        if (writer_move_push(&model->moves, move,
                             error, error_size) != 0)
            return -1;
        model->files.items[current_file].move_count++;
        if (model->files.items[current_file].total_bytes >
            UINT64_MAX - num_bytes) {
            set_error(error, error_size, "Btrfs file length overflows");
            return -1;
        }
        model->files.items[current_file].total_bytes += num_bytes;
        expected_offset += num_bytes;
    }
    return 0;
}

static bool writer_ranges_overlap(uint64_t a_start, uint64_t a_end,
                                  uint64_t b_start, uint64_t b_end)
{
    return a_start < b_end && b_start < a_end;
}

static int writer_validate_csum_tree(const WriterModel *model,
                                     char *error, size_t error_size)
{
    if (!model->have_csum_leaf)
        return 0;
    for (size_t index = 0U; index < model->csum_leaf.items.count; ++index) {
        const TreeItem *item = &model->csum_leaf.items.items[index];
        if (item->key.type != BTRFS_EXTENT_CSUM_KEY)
            continue;
        if (item->size % 4U != 0U) {
            set_error(error, error_size,
                      "Btrfs CRC32 checksum item has an invalid size");
            return -1;
        }
        const uint64_t sectors = item->size / 4U;
        if (sectors > UINT64_MAX / model->sector_size) {
            set_error(error, error_size, "Btrfs checksum range overflows");
            return -1;
        }
        const uint64_t length = sectors * model->sector_size;
        if (item->key.offset > UINT64_MAX - length) {
            set_error(error, error_size, "Btrfs checksum range overflows");
            return -1;
        }
        const uint64_t end = item->key.offset + length;
        for (size_t move = 0U; move < model->moves.count; ++move) {
            const WriterMove *entry = &model->moves.items[move];
            if (writer_ranges_overlap(item->key.offset, end,
                                      entry->old_bytenr,
                                      entry->old_bytenr + entry->length)) {
                set_error(error, error_size,
                          "Btrfs writer refuses a movable extent that still has a checksum-tree entry");
                return -1;
            }
        }
    }
    return 0;
}

static const Chunk *writer_find_mixed_chunk(const ChunkVec *chunks,
                                            uint64_t logical)
{
    const Chunk *chunk = mapper_find(chunks, logical);
    if (chunk == NULL)
        return NULL;
    const uint64_t required =
        BTRFS_BLOCK_GROUP_DATA | BTRFS_BLOCK_GROUP_METADATA;
    if ((chunk->chunk_type & required) != required ||
        (chunk->chunk_type & BTRFS_BLOCK_GROUP_PROFILE_MASK) != 0U ||
        chunk->stripe_count != 1U)
        return NULL;
    return chunk;
}

static int writer_model_load(const char *path, WriterModel *model,
                             char *error, size_t error_size)
{
    memset(model, 0, sizeof(*model));
    model->reader.fd = -1;
    if (open_reader(path, &model->reader, error, error_size) != 0)
        goto fail;
    if (model->reader.size < BTRFS_SUPER_OFFSET + BTRFS_SUPER_SIZE ||
        read_exact(&model->reader, BTRFS_SUPER_OFFSET, model->superblock,
                   sizeof(model->superblock), error, error_size) != 0)
        goto fail;
    if (memcmp(model->superblock + 0x40U, "_BHRfS_M", 8U) != 0 ||
        !writer_checksum_valid(model->superblock, BTRFS_SUPER_SIZE)) {
        set_error(error, error_size,
                  "Btrfs writer requires a checksum-valid primary superblock");
        goto fail;
    }
    model->generation = infiltratr_load_le64(model->superblock + 72U);
    const uint64_t root = infiltratr_load_le64(model->superblock + 80U);
    const uint64_t chunk_root =
        infiltratr_load_le64(model->superblock + 88U);
    const uint64_t log_root =
        infiltratr_load_le64(model->superblock + 96U);
    model->total_bytes =
        infiltratr_load_le64(model->superblock + 112U);
    const uint64_t num_devices =
        infiltratr_load_le64(model->superblock + 136U);
    model->sector_size =
        infiltratr_load_le32(model->superblock + 144U);
    model->node_size =
        infiltratr_load_le32(model->superblock + 148U);
    model->compat_ro_flags =
        infiltratr_load_le64(model->superblock +
                             BTRFS_SUPER_COMPAT_RO_OFFSET);
    model->incompat_flags =
        infiltratr_load_le64(model->superblock +
                             BTRFS_SUPER_INCOMPAT_OFFSET);
    const uint16_t csum_type =
        infiltratr_load_le16(model->superblock +
                             BTRFS_SUPER_CSUM_TYPE_OFFSET);
    const uint8_t root_level = model->superblock[198U];
    const uint8_t chunk_level = model->superblock[199U];
    model->devid = infiltratr_load_le64(model->superblock + 201U);

    if (num_devices != 1U || log_root != 0U ||
        root_level != 0U || chunk_level != 0U ||
        csum_type != 0U ||
        model->total_bytes == 0U ||
        model->total_bytes > model->reader.size ||
        model->generation == UINT64_MAX ||
        (model->compat_ro_flags & ~BTRFS_COMPAT_RO_ALLOWED) != 0U ||
        (model->incompat_flags & ~BTRFS_INCOMPAT_WRITER_ALLOWED) != 0U ||
        (model->incompat_flags & BTRFS_INCOMPAT_MIXED_GROUPS) == 0U ||
        (model->incompat_flags & BTRFS_INCOMPAT_SKINNY_METADATA) == 0U) {
        set_error(error, error_size,
                  "Btrfs writer supports only CRC32C, clean-log, single-device, level-0 mixed-group/skinny-metadata filesystems without unsupported feature bits");
        goto fail;
    }

    if (parse_system_chunks(model->superblock, &model->chunks,
                            error, error_size) != 0 ||
        mapper_prepare(&model->chunks, model->devid, model->total_bytes,
                       error, error_size) != 0 ||
        writer_leaf_load(&model->reader, &model->chunks, model->devid,
                         model->node_size, chunk_root,
                         BTRFS_CHUNK_TREE_OBJECTID, model->superblock,
                         &model->chunk_leaf, error, error_size) != 0)
        goto fail;
    for (size_t index = 0U; index < model->chunk_leaf.items.count; ++index) {
        TreeItem *item = &model->chunk_leaf.items.items[index];
        if (item->key.type != BTRFS_CHUNK_ITEM)
            continue;
        Chunk chunk = {0};
        if (parse_chunk(item->data, item->size, item->key.offset,
                        &chunk, error, error_size) != 0)
            goto fail;
        if (chunk_add_unique(&model->chunks, &chunk,
                             error, error_size) != 0) {
            chunk_free(&chunk);
            goto fail;
        }
    }
    if (mapper_prepare(&model->chunks, model->devid,
                       model->total_bytes, error, error_size) != 0 ||
        writer_leaf_load(&model->reader, &model->chunks, model->devid,
                         model->node_size, root,
                         BTRFS_ROOT_TREE_OBJECTID, model->superblock,
                         &model->root_leaf, error, error_size) != 0 ||
        collect_roots(&model->root_leaf.items, &model->roots,
                      error, error_size) != 0)
        goto fail;

    for (size_t index = 0U; index < model->roots.count; ++index) {
        const RootRecord *record = &model->roots.items[index];
        if (record->refs == 0U)
            continue;
        if (record->objectid >= BTRFS_FIRST_FREE_OBJECTID ||
            record->objectid == BTRFS_QUOTA_TREE_OBJECTID) {
            set_error(error, error_size,
                      "Btrfs writer refuses snapshots/subvolumes and qgroup trees in the bounded subset");
            goto fail;
        }
    }
    for (size_t index = 0U; index < model->root_leaf.items.count; ++index) {
        if (model->root_leaf.items.items[index].key.type == 248U) {
            set_error(error, error_size,
                      "Btrfs writer refuses a filesystem with a persisted balance/temporary root-tree item");
            goto fail;
        }
    }

    const RootRecord *extent_root =
        find_root(&model->roots, BTRFS_EXTENT_TREE_OBJECTID);
    const RootRecord *fs_root =
        find_root(&model->roots, BTRFS_FS_TREE_OBJECTID);
    const RootRecord *dev_root =
        find_root(&model->roots, BTRFS_DEV_TREE_OBJECTID);
    const RootRecord *csum_root =
        find_root(&model->roots, BTRFS_CSUM_TREE_OBJECTID);
    if (extent_root == NULL || fs_root == NULL ||
        dev_root == NULL || csum_root == NULL ||
        extent_root->level != 0U || fs_root->level != 0U ||
        dev_root->level != 0U || csum_root->level != 0U) {
        set_error(error, error_size,
                  "Btrfs writer requires level-0 extent, filesystem, device and checksum roots");
        goto fail;
    }

    model->mixed_chunk =
        writer_find_mixed_chunk(&model->chunks, root);
    if (model->mixed_chunk == NULL ||
        mapper_find(&model->chunks, extent_root->bytenr) !=
            model->mixed_chunk ||
        mapper_find(&model->chunks, fs_root->bytenr) !=
            model->mixed_chunk ||
        mapper_find(&model->chunks, dev_root->bytenr) !=
            model->mixed_chunk ||
        mapper_find(&model->chunks, csum_root->bytenr) !=
            model->mixed_chunk) {
        set_error(error, error_size,
                  "Btrfs writer requires the mutable roots in one unprofiled mixed data/metadata chunk");
        goto fail;
    }

    if (writer_leaf_load(&model->reader, &model->chunks, model->devid,
                         model->node_size, extent_root->bytenr,
                         BTRFS_EXTENT_TREE_OBJECTID, model->superblock,
                         &model->extent_leaf, error, error_size) != 0 ||
        writer_leaf_load(&model->reader, &model->chunks, model->devid,
                         model->node_size, fs_root->bytenr,
                         BTRFS_FS_TREE_OBJECTID, model->superblock,
                         &model->fs_leaf, error, error_size) != 0 ||
        writer_leaf_load(&model->reader, &model->chunks, model->devid,
                         model->node_size, dev_root->bytenr,
                         BTRFS_DEV_TREE_OBJECTID, model->superblock,
                         &model->dev_leaf, error, error_size) != 0 ||
        writer_leaf_load(&model->reader, &model->chunks, model->devid,
                         model->node_size, csum_root->bytenr,
                         BTRFS_CSUM_TREE_OBJECTID, model->superblock,
                         &model->csum_leaf, error, error_size) != 0)
        goto fail;
    model->have_dev_leaf = true;
    model->have_csum_leaf = true;

    for (size_t index = 0U; index < model->dev_leaf.items.count; ++index) {
        if (model->dev_leaf.items.items[index].key.type ==
            BTRFS_DEV_REPLACE_KEY) {
            set_error(error, error_size,
                      "Btrfs writer refuses a filesystem with device replacement state");
            goto fail;
        }
    }

    const uint64_t tree_bytenrs[] = {
        chunk_root, root, extent_root->bytenr, fs_root->bytenr,
        dev_root->bytenr, csum_root->bytenr
    };
    const uint64_t tree_owners[] = {
        BTRFS_CHUNK_TREE_OBJECTID, BTRFS_ROOT_TREE_OBJECTID,
        BTRFS_EXTENT_TREE_OBJECTID, BTRFS_FS_TREE_OBJECTID,
        BTRFS_DEV_TREE_OBJECTID, BTRFS_CSUM_TREE_OBJECTID
    };
    for (size_t index = 0U;
         index < sizeof(tree_bytenrs) / sizeof(tree_bytenrs[0]); ++index) {
        if (writer_validate_metadata_extent(
                model, tree_bytenrs[index], tree_owners[index],
                error, error_size) != 0)
            goto fail;
    }

    if (writer_scan_files(model, error, error_size) != 0 ||
        writer_validate_csum_tree(model, error, error_size) != 0)
        goto fail;
    return 0;

fail:
    writer_model_free(model);
    return -1;
}

static bool writer_move_matches(const WriterModel *model,
                                uint64_t bytenr, uint64_t length)
{
    for (size_t index = 0U; index < model->moves.count; ++index) {
        if (model->moves.items[index].old_bytenr == bytenr &&
            model->moves.items[index].length == length)
            return true;
    }
    return false;
}

static int writer_build_claimed(const WriterModel *model, RangeVec *claimed,
                                char *error, size_t error_size)
{
    for (size_t index = 0U; index < model->extent_leaf.items.count; ++index) {
        const TreeItem *item = &model->extent_leaf.items.items[index];
        uint64_t length = 0U;
        if (item->key.type == BTRFS_EXTENT_ITEM) {
            length = item->key.offset;
            if (writer_move_matches(model, item->key.objectid, length))
                continue;
        } else if (item->key.type == BTRFS_METADATA_ITEM) {
            length = model->node_size;
        } else {
            continue;
        }
        if (item->key.objectid > UINT64_MAX - length ||
            range_push(claimed, item->key.objectid,
                       item->key.objectid + length,
                       error, error_size) != 0)
            return -1;
    }
    range_merge(claimed);
    return 0;
}

static uint64_t writer_align_up(uint64_t value, uint32_t alignment)
{
    const uint64_t remainder = value % alignment;
    if (remainder == 0U)
        return value;
    return value + ((uint64_t)alignment - remainder);
}

static bool writer_physical_hits_super(const WriterModel *model,
                                       uint64_t logical, uint64_t length)
{
    RangeVec physical = {0};
    char error[128] = {0};
    if (mapper_ranges(&model->chunks, model->devid, model->total_bytes,
                      logical, length, &physical,
                      error, sizeof(error)) != 0) {
        free(physical.items);
        return true;
    }
    const uint64_t mirrors[] = {
        64ULL * 1024ULL,
        64ULL * 1024ULL * 1024ULL,
        256ULL * 1024ULL * 1024ULL * 1024ULL,
    };
    bool hit = false;
    for (size_t range = 0U; range < physical.count && !hit; ++range) {
        for (size_t mirror = 0U;
             mirror < sizeof(mirrors) / sizeof(mirrors[0]); ++mirror) {
            if (mirrors[mirror] > model->total_bytes ||
                BTRFS_SUPER_SIZE > model->total_bytes - mirrors[mirror])
                continue;
            if (writer_ranges_overlap(
                    physical.items[range].start, physical.items[range].end,
                    mirrors[mirror], mirrors[mirror] + BTRFS_SUPER_SIZE)) {
                hit = true;
                break;
            }
        }
    }
    free(physical.items);
    return hit;
}

static int writer_choose_run(const WriterModel *model, RangeVec *claimed,
                             uint64_t data_bytes, uint64_t reserve_bytes,
                             uint64_t *destination,
                             char *error, size_t error_size)
{
    if (data_bytes == 0U ||
        data_bytes > UINT64_MAX - reserve_bytes) {
        set_error(error, error_size, "invalid Btrfs placement span");
        return -1;
    }
    const uint64_t span = data_bytes + reserve_bytes;
    const uint64_t chunk_end =
        model->mixed_chunk->logical + model->mixed_chunk->length;
    uint64_t candidate =
        writer_align_up(model->mixed_chunk->logical, model->sector_size);
    while (candidate <= chunk_end &&
           span <= chunk_end - candidate) {
        bool overlapped = false;
        uint64_t advance = candidate + model->sector_size;
        for (size_t index = 0U; index < claimed->count; ++index) {
            const BtrfsRange range = claimed->items[index];
            if (range.end <= candidate)
                continue;
            if (range.start >= candidate + span)
                break;
            overlapped = true;
            advance = writer_align_up(range.end, model->sector_size);
            break;
        }
        if (overlapped) {
            candidate = advance;
            continue;
        }
        if (writer_physical_hits_super(model, candidate, span)) {
            candidate += model->sector_size;
            continue;
        }
        if (range_push(claimed, candidate, candidate + span,
                       error, error_size) != 0)
            return -1;
        range_merge(claimed);
        *destination = candidate;
        return 0;
    }
    set_error(error, error_size,
              "Btrfs writer cannot place a file contiguously with the requested reserve");
    return -1;
}

static int writer_copy_logical(const WriterModel *model, int stage_fd,
                               uint64_t source_logical,
                               uint64_t target_logical, uint64_t length,
                               char *error, size_t error_size)
{
    uint64_t source_physical = 0U;
    uint64_t target_physical = 0U;
    if (mapper_read_physical(&model->chunks, model->devid,
                             source_logical, length, &source_physical,
                             error, error_size) != 0 ||
        mapper_read_physical(&model->chunks, model->devid,
                             target_logical, length, &target_physical,
                             error, error_size) != 0)
        return -1;
    uint8_t *buffer = malloc(BTRFS_WRITER_IO_BYTES);
    if (buffer == NULL) {
        set_error(error, error_size, "out of memory relocating Btrfs data");
        return -1;
    }
    int result = 0;
    for (uint64_t offset = 0U; offset < length;) {
        if (ld_stop_requested()) {
            result = BTRFS_WRITER_STOPPED;
            break;
        }
        const uint64_t remaining = length - offset;
        const size_t count = remaining > BTRFS_WRITER_IO_BYTES
                           ? BTRFS_WRITER_IO_BYTES : (size_t)remaining;
        if (infiltratr_pread_full(model->reader.fd, buffer, count,
                                  source_physical + offset) != 0 ||
            infiltratr_pwrite_full(stage_fd, buffer, count,
                                  target_physical + offset) != 0) {
            set_error(error, error_size,
                      "short I/O relocating Btrfs file data");
            result = -1;
            break;
        }
        offset += count;
    }
    free(buffer);
    return result;
}

static int writer_copy_prefix(const WriterModel *model, int stage_fd,
                              char *error, size_t error_size)
{
    uint8_t *buffer = malloc(BTRFS_WRITER_IO_BYTES);
    if (buffer == NULL) {
        set_error(error, error_size, "out of memory staging Btrfs filesystem");
        return -1;
    }
    int result = 0;
    for (uint64_t offset = 0U; offset < model->total_bytes;) {
        if (ld_stop_requested()) {
            result = BTRFS_WRITER_STOPPED;
            break;
        }
        const uint64_t remaining = model->total_bytes - offset;
        const size_t count = remaining > BTRFS_WRITER_IO_BYTES
                           ? BTRFS_WRITER_IO_BYTES : (size_t)remaining;
        if (infiltratr_pread_full(model->reader.fd, buffer, count, offset) != 0 ||
            infiltratr_pwrite_full(stage_fd, buffer, count, offset) != 0) {
            set_error(error, error_size,
                      "short I/O creating Btrfs transaction stage");
            result = -1;
            break;
        }
        offset += count;
    }
    free(buffer);
    return result;
}

static TreeItem *writer_find_root_item(WriterLeaf *root_leaf, uint64_t objectid)
{
    for (size_t index = 0U; index < root_leaf->items.count; ++index) {
        TreeItem *item = &root_leaf->items.items[index];
        if (item->key.objectid == objectid &&
            item->key.type == BTRFS_ROOT_ITEM)
            return item;
    }
    return NULL;
}

static int writer_update_root_item(WriterLeaf *root_leaf, uint64_t objectid,
                                   uint64_t generation,
                                   char *error, size_t error_size)
{
    TreeItem *item = writer_find_root_item(root_leaf, objectid);
    if (item == NULL || item->size < 239U) {
        set_error(error, error_size,
                  "Btrfs writer cannot update a required root item");
        return -1;
    }
    infiltratr_store_le64(item->data + 160U, generation);
    if (item->size >= 247U)
        infiltratr_store_le64(item->data + 239U, generation);
    return 0;
}

static int writer_update_metadata_generation(WriterLeaf *extent_leaf,
                                             uint64_t bytenr,
                                             uint64_t generation,
                                             char *error, size_t error_size)
{
    for (size_t index = 0U; index < extent_leaf->items.count; ++index) {
        TreeItem *item = &extent_leaf->items.items[index];
        if (item->key.objectid == bytenr &&
            item->key.type == BTRFS_METADATA_ITEM &&
            item->key.offset == 0U) {
            if (item->size < 24U) {
                set_error(error, error_size,
                          "Btrfs metadata extent item is truncated");
                return -1;
            }
            infiltratr_store_le64(item->data + 8U, generation);
            return 0;
        }
    }
    set_error(error, error_size,
              "Btrfs writer lost a metadata extent item");
    return -1;
}

static int writer_super_write(int stage_fd, WriterModel *model,
                              uint64_t generation,
                              char *error, size_t error_size)
{
    uint8_t updated[BTRFS_SUPER_SIZE];
    memcpy(updated, model->superblock, sizeof(updated));
    infiltratr_store_le64(updated + 72U, generation);
    const uint64_t compat =
        model->compat_ro_flags &
        ~BTRFS_COMPAT_RO_FREE_SPACE_TREE_VALID;
    infiltratr_store_le64(updated + BTRFS_SUPER_COMPAT_RO_OFFSET, compat);
    infiltratr_store_le64(updated + BTRFS_SUPER_CACHE_GENERATION_OFFSET,
                          UINT64_MAX);
    memset(updated + BTRFS_SUPER_BACKUP_ROOTS_OFFSET, 0,
           BTRFS_SUPER_BACKUP_ROOTS_BYTES);

    const uint64_t mirrors[] = {
        64ULL * 1024ULL,
        64ULL * 1024ULL * 1024ULL,
        256ULL * 1024ULL * 1024ULL * 1024ULL,
    };
    for (size_t index = 0U;
         index < sizeof(mirrors) / sizeof(mirrors[0]); ++index) {
        const uint64_t offset = mirrors[index];
        if (offset > model->total_bytes ||
            BTRFS_SUPER_SIZE > model->total_bytes - offset)
            continue;
        if (index != 0U) {
            uint8_t existing[BTRFS_SUPER_SIZE];
            if (infiltratr_pread_full(stage_fd, existing, sizeof(existing),
                                      offset) != 0 ||
                memcmp(existing + 0x40U, "_BHRfS_M", 8U) != 0 ||
                memcmp(existing + 32U, model->superblock + 32U, 16U) != 0)
                continue;
        }
        uint8_t mirror[BTRFS_SUPER_SIZE];
        memcpy(mirror, updated, sizeof(mirror));
        infiltratr_store_le64(mirror + 48U, offset);
        writer_checksum_store(mirror, sizeof(mirror));
        if (infiltratr_pwrite_full(stage_fd, mirror, sizeof(mirror),
                                   offset) != 0) {
            set_error(error, error_size,
                      "cannot publish a Btrfs superblock mirror");
            return -1;
        }
    }
    return 0;
}

static int writer_payload_compare(const WriterModel *before,
                                  const WriterModel *after,
                                  char *error, size_t error_size)
{
    if (before->moves.count != after->moves.count ||
        before->files.count != after->files.count) {
        set_error(error, error_size,
                  "Btrfs staged file extent inventory changed");
        return -1;
    }
    uint8_t *left = malloc(BTRFS_WRITER_IO_BYTES);
    uint8_t *right = malloc(BTRFS_WRITER_IO_BYTES);
    if (left == NULL || right == NULL) {
        free(left);
        free(right);
        set_error(error, error_size,
                  "out of memory verifying Btrfs staged payloads");
        return -1;
    }
    int result = 0;
    for (size_t index = 0U; index < before->moves.count; ++index) {
        const WriterMove *a = &before->moves.items[index];
        const WriterMove *b = &after->moves.items[index];
        if (a->inode != b->inode ||
            a->file_offset != b->file_offset ||
            a->length != b->length) {
            set_error(error, error_size,
                      "Btrfs staged file extent identity changed");
            result = -1;
            break;
        }
        uint64_t pa = 0U, pb = 0U;
        if (mapper_read_physical(&before->chunks, before->devid,
                                 a->old_bytenr, a->length, &pa,
                                 error, error_size) != 0 ||
            mapper_read_physical(&after->chunks, after->devid,
                                 b->old_bytenr, b->length, &pb,
                                 error, error_size) != 0) {
            result = -1;
            break;
        }
        for (uint64_t offset = 0U; offset < a->length;) {
            const uint64_t remaining = a->length - offset;
            const size_t count = remaining > BTRFS_WRITER_IO_BYTES
                               ? BTRFS_WRITER_IO_BYTES : (size_t)remaining;
            if (infiltratr_pread_full(before->reader.fd, left, count,
                                      pa + offset) != 0 ||
                infiltratr_pread_full(after->reader.fd, right, count,
                                      pb + offset) != 0 ||
                memcmp(left, right, count) != 0) {
                set_error(error, error_size,
                          "Btrfs file payload changed during staging");
                result = -1;
                break;
            }
            offset += count;
        }
        if (result != 0)
            break;
    }
    free(left);
    free(right);
    return result;
}

static bool writer_extent_overlaps(const WriterLeaf *extent_leaf,
                                   uint64_t start, uint64_t end,
                                   uint32_t node_size)
{
    for (size_t index = 0U; index < extent_leaf->items.count; ++index) {
        const TreeItem *item = &extent_leaf->items.items[index];
        uint64_t length = 0U;
        if (item->key.type == BTRFS_EXTENT_ITEM)
            length = item->key.offset;
        else if (item->key.type == BTRFS_METADATA_ITEM)
            length = node_size;
        else
            continue;
        if (item->key.objectid > UINT64_MAX - length)
            return true;
        if (writer_ranges_overlap(start, end, item->key.objectid,
                                  item->key.objectid + length))
            return true;
    }
    return false;
}

int btrfs_verify_layout(const char *path, bool growth,
                        unsigned growth_percent,
                        char *error, size_t error_size)
{
    if (growth && growth_percent != 10U) {
        set_error(error, error_size,
                  "Btrfs Growth Defrag requires exactly 10 percent reserve");
        return -1;
    }
    WriterModel model;
    if (writer_model_load(path, &model, error, error_size) != 0)
        return -1;
    int result = 0;
    for (size_t file_index = 0U;
         file_index < model.files.count && result == 0; ++file_index) {
        const WriterFile *file = &model.files.items[file_index];
        if (file->move_count == 0U)
            continue;
        const WriterMove *first =
            &model.moves.items[file->first_move];
        uint64_t expected = first->old_bytenr;
        for (size_t offset = 0U; offset < file->move_count; ++offset) {
            const WriterMove *move =
                &model.moves.items[file->first_move + offset];
            if (move->old_bytenr != expected) {
                set_error(error, error_size,
                          "Btrfs file %" PRIu64 " remains fragmented",
                          file->inode);
                result = -1;
                break;
            }
            expected += move->length;
        }
        if (result != 0 || !growth)
            continue;
        const uint64_t blocks =
            file->total_bytes / model.sector_size;
        const uint64_t reserve_blocks =
            (blocks * growth_percent + 99U) / 100U;
        if (reserve_blocks >
            UINT64_MAX / model.sector_size) {
            set_error(error, error_size,
                      "Btrfs growth reserve overflows");
            result = -1;
            break;
        }
        const uint64_t reserve =
            reserve_blocks * model.sector_size;
        if (expected > UINT64_MAX - reserve ||
            (reserve != 0U &&
             writer_extent_overlaps(&model.extent_leaf, expected,
                                    expected + reserve, model.node_size))) {
            set_error(error, error_size,
                      "Btrfs file %" PRIu64
                      " lacks its required 10 percent growth reserve",
                      file->inode);
            result = -1;
            break;
        }
    }
    if (result == 0) {
        BtrfsAnalysis analysis;
        if (btrfs_analyse(path, &analysis, error, error_size) != 0)
            result = -1;
        else {
            if (analysis.fragmented_files != 0U) {
                set_error(error, error_size,
                          "Btrfs independent analyser still reports fragmented files");
                result = -1;
            }
            btrfs_analysis_free(&analysis);
        }
    }
    writer_model_free(&model);
    return result;
}

int btrfs_build_stage(const char *source_path, const char *stage_path,
                      bool growth, unsigned growth_percent,
                      bool live_updates, uint64_t *commit_bytes,
                      char *error, size_t error_size)
{
    if (growth && growth_percent != 10U) {
        set_error(error, error_size,
                  "Btrfs Growth Defrag requires exactly 10 percent reserve");
        return -1;
    }
    WriterModel source;
    if (writer_model_load(source_path, &source,
                          error, error_size) != 0)
        return -1;
    (void)unlink(stage_path);
    int flags = O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int stage_fd = open(stage_path, flags, 0600);
    if (stage_fd < 0 ||
        ftruncate(stage_fd, (off_t)source.total_bytes) != 0) {
        if (stage_fd >= 0)
            (void)close(stage_fd);
        set_error(error, error_size,
                  "cannot create Btrfs recovery stage: %s",
                  strerror(errno));
        writer_model_free(&source);
        return -1;
    }
    int result =
        writer_copy_prefix(&source, stage_fd, error, error_size);
    if (result != 0)
        goto done;

    RangeVec claimed = {0};
    if (writer_build_claimed(&source, &claimed,
                             error, error_size) != 0) {
        result = -1;
        goto claimed_done;
    }
    const uint64_t new_generation = source.generation + 1U;
    size_t live_sequence = 0U;
    for (size_t file_index = 0U;
         file_index < source.files.count; ++file_index) {
        WriterFile *file = &source.files.items[file_index];
        if (file->move_count == 0U)
            continue;
        const uint64_t blocks =
            file->total_bytes / source.sector_size;
        const uint64_t reserve_blocks = growth
            ? (blocks * growth_percent + 99U) / 100U : 0U;
        if (reserve_blocks >
            UINT64_MAX / source.sector_size) {
            set_error(error, error_size,
                      "Btrfs growth reserve overflows");
            result = -1;
            break;
        }
        const uint64_t reserve =
            reserve_blocks * source.sector_size;
        uint64_t destination = 0U;
        if (writer_choose_run(&source, &claimed,
                              file->total_bytes, reserve,
                              &destination, error, error_size) != 0) {
            result = -1;
            break;
        }
        uint64_t cursor = destination;
        for (size_t offset = 0U; offset < file->move_count; ++offset) {
            WriterMove *move =
                &source.moves.items[file->first_move + offset];
            move->new_bytenr = cursor;
            const int copy_rc =
                writer_copy_logical(&source, stage_fd,
                                    move->old_bytenr,
                                    move->new_bytenr,
                                    move->length,
                                    error, error_size);
            if (copy_rc != 0) {
                result = copy_rc;
                break;
            }
            TreeItem *fs_item =
                &source.fs_leaf.items.items[move->fs_item_index];
            TreeItem *extent_item =
                &source.extent_leaf.items.items[move->extent_item_index];
            infiltratr_store_le64(fs_item->data, new_generation);
            infiltratr_store_le64(fs_item->data + 21U,
                                  move->new_bytenr);
            extent_item->key.objectid = move->new_bytenr;
            infiltratr_store_le64(extent_item->data + 8U,
                                  new_generation);
            cursor += move->length;
        }
        if (result != 0)
            break;
        if (live_updates) {
            (void)printf(
                "@@LIVE_RANGES {\"ranges\":[[%" PRIu64 ",%" PRIu64
                ",1]],\"sequence\":%zu}\n",
                destination / source.sector_size,
                (destination + file->total_bytes) / source.sector_size,
                ++live_sequence);
            (void)fflush(stdout);
        }
    }
    if (result == 0 &&
        (writer_update_root_item(&source.root_leaf,
                                 BTRFS_EXTENT_TREE_OBJECTID,
                                 new_generation,
                                 error, error_size) != 0 ||
         writer_update_root_item(&source.root_leaf,
                                 BTRFS_FS_TREE_OBJECTID,
                                 new_generation,
                                 error, error_size) != 0 ||
         writer_update_metadata_generation(
             &source.extent_leaf, source.root_leaf.logical,
             new_generation, error, error_size) != 0 ||
         writer_update_metadata_generation(
             &source.extent_leaf, source.extent_leaf.logical,
             new_generation, error, error_size) != 0 ||
         writer_update_metadata_generation(
             &source.extent_leaf, source.fs_leaf.logical,
             new_generation, error, error_size) != 0 ||
         writer_leaf_write(&source, stage_fd, &source.fs_leaf,
                           new_generation, error, error_size) != 0 ||
         writer_leaf_write(&source, stage_fd, &source.extent_leaf,
                           new_generation, error, error_size) != 0 ||
         writer_leaf_write(&source, stage_fd, &source.root_leaf,
                           new_generation, error, error_size) != 0 ||
         writer_super_write(stage_fd, &source, new_generation,
                            error, error_size) != 0 ||
         fsync(stage_fd) != 0)) {
        if (error != NULL && error[0] == '\0')
            set_error(error, error_size,
                      "cannot sync Btrfs recovery stage: %s",
                      strerror(errno));
        result = -1;
    }

claimed_done:
    free(claimed.items);
done:
    (void)close(stage_fd);
    if (result == 0) {
        WriterModel after;
        if (writer_model_load(stage_path, &after,
                              error, error_size) != 0)
            result = -1;
        else {
            if (writer_payload_compare(&source, &after,
                                       error, error_size) != 0 ||
                btrfs_verify_layout(stage_path, growth,
                                    growth_percent,
                                    error, error_size) != 0)
                result = -1;
            writer_model_free(&after);
        }
    }
    if (result == 0 && commit_bytes != NULL)
        *commit_bytes = source.total_bytes;
    writer_model_free(&source);
    return result;
}
