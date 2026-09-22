// SPDX-License-Identifier: GPL-3.0-or-later
#include "zfs_native.h"

#include "ld_io.h"
#include "infiltratr/endian.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ZFS_VDEV_LABEL_START_SIZE (UINT64_C(4) << 20)
#define ZFS_VDEV_LABEL_END_SIZE (UINT64_C(512) << 10)
#define ZFS_MAX_BLOCK_SIZE (UINT64_C(32) << 20)
#define ZFS_MAX_METASLABS UINT64_C(1048576)
#define ZFS_BP_SIZE 128U
#define ZFS_DNODE_SIZE 512U
#define ZFS_DNODE_CORE_SIZE 64U
#define ZFS_DNODE_MAX_LEVELS 5U
#define ZFS_DNODE_MIN_INDBLKSHIFT 12U
#define ZFS_DNODE_MAX_INDBLKSHIFT 17U
#define ZFS_DMU_OT_OBJECT_ARRAY 2U
#define ZFS_DMU_OT_SPACE_MAP_HEADER 7U
#define ZFS_DMU_OT_SPACE_MAP 8U
#define ZFS_DMU_OT_OBJSET 11U
#define ZFS_CHECKSUM_OFF 2U
#define ZFS_CHECKSUM_FLETCHER4 7U
#define ZFS_COMPRESS_OFF 2U
#define ZFS_COMPRESS_LZJB 3U
#define ZFS_COMPRESS_LZ4 15U
#define ZFS_POOL_VERSION_LAST_LEGACY 28U
#define ZFS_SM_DEBUG_PREFIX 2U
#define ZFS_SM2_PREFIX 3U
#define ZFS_SM_NO_VDEVID (UINT64_C(1) << 24)

typedef struct {
    uint64_t asize;
    uint64_t vdev;
    uint64_t offset;
    bool gang;
} ZfsDva;

typedef struct {
    ZfsDva dva[3];
    uint64_t lsize;
    uint64_t psize;
    uint64_t logical_birth;
    uint64_t checksum_words[4];
    uint32_t compression;
    uint32_t checksum;
    uint32_t type;
    uint32_t level;
    bool embedded;
    bool uses_crypt;
    bool dedup;
    bool hole;
    LdZfsByteOrder data_order;
} ZfsBlockPointer;

typedef struct {
    uint8_t *raw;
    size_t raw_size;
    LdZfsByteOrder order;
    uint8_t type;
    uint8_t indblkshift;
    uint8_t nlevels;
    uint8_t nblkptr;
    uint8_t bonustype;
    uint8_t checksum;
    uint8_t compress;
    uint8_t flags;
    uint16_t datablkszsec;
    uint16_t bonuslen;
    uint8_t extra_slots;
    uint64_t maxblkid;
    uint64_t used;
} ZfsDnode;

typedef struct {
    uint64_t start;
    uint64_t end;
} ZfsByteRange;

typedef struct {
    ZfsByteRange *items;
    size_t count;
    size_t capacity;
} ZfsRangeSet;

typedef struct {
    int fd;
    LdZfsSummary summary;
    uint8_t *mos;
    size_t mos_size;
    LdZfsByteOrder mos_order;
    ZfsDnode meta_dnode;
} ZfsContext;

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size != 0U)
        (void)snprintf(error, error_size, "%s", message);
}

static void set_errno_error(char *error, size_t error_size, const char *prefix)
{
    if (error != NULL && error_size != 0U)
        (void)snprintf(error, error_size, "%s: %s", prefix, strerror(errno));
}

static uint16_t load_u16(const uint8_t *data, LdZfsByteOrder order)
{
    return order == LD_ZFS_BYTE_ORDER_LITTLE
        ? infiltratr_load_le16(data)
        : infiltratr_load_be16(data);
}

static uint32_t load_u32(const uint8_t *data, LdZfsByteOrder order)
{
    return order == LD_ZFS_BYTE_ORDER_LITTLE
        ? infiltratr_load_le32(data)
        : infiltratr_load_be32(data);
}

static uint64_t load_u64_order(const uint8_t *data, LdZfsByteOrder order)
{
    return order == LD_ZFS_BYTE_ORDER_LITTLE
        ? infiltratr_load_le64(data)
        : infiltratr_load_be64(data);
}

static int range_reserve(ZfsRangeSet *set, size_t needed)
{
    if (needed <= set->capacity)
        return 0;
    size_t capacity = set->capacity == 0U ? 16U : set->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2U) {
            errno = EOVERFLOW;
            return -1;
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*set->items)) {
        errno = EOVERFLOW;
        return -1;
    }
    ZfsByteRange *items = realloc(set->items, capacity * sizeof(*items));
    if (items == NULL)
        return -1;
    set->items = items;
    set->capacity = capacity;
    return 0;
}

static uint64_t range_total(const ZfsRangeSet *set)
{
    uint64_t total = 0U;
    for (size_t index = 0U; index < set->count; ++index)
        total += set->items[index].end - set->items[index].start;
    return total;
}

static int range_add(ZfsRangeSet *set, uint64_t start, uint64_t length)
{
    if (length == 0U)
        return 0;
    if (start > UINT64_MAX - length) {
        errno = EOVERFLOW;
        return -1;
    }
    uint64_t end = start + length;
    size_t first = 0U;
    while (first < set->count && set->items[first].end < start)
        first++;

    size_t last = first;
    while (last < set->count && set->items[last].start <= end) {
        if (set->items[last].start < start)
            start = set->items[last].start;
        if (set->items[last].end > end)
            end = set->items[last].end;
        last++;
    }

    if (first == last) {
        if (range_reserve(set, set->count + 1U) != 0)
            return -1;
        memmove(&set->items[first + 1U], &set->items[first],
                (set->count - first) * sizeof(*set->items));
        set->items[first].start = start;
        set->items[first].end = end;
        set->count++;
        return 0;
    }

    set->items[first].start = start;
    set->items[first].end = end;
    if (last > first + 1U) {
        memmove(&set->items[first + 1U], &set->items[last],
                (set->count - last) * sizeof(*set->items));
        set->count -= last - first - 1U;
    }
    return 0;
}

