// SPDX-License-Identifier: GPL-3.0-or-later
#include "zfs_native.h"

#include "ld_io.h"
#include "infiltratr/arithmetic.h"
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

/*
 * Exact-bounded, read-only ZFS allocation/fragmentation engine.
 *
 * zfs_native.c selects and validates the committed leaf-vdev label/uberblock
 * and packed-XDR topology.  This unit consumes that identity, validates every
 * block-pointer form it follows, verifies supported checksums/compression,
 * resolves MOS/dnodes, replays per-metaslab space maps, and walks regular-file
 * block trees.  "Exact" is emitted only when those proofs cover the qualified
 * single-disk topology; unsupported features or transaction state fail closed
 * instead of being approximated.  No source-mutation path belongs in this file.
 */

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
#define ZFS_DMU_OT_OBJECT_DIRECTORY 1U
#define ZFS_DMU_OT_OBJECT_ARRAY 2U
#define ZFS_DMU_OT_SPACE_MAP_HEADER 7U
#define ZFS_DMU_OT_SPACE_MAP 8U
#define ZFS_DMU_OT_OBJSET 11U
#define ZFS_DMU_OT_DSL_DATASET 16U
#define ZFS_DMU_OT_PLAIN_FILE_CONTENTS 19U
#define ZFS_DSL_DATASET_BP_OFFSET 128U
#define ZFS_DSL_DATASET_NUM_CHILDREN_OFFSET 40U
#define ZFS_CHECKSUM_OFF 2U
#define ZFS_CHECKSUM_FLETCHER4 7U
#define ZFS_COMPRESS_OFF 2U
#define ZFS_COMPRESS_LZJB 3U
#define ZFS_COMPRESS_LZ4 15U
#define ZFS_POOL_VERSION_LAST_LEGACY 28U
#define ZFS_POOL_VERSION_FEATURES 5000U
#define ZFS_ZBT_MICRO (UINT64_C(1) << 63U | UINT64_C(3))
#define ZFS_MZAP_HEADER_SIZE 64U
#define ZFS_MZAP_ENTRY_SIZE 64U
#define ZFS_MZAP_NAME_OFFSET 14U
#define ZFS_MZAP_NAME_SIZE 50U
#define ZFS_POOL_DIRECTORY_OBJECT 1U
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
    if (!infiltratr_array_reserve((void **)&set->items, &set->capacity,
                                  sizeof(*set->items), needed, 16U)) {
        errno = ENOMEM;
        return -1;
    }
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

static bool range_contains(const ZfsRangeSet *set,
                           uint64_t start, uint64_t length)
{
    if (length == 0U)
        return true;
    if (start > UINT64_MAX - length)
        return false;
    const uint64_t end = start + length;
    for (size_t index = 0U; index < set->count; ++index) {
        if (set->items[index].start > start)
            return false;
        if (set->items[index].start <= start &&
            set->items[index].end >= end)
            return true;
        if (set->items[index].end > start)
            return false;
    }
    return false;
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

static int microzap_lookup_uint64(ZfsContext *context, uint64_t object,
                                  const char *name, bool *found,
                                  uint64_t *value,
                                  char *error, size_t error_size)
{
    *found = false;
    *value = 0U;

    ZfsDnode zap;
    memset(&zap, 0, sizeof(zap));
    if (read_object_dnode(context, object, &zap, error, error_size) != 0)
        return -1;

    int result = -1;
    if (zap.type != ZFS_DMU_OT_OBJECT_DIRECTORY ||
        zap.maxblkid != 0U || zap.datablkszsec == 0U) {
        errno = ENOTSUP;
        set_error(error, error_size,
                  "ZFS feature/pool directory uses unsupported fat or multi-block ZAP");
        goto cleanup;
    }

    const uint64_t block_size = (uint64_t)zap.datablkszsec << 9U;
    if (block_size < ZFS_MZAP_HEADER_SIZE + ZFS_MZAP_ENTRY_SIZE ||
        block_size > ZFS_MAX_BLOCK_SIZE ||
        block_size > SIZE_MAX ||
        (block_size % ZFS_MZAP_ENTRY_SIZE) != 0U) {
        errno = EINVAL;
        set_error(error, error_size, "invalid ZFS micro-ZAP block size");
        goto cleanup;
    }

    uint8_t *block = malloc((size_t)block_size);
    if (block == NULL)
        goto cleanup;
    LdZfsByteOrder order = zap.order;
    if (object_read_bytes(context, &zap, 0U, block, (size_t)block_size,
                          &order, error, error_size) != 0) {
        free(block);
        goto cleanup;
    }

    if (load_u64_order(block, order) != ZFS_ZBT_MICRO) {
        free(block);
        errno = ENOTSUP;
        set_error(error, error_size,
                  "ZFS feature/pool directory is a fat ZAP outside the bounded exact reader");
        goto cleanup;
    }

    const size_t entries =
        ((size_t)block_size - ZFS_MZAP_HEADER_SIZE) / ZFS_MZAP_ENTRY_SIZE;
    for (size_t index = 0U; index < entries; ++index) {
        const uint8_t *entry =
            block + ZFS_MZAP_HEADER_SIZE + index * ZFS_MZAP_ENTRY_SIZE;
        const char *entry_name =
            (const char *)(entry + ZFS_MZAP_NAME_OFFSET);
        if (entry_name[0] == '\0')
            continue;
        const void *terminator =
            memchr(entry_name, '\0', ZFS_MZAP_NAME_SIZE);
        if (terminator == NULL) {
            free(block);
            errno = EINVAL;
            set_error(error, error_size, "unterminated ZFS micro-ZAP name");
            goto cleanup;
        }
        if (strcmp(entry_name, name) == 0) {
            *value = load_u64_order(entry, order);
            *found = true;
            break;
        }
    }
    free(block);
    result = 0;

cleanup:
    dnode_destroy(&zap);
    return result;
}

static int validate_modern_allocation_features(ZfsContext *context,
                                               char *error,
                                               size_t error_size)
{
    if (context->summary.uberblock_version != ZFS_POOL_VERSION_FEATURES)
        return 0;

    bool found = false;
    uint64_t feature_object = 0U;
    if (microzap_lookup_uint64(context, ZFS_POOL_DIRECTORY_OBJECT,
                               "features_for_write", &found,
                               &feature_object, error, error_size) != 0)
        return -1;
    if (!found || feature_object == 0U) {
        errno = ENOTSUP;
        set_error(error, error_size,
                  "feature-flag ZFS pool has no readable features_for_write directory");
        return -1;
    }

    uint64_t refcount = 0U;
    if (microzap_lookup_uint64(context, feature_object,
                               "com.delphix:log_spacemap", &found,
                               &refcount, error, error_size) != 0)
        return -1;
    if (found && refcount != 0U) {
        errno = ENOTSUP;
        set_error(error, error_size,
                  "active ZFS log_spacemap requires log-space-map replay before allocation can be exact");
        return -1;
    }
    return 0;
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
        (context->summary.uberblock_version > ZFS_POOL_VERSION_LAST_LEGACY &&
         context->summary.uberblock_version != ZFS_POOL_VERSION_FEATURES)) {
        errno = ENOTSUP;
        set_error(error, error_size,
                  "unsupported ZFS pool version for exact native analysis");
        return -1;
    }
    if (context->summary.uberblock_version == ZFS_POOL_VERSION_FEATURES &&
        !context->summary.mos_features_supported) {
        errno = ENOTSUP;
        if (error != NULL && error_size != 0U) {
            (void)snprintf(
                error, error_size,
                "unsupported ZFS MOS feature required for reading: %s",
                context->summary.unsupported_mos_feature[0] != '\0'
                    ? context->summary.unsupported_mos_feature
                    : "unknown");
        }
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
    if (validate_modern_allocation_features(context,
                                            error, error_size) != 0) {
        const int saved_errno = errno;
        dnode_destroy(&context->meta_dnode);
        free(context->mos);
        context->mos = NULL;
        (void)close(context->fd);
        context->fd = -1;
        errno = saved_errno;
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
            if (!range_contains(allocated, start, run)) {
                free(words);
                errno = EINVAL;
                set_error(error, error_size,
                          "ZFS space map frees a range that is not allocated");
                goto cleanup;
            }
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

typedef struct {
    uint64_t block_id;
    uint64_t start;
    uint64_t length;
} ZfsFileExtent;

typedef struct {
    ZfsFileExtent *items;
    size_t count;
    size_t capacity;
} ZfsFileExtentList;

static void file_extent_destroy(ZfsFileExtentList *list)
{
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int file_extent_append(ZfsFileExtentList *list, uint64_t block_id,
                              uint64_t start, uint64_t length)
{
    if (list->count == SIZE_MAX ||
        !infiltratr_array_reserve((void **)&list->items, &list->capacity,
                                  sizeof(*list->items), list->count + 1U, 8U)) {
        errno = ENOMEM;
        return -1;
    }
    list->items[list->count].block_id = block_id;
    list->items[list->count].start = start;
    list->items[list->count].length = length;
    list->count++;
    return 0;
}

static int primary_dva(const ZfsContext *context,
                       const ZfsBlockPointer *bp, ZfsDva *dva)
{
    if (bp->embedded || bp->uses_crypt) {
        errno = ENOTSUP;
        return -1;
    }
    for (size_t index = 0U; index < 3U; ++index) {
        if (bp->dva[index].asize == 0U ||
            bp->dva[index].vdev != context->summary.top_vdev_id)
            continue;
        if (bp->dva[index].gang) {
            errno = ENOTSUP;
            return -1;
        }
        *dva = bp->dva[index];
        return 0;
    }
    errno = EINVAL;
    return -1;
}

static int collect_file_tree(ZfsContext *context,
                             const ZfsBlockPointer *bp,
                             unsigned int level, uint64_t base_block,
                             uint64_t max_block_id,
                             uint64_t entries_per_indirect,
                             ZfsFileExtentList *extents,
                             char *error, size_t error_size)
{
    if (base_block > max_block_id || bp->hole)
        return 0;

    if (level == 0U) {
        ZfsDva dva;
        if (primary_dva(context, bp, &dva) != 0) {
            set_error(error, error_size,
                      "ZFS file extent uses unsupported gang/encrypted/foreign-vdev storage");
            return -1;
        }
        if (dva.offset > context->summary.top_vdev_asize ||
            dva.asize > context->summary.top_vdev_asize - dva.offset) {
            errno = EINVAL;
            set_error(error, error_size,
                      "ZFS file extent escapes the qualified top vdev");
            return -1;
        }
        return file_extent_append(extents, base_block,
                                  dva.offset, dva.asize);
    }

    uint8_t *indirect = NULL;
    size_t indirect_length = 0U;
    if (read_block_pointer_data(context, bp, &indirect, &indirect_length,
                                error, error_size) != 0)
        return -1;
    if (indirect_length == 0U ||
        (indirect_length % ZFS_BP_SIZE) != 0U) {
        free(indirect);
        errno = EINVAL;
        set_error(error, error_size,
                  "invalid ZFS file indirect block");
        return -1;
    }

    uint64_t child_span = 0U;
    if (power_u64(entries_per_indirect, level - 1U, &child_span) != 0) {
        free(indirect);
        return -1;
    }
    const size_t children = indirect_length / ZFS_BP_SIZE;
    for (size_t index = 0U; index < children; ++index) {
        if ((uint64_t)index > UINT64_MAX / child_span) {
            free(indirect);
            errno = EOVERFLOW;
            return -1;
        }
        const uint64_t delta = (uint64_t)index * child_span;
        if (base_block > UINT64_MAX - delta) {
            free(indirect);
            errno = EOVERFLOW;
            return -1;
        }
        const uint64_t child_base = base_block + delta;
        if (child_base > max_block_id)
            break;
        ZfsBlockPointer child;
        decode_block_pointer(indirect + index * ZFS_BP_SIZE,
                             bp->data_order, &child);
        if (collect_file_tree(context, &child, level - 1U,
                              child_base, max_block_id,
                              entries_per_indirect, extents,
                              error, error_size) != 0) {
            free(indirect);
            return -1;
        }
    }
    free(indirect);
    return 0;
}

static int collect_file_extents(ZfsContext *context,
                                const ZfsDnode *dnode,
                                ZfsFileExtentList *extents,
                                char *error, size_t error_size)
{
    if (dnode->nlevels == 0U)
        return 0;
    if (dnode->nlevels > ZFS_DNODE_MAX_LEVELS ||
        dnode->nblkptr == 0U ||
        dnode->indblkshift < ZFS_DNODE_MIN_INDBLKSHIFT ||
        dnode->indblkshift > ZFS_DNODE_MAX_INDBLKSHIFT) {
        errno = ENOTSUP;
        set_error(error, error_size,
                  "unsupported ZFS file dnode geometry");
        return -1;
    }
    const uint64_t entries_per_indirect =
        UINT64_C(1) << (dnode->indblkshift - 7U);
    uint64_t root_span = 0U;
    if (power_u64(entries_per_indirect, dnode->nlevels - 1U,
                  &root_span) != 0)
        return -1;

    for (size_t index = 0U; index < dnode->nblkptr; ++index) {
        if ((uint64_t)index > UINT64_MAX / root_span) {
            errno = EOVERFLOW;
            return -1;
        }
        const uint64_t base = (uint64_t)index * root_span;
        if (base > dnode->maxblkid)
            break;
        ZfsBlockPointer bp;
        if (dnode_bp(dnode, index, &bp) != 0)
            return -1;
        if (collect_file_tree(context, &bp, dnode->nlevels - 1U,
                              base, dnode->maxblkid,
                              entries_per_indirect, extents,
                              error, error_size) != 0)
            return -1;
    }
    return 0;
}

static int analyse_file_dnode(ZfsContext *context, const uint8_t raw[512],
                              LdZfsByteOrder order,
                              ZfsRangeSet *fragmented,
                              LdZfsAnalysis *analysis,
                              char *error, size_t error_size)
{
    uint8_t *copy = malloc(ZFS_DNODE_SIZE);
    if (copy == NULL)
        return -1;
    memcpy(copy, raw, ZFS_DNODE_SIZE);

    ZfsDnode dnode;
    if (dnode_attach_raw(&dnode, copy, ZFS_DNODE_SIZE, order) != 0) {
        free(copy);
        return -1;
    }
    if (dnode.extra_slots != 0U) {
        dnode_destroy(&dnode);
        errno = ENOTSUP;
        set_error(error, error_size,
                  "large ZFS dnodes are outside the bounded exact-fragmentation subset");
        return -1;
    }

    analysis->files_seen++;
    ZfsFileExtentList extents = {0};
    if (collect_file_extents(context, &dnode, &extents,
                             error, error_size) != 0) {
        file_extent_destroy(&extents);
        dnode_destroy(&dnode);
        return -1;
    }

    bool is_fragmented = false;
    for (size_t index = 1U; index < extents.count; ++index) {
        const ZfsFileExtent *previous = &extents.items[index - 1U];
        const ZfsFileExtent *current = &extents.items[index];
        if (current->block_id == previous->block_id + 1U &&
            (previous->start > UINT64_MAX - previous->length ||
             current->start != previous->start + previous->length)) {
            is_fragmented = true;
            break;
        }
    }

    if (is_fragmented) {
        analysis->fragmented_files++;
        for (size_t index = 0U; index < extents.count; ++index) {
            if (range_add(fragmented, extents.items[index].start,
                          extents.items[index].length) != 0) {
                file_extent_destroy(&extents);
                dnode_destroy(&dnode);
                return -1;
            }
        }
    }

    file_extent_destroy(&extents);
    dnode_destroy(&dnode);
    return 0;
}

static int scan_dataset_objset(ZfsContext *context,
                               const ZfsBlockPointer *root,
                               ZfsRangeSet *fragmented,
                               LdZfsAnalysis *analysis,
                               char *error, size_t error_size)
{
    if (root->type != ZFS_DMU_OT_OBJSET || root->level != 0U ||
        root->embedded || root->uses_crypt) {
        errno = ENOTSUP;
        set_error(error, error_size,
                  "unsupported ZFS dataset root block pointer");
        return -1;
    }

    uint8_t *objset = NULL;
    size_t objset_size = 0U;
    if (read_block_pointer_data(context, root, &objset, &objset_size,
                                error, error_size) != 0)
        return -1;
    if (objset_size < ZFS_DNODE_SIZE) {
        free(objset);
        errno = EINVAL;
        set_error(error, error_size, "ZFS dataset objset is too small");
        return -1;
    }

    uint8_t *meta_raw = malloc(ZFS_DNODE_SIZE);
    if (meta_raw == NULL) {
        free(objset);
        return -1;
    }
    memcpy(meta_raw, objset, ZFS_DNODE_SIZE);
    ZfsDnode meta;
    if (dnode_attach_raw(&meta, meta_raw, ZFS_DNODE_SIZE,
                         root->data_order) != 0) {
        free(meta_raw);
        free(objset);
        return -1;
    }
    free(objset);

    if (meta.type != 10U || meta.extra_slots != 0U) {
        dnode_destroy(&meta);
        errno = ENOTSUP;
        set_error(error, error_size,
                  "unsupported ZFS dataset metadnode");
        return -1;
    }
    if (meta.datablkszsec == 0U) {
        dnode_destroy(&meta);
        errno = EINVAL;
        return -1;
    }
    const uint64_t block_size = (uint64_t)meta.datablkszsec << 9U;
    if (block_size == 0U || block_size > ZFS_MAX_BLOCK_SIZE ||
        (block_size % ZFS_DNODE_SIZE) != 0U) {
        dnode_destroy(&meta);
        errno = EINVAL;
        set_error(error, error_size,
                  "invalid ZFS dataset dnode-block size");
        return -1;
    }

    for (uint64_t block_id = 0U; block_id <= meta.maxblkid; ++block_id) {
        ZfsBlockPointer bp;
        if (object_lookup_bp(context, &meta, block_id, &bp,
                             error, error_size) != 0) {
            dnode_destroy(&meta);
            return -1;
        }
        if (bp.hole)
            continue;

        uint8_t *block = NULL;
        size_t block_length = 0U;
        if (read_block_pointer_data(context, &bp, &block, &block_length,
                                    error, error_size) != 0) {
            dnode_destroy(&meta);
            return -1;
        }
        if (block_length != block_size ||
            (block_length % ZFS_DNODE_SIZE) != 0U) {
            free(block);
            dnode_destroy(&meta);
            errno = EINVAL;
            set_error(error, error_size,
                      "invalid ZFS dataset dnode block");
            return -1;
        }

        for (size_t offset = 0U; offset < block_length;
             offset += ZFS_DNODE_SIZE) {
            const uint8_t *raw = block + offset;
            if (raw[0] == 0U)
                continue;
            if (raw[12] != 0U) {
                free(block);
                dnode_destroy(&meta);
                errno = ENOTSUP;
                set_error(error, error_size,
                          "large ZFS dnode encountered in bounded exact subset");
                return -1;
            }
            if (raw[0] == ZFS_DMU_OT_PLAIN_FILE_CONTENTS &&
                analyse_file_dnode(context, raw, bp.data_order,
                                   fragmented, analysis,
                                   error, error_size) != 0) {
                free(block);
                dnode_destroy(&meta);
                return -1;
            }
        }
        free(block);

        if (block_id == UINT64_MAX)
            break;
    }

    dnode_destroy(&meta);
    return 0;
}

static int scan_exact_fragmentation(ZfsContext *context,
                                    ZfsRangeSet *fragmented,
                                    LdZfsAnalysis *analysis,
                                    char *error, size_t error_size)
{
    if (context->meta_dnode.datablkszsec == 0U) {
        errno = EINVAL;
        return -1;
    }
    const uint64_t block_size =
        (uint64_t)context->meta_dnode.datablkszsec << 9U;
    if (block_size == 0U || block_size > ZFS_MAX_BLOCK_SIZE ||
        (block_size % ZFS_DNODE_SIZE) != 0U) {
        errno = EINVAL;
        set_error(error, error_size, "invalid ZFS MOS dnode-block size");
        return -1;
    }

    for (uint64_t block_id = 0U;
         block_id <= context->meta_dnode.maxblkid; ++block_id) {
        ZfsBlockPointer bp;
        if (object_lookup_bp(context, &context->meta_dnode,
                             block_id, &bp, error, error_size) != 0)
            return -1;
        if (bp.hole)
            continue;

        uint8_t *block = NULL;
        size_t block_length = 0U;
        if (read_block_pointer_data(context, &bp, &block, &block_length,
                                    error, error_size) != 0)
            return -1;
        if (block_length != block_size ||
            (block_length % ZFS_DNODE_SIZE) != 0U) {
            free(block);
            errno = EINVAL;
            set_error(error, error_size, "invalid ZFS MOS dnode block");
            return -1;
        }

        for (size_t offset = 0U; offset < block_length;
             offset += ZFS_DNODE_SIZE) {
            const uint8_t *raw = block + offset;
            if (raw[0] == 0U)
                continue;
            if (raw[12] != 0U) {
                free(block);
                errno = ENOTSUP;
                set_error(error, error_size,
                          "large ZFS MOS dnode encountered in bounded exact subset");
                return -1;
            }
            const uint8_t nblkptr = raw[3];
            const size_t bonus_offset =
                ZFS_DNODE_CORE_SIZE + (size_t)nblkptr * ZFS_BP_SIZE;
            if (raw[4] != ZFS_DMU_OT_DSL_DATASET ||
                bonus_offset > ZFS_DNODE_SIZE ||
                ZFS_DSL_DATASET_BP_OFFSET + ZFS_BP_SIZE >
                    ZFS_DNODE_SIZE - bonus_offset)
                continue;

            const uint16_t bonus_length =
                load_u16(raw + 10U, bp.data_order);
            if (bonus_length <
                ZFS_DSL_DATASET_BP_OFFSET + ZFS_BP_SIZE)
                continue;
            const uint8_t *bonus = raw + bonus_offset;
            const uint64_t num_children =
                load_u64_order(
                    bonus + ZFS_DSL_DATASET_NUM_CHILDREN_OFFSET,
                    bp.data_order);
            if (num_children != 0U)
                continue;

            ZfsBlockPointer dataset_root;
            decode_block_pointer(
                bonus + ZFS_DSL_DATASET_BP_OFFSET,
                bp.data_order, &dataset_root);
            if (dataset_root.hole)
                continue;
            if (scan_dataset_objset(context, &dataset_root,
                                    fragmented, analysis,
                                    error, error_size) != 0) {
                free(block);
                return -1;
            }
        }
        free(block);

        if (block_id == UINT64_MAX)
            break;
    }
    return 0;
}

static int append_analysis_range(LdZfsAnalysis *analysis,
                                 uint64_t start, uint64_t length,
                                 uint32_t flags)
{
    if (length == 0U)
        return 0;
    if (analysis->range_count == SIZE_MAX ||
        !infiltratr_array_reserve((void **)&analysis->ranges,
                                  &analysis->range_capacity,
                                  sizeof(*analysis->ranges),
                                  analysis->range_count + 1U, 64U)) {
        errno = ENOMEM;
        return -1;
    }
    analysis->ranges[analysis->range_count].start = start;
    analysis->ranges[analysis->range_count].length = length;
    analysis->ranges[analysis->range_count].flags = flags;
    analysis->range_count++;
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
    ZfsRangeSet fragmented = {0};
    if (load_exact_allocation(&context, &allocated,
                              error, error_size) != 0) {
        const int saved_errno = errno;
        range_destroy(&allocated);
        range_destroy(&fragmented);
        context_close(&context);
        errno = saved_errno;
        return -1;
    }
    if (scan_exact_fragmentation(&context, &fragmented, analysis,
                                 error, error_size) != 0) {
        const int saved_errno = errno;
        range_destroy(&allocated);
        range_destroy(&fragmented);
        context_close(&context);
        errno = saved_errno;
        return -1;
    }

    const uint64_t allocated_bytes = range_total(&allocated);
    if (allocated_bytes > context.summary.top_vdev_asize) {
        range_destroy(&allocated);
        range_destroy(&fragmented);
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
    analysis->exact_fragmentation = true;
    analysis->fragmented_bytes = range_total(&fragmented);

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

    for (size_t index = 0U; index < fragmented.count; ++index) {
        if (append_analysis_range(
                analysis,
                ZFS_VDEV_LABEL_START_SIZE + fragmented.items[index].start,
                fragmented.items[index].end - fragmented.items[index].start,
                LD_ZFS_RANGE_FRAGMENTED) != 0)
            goto fail;
    }

    analysis->used_bytes = analysis->size_bytes - analysis->free_bytes;
    analysis->unknown_bytes = 0U;
    if (error != NULL && error_size != 0U)
        error[0] = '\0';

    range_destroy(&allocated);
    range_destroy(&fragmented);
    context_close(&context);
    return 0;

fail: {
        const int saved_errno = errno;
        zfs_analysis_destroy(analysis);
        range_destroy(&allocated);
        range_destroy(&fragmented);
        context_close(&context);
        errno = saved_errno;
        return -1;
    }
}