static int range_remove(ZfsRangeSet *set, uint64_t start, uint64_t length)
{
    if (length == 0U)
        return 0;
    if (start > UINT64_MAX - length) {
        errno = EOVERFLOW;
        return -1;
    }
    const uint64_t end = start + length;

    for (size_t index = 0U; index < set->count;) {
        ZfsByteRange current = set->items[index];
        if (current.end <= start) {
            index++;
            continue;
        }
        if (current.start >= end)
            break;

        if (start <= current.start && end >= current.end) {
            memmove(&set->items[index], &set->items[index + 1U],
                    (set->count - index - 1U) * sizeof(*set->items));
            set->count--;
            continue;
        }
        if (start <= current.start) {
            set->items[index].start = end;
            break;
        }
        if (end >= current.end) {
            set->items[index].end = start;
            index++;
            continue;
        }

        if (range_reserve(set, set->count + 1U) != 0)
            return -1;
        memmove(&set->items[index + 2U], &set->items[index + 1U],
                (set->count - index - 1U) * sizeof(*set->items));
        set->items[index].end = start;
        set->items[index + 1U].start = end;
        set->items[index + 1U].end = current.end;
        set->count++;
        break;
    }
    return 0;
}

static void range_destroy(ZfsRangeSet *set)
{
    free(set->items);
    memset(set, 0, sizeof(*set));
}

static void decode_block_pointer(const uint8_t raw[ZFS_BP_SIZE],
                                 LdZfsByteOrder container_order,
                                 ZfsBlockPointer *bp)
{
    memset(bp, 0, sizeof(*bp));
    for (size_t index = 0U; index < 3U; ++index) {
        const uint64_t word0 =
            load_u64_order(raw + index * 16U, container_order);
        const uint64_t word1 =
            load_u64_order(raw + index * 16U + 8U, container_order);
        bp->dva[index].asize = (word0 & UINT64_C(0xffffff)) << 9U;
        bp->dva[index].vdev = (word0 >> 32U) & UINT64_C(0xffffff);
        bp->dva[index].offset =
            (word1 & UINT64_C(0x7fffffffffffffff)) << 9U;
        bp->dva[index].gang = (word1 >> 63U) != 0U;
    }

    const uint64_t prop = load_u64_order(raw + 48U, container_order);
    bp->embedded = ((prop >> 39U) & 1U) != 0U;
    if (!bp->embedded) {
        bp->lsize = ((prop & UINT64_C(0xffff)) + 1U) << 9U;
        bp->psize =
            (((prop >> 16U) & UINT64_C(0xffff)) + 1U) << 9U;
    }
    bp->compression = (uint32_t)((prop >> 32U) & UINT64_C(0x7f));
    bp->checksum = (uint32_t)((prop >> 40U) & UINT64_C(0xff));
    bp->type = (uint32_t)((prop >> 48U) & UINT64_C(0xff));
    bp->level = (uint32_t)((prop >> 56U) & UINT64_C(0x1f));
    bp->uses_crypt = ((prop >> 61U) & 1U) != 0U;
    bp->dedup = ((prop >> 62U) & 1U) != 0U;
    bp->data_order = ((prop >> 63U) & 1U) != 0U
        ? LD_ZFS_BYTE_ORDER_LITTLE
        : LD_ZFS_BYTE_ORDER_BIG;
    bp->logical_birth = load_u64_order(raw + 80U, container_order);
    for (size_t index = 0U; index < 4U; ++index)
        bp->checksum_words[index] =
            load_u64_order(raw + 96U + index * 8U, container_order);

    const uint64_t first0 = load_u64_order(raw, container_order);
    const uint64_t first1 = load_u64_order(raw + 8U, container_order);
    bp->hole = !bp->embedded && first0 == 0U && first1 == 0U;
}

static bool checksum_fletcher4(const uint8_t *data, size_t length,
                               LdZfsByteOrder order,
                               const uint64_t expected[4])
{
    if ((length & 3U) != 0U)
        return false;
    uint64_t a = 0U;
    uint64_t b = 0U;
    uint64_t c = 0U;
    uint64_t d = 0U;
    for (size_t offset = 0U; offset < length; offset += 4U) {
        const uint32_t value = load_u32(data + offset, order);
        a += value;
        b += a;
        c += b;
        d += c;
    }
    return a == expected[0] && b == expected[1] &&
           c == expected[2] && d == expected[3];
}

static int decompress_lzjb(const uint8_t *source, size_t source_length,
                           uint8_t *destination, size_t destination_length)
{
    size_t source_offset = 0U;
    size_t destination_offset = 0U;
    uint8_t copy_map = 0U;
    unsigned int copy_mask = 1U << 7U;

    while (destination_offset < destination_length) {
        copy_mask <<= 1U;
        if (copy_mask == (1U << 8U)) {
            copy_mask = 1U;
            if (source_offset >= source_length)
                return -1;
            copy_map = source[source_offset++];
        }

        if ((copy_map & copy_mask) != 0U) {
            if (source_length - source_offset < 2U)
                return -1;
            const unsigned int match_length =
                (unsigned int)(source[source_offset] >> 2U) + 3U;
            const unsigned int match_offset =
                (((unsigned int)source[source_offset] << 8U) |
                 source[source_offset + 1U]) & 0x3ffU;
            source_offset += 2U;
            if (match_offset == 0U || match_offset > destination_offset)
                return -1;
            size_t copy_from = destination_offset - match_offset;
            for (unsigned int index = 0U;
                 index < match_length &&
                 destination_offset < destination_length;
                 ++index)
                destination[destination_offset++] =
                    destination[copy_from++];
        } else {
            if (source_offset >= source_length)
                return -1;
            destination[destination_offset++] = source[source_offset++];
        }
    }
    return 0;
}

static int lz4_extended_length(const uint8_t *source, size_t source_length,
                               size_t *source_offset, size_t *length)
{
    if (*length != 15U)
        return 0;
    for (;;) {
        if (*source_offset >= source_length)
            return -1;
        const uint8_t value = source[(*source_offset)++];
        if (*length > SIZE_MAX - value)
            return -1;
        *length += value;
        if (value != 255U)
            return 0;
    }
}

static int decompress_lz4_raw(const uint8_t *source, size_t source_length,
                              uint8_t *destination,
                              size_t destination_length)
{
    size_t source_offset = 0U;
    size_t destination_offset = 0U;

    while (source_offset < source_length) {
        const uint8_t token = source[source_offset++];
        size_t literal_length = (size_t)(token >> 4U);
        if (lz4_extended_length(source, source_length, &source_offset,
                                &literal_length) != 0)
            return -1;
        if (literal_length > source_length - source_offset ||
            literal_length > destination_length - destination_offset)
            return -1;
        memcpy(destination + destination_offset,
               source + source_offset, literal_length);
        source_offset += literal_length;
        destination_offset += literal_length;

        if (source_offset == source_length)
            return destination_offset == destination_length ? 0 : -1;
        if (source_length - source_offset < 2U)
            return -1;

        const size_t match_offset =
            (size_t)source[source_offset] |
            ((size_t)source[source_offset + 1U] << 8U);
        source_offset += 2U;
        if (match_offset == 0U || match_offset > destination_offset)
            return -1;

        size_t match_length = (size_t)(token & 0x0fU);
        if (lz4_extended_length(source, source_length, &source_offset,
                                &match_length) != 0 ||
            match_length > SIZE_MAX - 4U)
            return -1;
        match_length += 4U;
        if (match_length > destination_length - destination_offset)
            return -1;

        size_t copy_from = destination_offset - match_offset;
        for (size_t index = 0U; index < match_length; ++index)
            destination[destination_offset++] = destination[copy_from++];
    }

    return destination_offset == destination_length ? 0 : -1;
}

static int decompress_lz4(const uint8_t *source, size_t source_length,
                          uint8_t *destination, size_t destination_length)
{
    if (source_length < 4U)
        return -1;
    const uint32_t compressed_length =
        ((uint32_t)source[0] << 24U) |
        ((uint32_t)source[1] << 16U) |
        ((uint32_t)source[2] << 8U) |
        (uint32_t)source[3];
    if ((uint64_t)compressed_length + 4U > source_length)
        return -1;
    return decompress_lz4_raw(source + 4U, compressed_length,
                              destination, destination_length);
}

static int read_block_pointer_data(ZfsContext *context,
                                   const ZfsBlockPointer *bp,
                                   uint8_t **data_out, size_t *length_out,
                                   char *error, size_t error_size)
{
    *data_out = NULL;
    *length_out = 0U;

    if (bp->hole) {
        if (bp->lsize == 0U)
            return 0;
        if (bp->lsize > SIZE_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
        uint8_t *zeros = calloc(1U, (size_t)bp->lsize);
        if (zeros == NULL)
            return -1;
        *data_out = zeros;
        *length_out = (size_t)bp->lsize;
        return 0;
    }
    if (bp->embedded || bp->uses_crypt) {
        errno = ENOTSUP;
        set_error(error, error_size,
                  "ZFS embedded/encrypted metadata is outside the bounded exact reader");
        return -1;
    }
    if (bp->lsize == 0U || bp->psize == 0U ||
        bp->lsize > ZFS_MAX_BLOCK_SIZE || bp->psize > ZFS_MAX_BLOCK_SIZE ||
        bp->lsize > SIZE_MAX || bp->psize > SIZE_MAX) {
        errno = EINVAL;
        set_error(error, error_size, "invalid ZFS block-pointer size");
        return -1;
    }
    if (bp->checksum != ZFS_CHECKSUM_OFF &&
        bp->checksum != ZFS_CHECKSUM_FLETCHER4) {
        errno = ENOTSUP;
        set_error(error, error_size,
                  "unsupported ZFS metadata checksum in bounded exact reader");
        return -1;
    }
    if (bp->compression != ZFS_COMPRESS_OFF &&
        bp->compression != ZFS_COMPRESS_LZJB &&
        bp->compression != ZFS_COMPRESS_LZ4) {
        errno = ENOTSUP;
        set_error(error, error_size,
                  "unsupported ZFS metadata compression in bounded exact reader");
        return -1;
    }

    int last_errno = EIO;
    for (size_t dva_index = 0U; dva_index < 3U; ++dva_index) {
        const ZfsDva *dva = &bp->dva[dva_index];
        if (dva->asize == 0U || dva->vdev != context->summary.top_vdev_id)
            continue;
        if (dva->gang) {
            last_errno = ENOTSUP;
            continue;
        }
        if (dva->asize < bp->psize ||
            dva->offset > context->summary.top_vdev_asize ||
            bp->psize > context->summary.top_vdev_asize - dva->offset) {
            last_errno = EINVAL;
            continue;
        }
        if (dva->offset >
            UINT64_MAX - ZFS_VDEV_LABEL_START_SIZE) {
            last_errno = EOVERFLOW;
            continue;
        }
        const uint64_t physical =
            ZFS_VDEV_LABEL_START_SIZE + dva->offset;
        if (physical > context->summary.size_bytes ||
            bp->psize > context->summary.size_bytes - physical) {
            last_errno = EIO;
            continue;
        }

        uint8_t *physical_data = malloc((size_t)bp->psize);
        if (physical_data == NULL)
            return -1;
        const ssize_t count =
            ld_pread_full(context->fd, physical_data,
                          (size_t)bp->psize, physical);
        if (count < 0 || (uint64_t)count != bp->psize) {
            last_errno = count < 0 ? errno : EIO;
            free(physical_data);
            continue;
        }
        if (bp->checksum == ZFS_CHECKSUM_FLETCHER4 &&
            !checksum_fletcher4(physical_data, (size_t)bp->psize,
                                bp->data_order, bp->checksum_words)) {
            last_errno = EIO;
            free(physical_data);
            continue;
        }

        uint8_t *logical_data = malloc((size_t)bp->lsize);
        if (logical_data == NULL) {
            free(physical_data);
            return -1;
        }
        int decompression = 0;
        if (bp->compression == ZFS_COMPRESS_OFF) {
            if (bp->psize != bp->lsize)
                decompression = -1;
            else
                memcpy(logical_data, physical_data, (size_t)bp->lsize);
        } else if (bp->compression == ZFS_COMPRESS_LZJB) {
            decompression =
                decompress_lzjb(physical_data, (size_t)bp->psize,
                                logical_data, (size_t)bp->lsize);
        } else {
            decompression =
                decompress_lz4(physical_data, (size_t)bp->psize,
                               logical_data, (size_t)bp->lsize);
        }
        free(physical_data);
        if (decompression != 0) {
            last_errno = EIO;
            free(logical_data);
            continue;
        }

        *data_out = logical_data;
        *length_out = (size_t)bp->lsize;
        return 0;
    }

    errno = last_errno;
    if (last_errno == ENOTSUP)
        set_error(error, error_size,
                  "ZFS gang block is outside the bounded exact reader");
    else
        set_error(error, error_size,
                  "no verified ZFS block copy could be read");
    return -1;
}

static int decode_dnode_header(const uint8_t *raw, size_t raw_size,
                               LdZfsByteOrder order, ZfsDnode *dnode)
{
    if (raw_size < ZFS_DNODE_SIZE) {
        errno = EINVAL;
        return -1;
    }
    memset(dnode, 0, sizeof(*dnode));
    dnode->order = order;
    dnode->type = raw[0];
    dnode->indblkshift = raw[1];
    dnode->nlevels = raw[2];
    dnode->nblkptr = raw[3];
    dnode->bonustype = raw[4];
    dnode->checksum = raw[5];
    dnode->compress = raw[6];
    dnode->flags = raw[7];
    dnode->datablkszsec = load_u16(raw + 8U, order);
    dnode->bonuslen = load_u16(raw + 10U, order);
    dnode->extra_slots = raw[12];
    dnode->maxblkid = load_u64_order(raw + 16U, order);
    dnode->used = load_u64_order(raw + 24U, order);
    return 0;
}

static void dnode_destroy(ZfsDnode *dnode)
{
    free(dnode->raw);
    memset(dnode, 0, sizeof(*dnode));
}

static int dnode_attach_raw(ZfsDnode *dnode, uint8_t *raw, size_t raw_size,
                            LdZfsByteOrder order)
{
    ZfsDnode parsed;
    if (decode_dnode_header(raw, raw_size, order, &parsed) != 0)
        return -1;
    const size_t slots = (size_t)parsed.extra_slots + 1U;
    if (slots > SIZE_MAX / ZFS_DNODE_SIZE ||
        slots * ZFS_DNODE_SIZE != raw_size) {
        errno = EINVAL;
        return -1;
    }
    const size_t pointers_end =
        ZFS_DNODE_CORE_SIZE + (size_t)parsed.nblkptr * ZFS_BP_SIZE;
    if (pointers_end > raw_size ||
        parsed.bonuslen > raw_size - pointers_end) {
        errno = EINVAL;
        return -1;
    }
    parsed.raw = raw;
    parsed.raw_size = raw_size;
    *dnode = parsed;
    return 0;
}

static int dnode_bp(const ZfsDnode *dnode, size_t index,
                    ZfsBlockPointer *bp)
{
    if (index >= dnode->nblkptr) {
        errno = EINVAL;
        return -1;
    }
    const size_t offset =
        ZFS_DNODE_CORE_SIZE + index * ZFS_BP_SIZE;
    if (offset > dnode->raw_size ||
        ZFS_BP_SIZE > dnode->raw_size - offset) {
        errno = EINVAL;
        return -1;
    }
    decode_block_pointer(dnode->raw + offset, dnode->order, bp);
    return 0;
}

static int power_u64(uint64_t base, unsigned int exponent, uint64_t *result)
{
    uint64_t value = 1U;
    for (unsigned int index = 0U; index < exponent; ++index) {
        if (base != 0U && value > UINT64_MAX / base) {
            errno = EOVERFLOW;
            return -1;
        }
        value *= base;
    }
    *result = value;
    return 0;
}

static int object_lookup_bp(ZfsContext *context, const ZfsDnode *dnode,
                            uint64_t block_id, ZfsBlockPointer *result,
                            char *error, size_t error_size)
{
    if (dnode->nlevels == 0U || dnode->nlevels > ZFS_DNODE_MAX_LEVELS ||
        dnode->nblkptr == 0U ||
        dnode->indblkshift < ZFS_DNODE_MIN_INDBLKSHIFT ||
        dnode->indblkshift > ZFS_DNODE_MAX_INDBLKSHIFT) {
        errno = ENOTSUP;
        set_error(error, error_size,
                  "unsupported ZFS dnode block-tree geometry");
        return -1;
    }

    const uint64_t entries_per_indirect =
        UINT64_C(1) << (dnode->indblkshift - 7U);
    uint64_t root_span = 0U;
    if (power_u64(entries_per_indirect, dnode->nlevels - 1U,
                  &root_span) != 0)
        return -1;
    const uint64_t root_index = block_id / root_span;
    if (root_index >= dnode->nblkptr) {
        memset(result, 0, sizeof(*result));
        result->hole = true;
        return 0;
    }

    ZfsBlockPointer current;
    if (dnode_bp(dnode, (size_t)root_index, &current) != 0)
        return -1;
    uint64_t remaining = block_id % root_span;

    for (unsigned int level = dnode->nlevels - 1U;
         level > 0U; --level) {
        if (current.hole) {
            *result = current;
            return 0;
        }
        uint8_t *indirect = NULL;
        size_t indirect_length = 0U;
        if (read_block_pointer_data(context, &current,
                                    &indirect, &indirect_length,
                                    error, error_size) != 0)
            return -1;
        if (indirect_length < ZFS_BP_SIZE ||
            indirect_length % ZFS_BP_SIZE != 0U) {
            free(indirect);
            errno = EINVAL;
            set_error(error, error_size,
                      "invalid ZFS indirect-block size");
            return -1;
        }

        uint64_t child_span = 0U;
        if (power_u64(entries_per_indirect, level - 1U,
                      &child_span) != 0) {
            free(indirect);
            return -1;
        }
        const uint64_t child_index = remaining / child_span;
        if (child_index > SIZE_MAX / ZFS_BP_SIZE ||
            (size_t)child_index >= indirect_length / ZFS_BP_SIZE) {
            free(indirect);
            errno = EINVAL;
            set_error(error, error_size,
                      "ZFS indirect-block index is out of range");
            return -1;
        }
        decode_block_pointer(indirect + (size_t)child_index * ZFS_BP_SIZE,
                             current.data_order, &current);
        free(indirect);
        remaining %= child_span;
    }

    *result = current;
    return 0;
}

static int object_read_bytes(ZfsContext *context, const ZfsDnode *dnode,
                             uint64_t offset, void *buffer, size_t length,
                             LdZfsByteOrder *data_order,
                             char *error, size_t error_size)
{
    if (length == 0U)
        return 0;
    if (dnode->datablkszsec == 0U) {
        errno = EINVAL;
        set_error(error, error_size, "ZFS object has no data block size");
        return -1;
    }
    const uint64_t block_size = (uint64_t)dnode->datablkszsec << 9U;
    if (block_size == 0U || block_size > ZFS_MAX_BLOCK_SIZE) {
        errno = EINVAL;
        set_error(error, error_size, "invalid ZFS object block size");
        return -1;
    }
    if (offset > UINT64_MAX - length) {
        errno = EOVERFLOW;
        return -1;
    }

    memset(buffer, 0, length);
    size_t copied = 0U;
    bool have_order = false;
    LdZfsByteOrder observed_order = LD_ZFS_BYTE_ORDER_LITTLE;

    while (copied < length) {
        const uint64_t absolute = offset + copied;
        const uint64_t block_id = absolute / block_size;
        const size_t in_block = (size_t)(absolute % block_size);
        size_t piece = (size_t)block_size - in_block;
        if (piece > length - copied)
            piece = length - copied;

        if (block_id <= dnode->maxblkid) {
            ZfsBlockPointer bp;
            if (object_lookup_bp(context, dnode, block_id, &bp,
                                 error, error_size) != 0)
                return -1;
            if (!bp.hole) {
                uint8_t *block = NULL;
                size_t block_length = 0U;
                if (read_block_pointer_data(context, &bp,
                                            &block, &block_length,
                                            error, error_size) != 0)
                    return -1;
                if (in_block > block_length ||
                    piece > block_length - in_block) {
                    free(block);
                    errno = EINVAL;
                    set_error(error, error_size,
                              "ZFS data block is shorter than dnode geometry");
                    return -1;
                }
                if (!have_order) {
                    observed_order = bp.data_order;
                    have_order = true;
                } else if (observed_order != bp.data_order) {
                    free(block);
                    errno = ENOTSUP;
                    set_error(error, error_size,
                              "mixed-endian ZFS object blocks are outside the bounded reader");
                    return -1;
                }
                memcpy((uint8_t *)buffer + copied,
                       block + in_block, piece);
                free(block);
            }
        }
        copied += piece;
    }

    if (data_order != NULL && have_order)
        *data_order = observed_order;
    return 0;
}

static int read_object_dnode(ZfsContext *context, uint64_t object,
                             ZfsDnode *dnode,
                             char *error, size_t error_size)
{
    if (object > UINT64_MAX / ZFS_DNODE_SIZE) {
        errno = EOVERFLOW;
        return -1;
    }
    const uint64_t offset = object * ZFS_DNODE_SIZE;
    uint8_t first[ZFS_DNODE_SIZE];
    LdZfsByteOrder order = context->mos_order;
    if (object_read_bytes(context, &context->meta_dnode, offset,
                          first, sizeof(first), &order,
                          error, error_size) != 0)
        return -1;

    ZfsDnode header;
    if (decode_dnode_header(first, sizeof(first), order, &header) != 0)
        return -1;
    const size_t slots = (size_t)header.extra_slots + 1U;
    if (slots > SIZE_MAX / ZFS_DNODE_SIZE) {
        errno = EOVERFLOW;
        return -1;
    }
    const size_t raw_size = slots * ZFS_DNODE_SIZE;
    uint8_t *raw = malloc(raw_size);
    if (raw == NULL)
        return -1;
    if (raw_size == sizeof(first)) {
        memcpy(raw, first, sizeof(first));
    } else if (object_read_bytes(context, &context->meta_dnode, offset,
                                 raw, raw_size, &order,
                                 error, error_size) != 0) {
        free(raw);
        return -1;
    }

    if (dnode_attach_raw(dnode, raw, raw_size, order) != 0) {
        free(raw);
        return -1;
    }
    return 0;
}

static const uint8_t *dnode_bonus(const ZfsDnode *dnode)
{
    const size_t offset =
        ZFS_DNODE_CORE_SIZE + (size_t)dnode->nblkptr * ZFS_BP_SIZE;
    return dnode->raw + offset;
}

static int context_open(const char *path, ZfsContext *context,
                        char *error, size_t error_size)
{
    memset(context, 0, sizeof(*context));
    context->fd = -1;
    if (zfs_read_summary(path, &context->summary,
                         error, error_size) != 0)
        return -1;
    if (!context->summary.single_leaf_supported) {
        errno = ENOTSUP;
        set_error(error, error_size,
                  "ZFS exact analysis requires one qualified top-level disk vdev");
        return -1;
    }
    if (context->summary.uberblock_version == 0U ||
        context->summary.uberblock_version > ZFS_POOL_VERSION_LAST_LEGACY) {
        errno = ENOTSUP;
        set_error(error, error_size,
                  "ZFS exact analysis currently requires legacy pool version 1-28; feature-flag pools remain summary-only");
        return -1;
    }
    if (context->summary.top_vdev_asize == 0U ||
        context->summary.metaslab_shift >= 63U) {
        errno = EINVAL;
        set_error(error, error_size, "invalid ZFS vdev allocation geometry");
        return -1;
    }
    const uint64_t metaslab_size =
        UINT64_C(1) << context->summary.metaslab_shift;
    if ((context->summary.top_vdev_asize % metaslab_size) != 0U) {
        errno = EINVAL;
        set_error(error, error_size,
                  "ZFS top-vdev size is not aligned to metaslab geometry");
        return -1;
    }

    context->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (context->fd < 0) {
        set_errno_error(error, error_size, "open");
        return -1;
    }

    uint8_t uberblock[1024];
    const ssize_t count =
        ld_pread_full(context->fd, uberblock, sizeof(uberblock),
                      context->summary.uberblock_magic_offset);
    if (count < 0 || (size_t)count != sizeof(uberblock)) {
        const int saved_errno = count < 0 ? errno : EIO;
        (void)close(context->fd);
        context->fd = -1;
        errno = saved_errno;
        set_errno_error(error, error_size, "read ZFS uberblock");
        return -1;
    }

    ZfsBlockPointer root;
    decode_block_pointer(uberblock + 40U, context->summary.byte_order, &root);
    if (root.type != ZFS_DMU_OT_OBJSET || root.level != 0U ||
        root.embedded || root.uses_crypt) {
        (void)close(context->fd);
        context->fd = -1;
        errno = ENOTSUP;
        set_error(error, error_size,
                  "unsupported ZFS MOS root block-pointer form");
        return -1;
    }
    if (read_block_pointer_data(context, &root,
                                &context->mos, &context->mos_size,
                                error, error_size) != 0) {
        const int saved_errno = errno;
        (void)close(context->fd);
        context->fd = -1;
        errno = saved_errno;
        return -1;
    }
    if (context->mos_size < ZFS_DNODE_SIZE) {
        free(context->mos);
        context->mos = NULL;
        (void)close(context->fd);
        context->fd = -1;
        errno = EINVAL;
        set_error(error, error_size, "ZFS MOS objset is too small");
        return -1;
    }
    context->mos_order = root.data_order;

    uint8_t *meta_raw = malloc(ZFS_DNODE_SIZE);
    if (meta_raw == NULL) {
        free(context->mos);
        context->mos = NULL;
        (void)close(context->fd);
        context->fd = -1;
        return -1;
    }
    memcpy(meta_raw, context->mos, ZFS_DNODE_SIZE);
    if (dnode_attach_raw(&context->meta_dnode, meta_raw,
                         ZFS_DNODE_SIZE, context->mos_order) != 0) {
        free(meta_raw);
        free(context->mos);
        context->mos = NULL;
        (void)close(context->fd);
        context->fd = -1;
        set_error(error, error_size, "invalid ZFS MOS metadnode");
        return -1;
    }
    if (context->meta_dnode.type != 10U) {
        dnode_destroy(&context->meta_dnode);
        free(context->mos);
        context->mos = NULL;
        (void)close(context->fd);
        context->fd = -1;
        errno = EINVAL;
        set_error(error, error_size, "ZFS MOS metadnode has unexpected type");
        return -1;
    }
    return 0;
}

static void context_close(ZfsContext *context)
{
    dnode_destroy(&context->meta_dnode);
    free(context->mos);
    if (context->fd >= 0)
        (void)close(context->fd);
    memset(context, 0, sizeof(*context));
    context->fd = -1;
}

static int replay_space_map(ZfsContext *context, uint64_t metaslab_id,
                            uint64_t object, ZfsRangeSet *allocated,
                            char *error, size_t error_size)
{
    ZfsDnode space_map;
    memset(&space_map, 0, sizeof(space_map));
    if (read_object_dnode(context, object, &space_map,
                          error, error_size) != 0)
        return -1;

    int result = -1;
    if (space_map.type != ZFS_DMU_OT_SPACE_MAP ||
        space_map.bonustype != ZFS_DMU_OT_SPACE_MAP_HEADER ||
        space_map.bonuslen < 24U) {
        errno = EINVAL;
        set_error(error, error_size,
                  "ZFS metaslab array references an invalid space-map object");
        goto cleanup;
    }

    const uint8_t *bonus = dnode_bonus(&space_map);
    const uint64_t length = load_u64_order(bonus + 8U, space_map.order);
    const int64_t expected_alloc =
        (int64_t)load_u64_order(bonus + 16U, space_map.order);
    if ((length & 7U) != 0U || length > SIZE_MAX ||
        expected_alloc < 0) {
        errno = EINVAL;
        set_error(error, error_size, "invalid ZFS space-map summary");
        goto cleanup;
    }

    uint8_t *words = NULL;
    LdZfsByteOrder words_order = space_map.order;
    if (length != 0U) {
        words = malloc((size_t)length);
        if (words == NULL)
            goto cleanup;
        if (object_read_bytes(context, &space_map, 0U,
                              words, (size_t)length, &words_order,
                              error, error_size) != 0) {
            free(words);
            goto cleanup;
        }
    }

    const uint64_t metaslab_size =
        UINT64_C(1) << context->summary.metaslab_shift;
    if (metaslab_id > UINT64_MAX / metaslab_size) {
        free(words);
        errno = EOVERFLOW;
        goto cleanup;
    }
    const uint64_t metaslab_start = metaslab_id * metaslab_size;
    const uint64_t before = range_total(allocated);

    const size_t word_count = (size_t)(length / 8U);
    for (size_t index = 0U; index < word_count; ++index) {
        uint64_t word =
            load_u64_order(words + index * 8U, words_order);
        const uint32_t prefix = (uint32_t)((word >> 62U) & 3U);
        if (prefix == ZFS_SM_DEBUG_PREFIX)
            continue;

        uint64_t raw_offset;
        uint64_t raw_run;
        uint32_t type;
        if (prefix != ZFS_SM2_PREFIX) {
            raw_offset =
                (word >> 16U) & ((UINT64_C(1) << 47U) - 1U);
            type = (uint32_t)((word >> 15U) & 1U);
            raw_run = (word & UINT64_C(0x7fff)) + 1U;
        } else {
            raw_run =
                ((word >> 24U) & ((UINT64_C(1) << 36U) - 1U)) + 1U;
            const uint64_t vdev = word & UINT64_C(0xffffff);
            if (vdev != context->summary.top_vdev_id &&
                vdev != ZFS_SM_NO_VDEVID) {
                free(words);
                errno = ENOTSUP;
                set_error(error, error_size,
                          "ZFS space-map entry references another vdev");
                goto cleanup;
            }
            if (++index >= word_count) {
                free(words);
                errno = EINVAL;
                set_error(error, error_size,
                          "truncated ZFS two-word space-map entry");
                goto cleanup;
            }
            word = load_u64_order(words + index * 8U, words_order);
            type = (uint32_t)(word >> 63U);
            raw_offset = word & UINT64_C(0x7fffffffffffffff);
        }

        if (raw_offset > (UINT64_MAX >> context->summary.ashift) ||
            raw_run > (UINT64_MAX >> context->summary.ashift)) {
            free(words);
            errno = EOVERFLOW;
            goto cleanup;
        }
        const uint64_t relative = raw_offset << context->summary.ashift;
        const uint64_t run = raw_run << context->summary.ashift;
        if (relative >= metaslab_size ||
            run > metaslab_size - relative) {
            free(words);
            errno = EINVAL;
            set_error(error, error_size,
                      "ZFS space-map entry escapes its metaslab");
            goto cleanup;
        }
        const uint64_t start = metaslab_start + relative;
        if (type == 0U) {
            if (range_add(allocated, start, run) != 0) {
                free(words);
                goto cleanup;
            }
        } else {
            if (range_remove(allocated, start, run) != 0) {
                free(words);
                goto cleanup;
            }
        }
    }
    free(words);

    const uint64_t after = range_total(allocated);
    if (after < before ||
        after - before != (uint64_t)expected_alloc) {
        errno = EINVAL;
        set_error(error, error_size,
                  "ZFS space-map replay disagrees with its allocation summary");
        goto cleanup;
    }

    result = 0;
cleanup:
    dnode_destroy(&space_map);
    return result;
}

static int load_exact_allocation(ZfsContext *context, ZfsRangeSet *allocated,
                                 char *error, size_t error_size)
{
    ZfsDnode metaslab_array;
    memset(&metaslab_array, 0, sizeof(metaslab_array));
    if (read_object_dnode(context, context->summary.metaslab_array,
                          &metaslab_array, error, error_size) != 0)
        return -1;

    int result = -1;
    if (metaslab_array.type != ZFS_DMU_OT_OBJECT_ARRAY) {
        errno = EINVAL;
        set_error(error, error_size,
                  "ZFS metaslab_array object has unexpected type");
        goto cleanup;
    }

    const uint64_t metaslab_size =
        UINT64_C(1) << context->summary.metaslab_shift;
    const uint64_t metaslab_count =
        context->summary.top_vdev_asize / metaslab_size;
    if (metaslab_count == 0U || metaslab_count > ZFS_MAX_METASLABS) {
        errno = EINVAL;
        set_error(error, error_size, "invalid ZFS metaslab count");
        goto cleanup;
    }

    for (uint64_t index = 0U; index < metaslab_count; ++index) {
        uint8_t raw_object[8];
        LdZfsByteOrder order = metaslab_array.order;
        if (object_read_bytes(context, &metaslab_array,
                              index * 8U, raw_object, sizeof(raw_object),
                              &order, error, error_size) != 0)
            goto cleanup;
        const uint64_t object = load_u64_order(raw_object, order);
        if (object == 0U)
            continue;
        if (replay_space_map(context, index, object, allocated,
                             error, error_size) != 0)
            goto cleanup;
    }

    result = 0;
cleanup:
    dnode_destroy(&metaslab_array);
    return result;
}

static int append_analysis_range(LdZfsAnalysis *analysis,
                                 uint64_t start, uint64_t length,
                                 uint32_t flags)
{
    if (length == 0U)
        return 0;
    if (analysis->range_count == SIZE_MAX / sizeof(*analysis->ranges)) {
        errno = EOVERFLOW;
        return -1;
    }
    const size_t next = analysis->range_count + 1U;
    LdZfsRange *ranges =
        realloc(analysis->ranges, next * sizeof(*ranges));
    if (ranges == NULL)
        return -1;
    analysis->ranges = ranges;
    analysis->ranges[analysis->range_count].start = start;
    analysis->ranges[analysis->range_count].length = length;
    analysis->ranges[analysis->range_count].flags = flags;
    analysis->range_count = next;
    return 0;
}

void zfs_analysis_destroy(LdZfsAnalysis *analysis)
{
    if (analysis == NULL)
        return;
    free(analysis->ranges);
    memset(analysis, 0, sizeof(*analysis));
}

int zfs_analyse_exact(const char *path, LdZfsAnalysis *analysis,
                      char *error, size_t error_size)
{
    if (path == NULL || analysis == NULL) {
        errno = EINVAL;
        set_error(error, error_size, "invalid ZFS exact-analysis request");
        return -1;
    }
    memset(analysis, 0, sizeof(*analysis));

    ZfsContext context;
    if (context_open(path, &context, error, error_size) != 0)
        return -1;

    ZfsRangeSet allocated = {0};
    if (load_exact_allocation(&context, &allocated,
                              error, error_size) != 0) {
        const int saved_errno = errno;
        range_destroy(&allocated);
        context_close(&context);
        errno = saved_errno;
        return -1;
    }

    const uint64_t allocated_bytes = range_total(&allocated);
    if (allocated_bytes > context.summary.top_vdev_asize) {
        range_destroy(&allocated);
        context_close(&context);
        errno = EINVAL;
        set_error(error, error_size,
                  "ZFS allocated bytes exceed top-vdev capacity");
        return -1;
    }

    analysis->size_bytes = context.summary.size_bytes;
    analysis->free_bytes =
        context.summary.top_vdev_asize - allocated_bytes;
    analysis->exact_allocation = true;
    analysis->exact_fragmentation = false;

    if (append_analysis_range(analysis, 0U,
                              ZFS_VDEV_LABEL_START_SIZE,
                              LD_ZFS_RANGE_ALLOCATED |
                              LD_ZFS_RANGE_RESERVED) != 0)
        goto fail;

    for (size_t index = 0U; index < allocated.count; ++index) {
        const uint64_t start =
            ZFS_VDEV_LABEL_START_SIZE + allocated.items[index].start;
        const uint64_t length =
            allocated.items[index].end - allocated.items[index].start;
        if (append_analysis_range(analysis, start, length,
                                  LD_ZFS_RANGE_ALLOCATED) != 0)
            goto fail;
        analysis->allocated_extents++;
    }

    const uint64_t allocatable_end =
        ZFS_VDEV_LABEL_START_SIZE + context.summary.top_vdev_asize;
    if (context.summary.size_bytes < ZFS_VDEV_LABEL_END_SIZE ||
        allocatable_end >
        context.summary.size_bytes - ZFS_VDEV_LABEL_END_SIZE) {
        errno = EINVAL;
        set_error(error, error_size,
                  "ZFS vdev geometry overlaps the end labels");
        goto fail;
    }
    const uint64_t end_labels_start =
        context.summary.size_bytes - ZFS_VDEV_LABEL_END_SIZE;
    if (allocatable_end < end_labels_start &&
        append_analysis_range(analysis, allocatable_end,
                              end_labels_start - allocatable_end,
                              LD_ZFS_RANGE_ALLOCATED |
                              LD_ZFS_RANGE_RESERVED) != 0)
        goto fail;
    if (append_analysis_range(analysis, end_labels_start,
                              ZFS_VDEV_LABEL_END_SIZE,
                              LD_ZFS_RANGE_ALLOCATED |
                              LD_ZFS_RANGE_RESERVED) != 0)
        goto fail;

    analysis->used_bytes = analysis->size_bytes - analysis->free_bytes;
    analysis->unknown_bytes = 0U;
    if (error != NULL && error_size != 0U)
        error[0] = '\0';

    range_destroy(&allocated);
    context_close(&context);
    return 0;

fail: {
        const int saved_errno = errno;
        zfs_analysis_destroy(analysis);
        range_destroy(&allocated);
        context_close(&context);
        errno = saved_errno;
        return -1;
    }
}
