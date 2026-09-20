// SPDX-License-Identifier: GPL-3.0-or-later
#include "apfs_native.h"

#include "ld_device.h"
#include "ld_io.h"
#include "ld_runtime.h"

#include "infiltratr/core.h"
#include "infiltratr/endian.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define APFS_MIN_BLOCK_SIZE 4096U
#define APFS_MAX_BLOCK_SIZE 65536U
#define APFS_OBJ_HEADER 32U
#define APFS_NX_MAGIC_OFFSET 32U
#define APFS_NX_BLOCK_SIZE_OFFSET 36U
#define APFS_NX_BLOCK_COUNT_OFFSET 40U
#define APFS_NX_UUID_OFFSET 72U
#define APFS_NX_DESC_BLOCKS_OFFSET 104U
#define APFS_NX_DESC_BASE_OFFSET 112U
#define APFS_NX_DESC_INDEX_OFFSET 136U
#define APFS_NX_DESC_LEN_OFFSET 140U
#define APFS_NX_SPACEMAN_OID_OFFSET 152U
#define APFS_NX_OMAP_OID_OFFSET 160U
#define APFS_NX_MAX_FILESYSTEMS_OFFSET 180U
#define APFS_NX_FS_OID_OFFSET 184U
#define APFS_NX_MAX_FILESYSTEMS 100U
#define APFS_DESC_NONCONTIGUOUS (UINT64_C(1) << 63U)

#define APFS_OBJ_TYPE_MASK 0x0000ffffU
#define APFS_OBJ_STORAGE_MASK 0xc0000000U
#define APFS_OBJ_PHYSICAL 0x40000000U
#define APFS_OBJ_EPHEMERAL 0x80000000U
#define APFS_OBJECT_TYPE_NX_SUPERBLOCK 1U
#define APFS_OBJECT_TYPE_BTREE 2U
#define APFS_OBJECT_TYPE_BTREE_NODE 3U
#define APFS_OBJECT_TYPE_SPACEMAN 5U
#define APFS_OBJECT_TYPE_SPACEMAN_CIB 7U
#define APFS_OBJECT_TYPE_OMAP 11U
#define APFS_OBJECT_TYPE_CHECKPOINT_MAP 12U
#define APFS_OBJECT_TYPE_FS 13U
#define APFS_OBJECT_TYPE_FSTREE 14U
#define APFS_OBJECT_TYPE_BLOCKREFTREE 15U

#define APFS_BTNODE_ROOT 0x0001U
#define APFS_BTNODE_LEAF 0x0002U
#define APFS_BTNODE_FIXED_KV_SIZE 0x0004U
#define APFS_BTNODE_MASK 0x0007U
#define APFS_BTREE_FOOTER_SIZE 40U
#define APFS_NODE_HEADER_SIZE 56U

#define APFS_OMAP_VAL_DELETED 0x00000001U
#define APFS_OMAP_VAL_ENCRYPTED 0x00000004U
#define APFS_FS_UNENCRYPTED UINT64_C(1)
#define APFS_INCOMPAT_SEALED_VOLUME UINT64_C(0x20)

#define APFS_KEY_ID_MASK UINT64_C(0x0fffffffffffffff)
#define APFS_KEY_TYPE_SHIFT 60U
#define APFS_TYPE_INODE 3U
#define APFS_TYPE_FILE_EXTENT 8U
#define APFS_FILE_EXTENT_LEN_MASK UINT64_C(0x00ffffffffffffff)
#define APFS_INODE_WAS_CLONED UINT64_C(0x10)
#define APFS_INODE_IS_SPARSE UINT64_C(0x200)
#define APFS_INODE_WAS_EVER_CLONED UINT64_C(0x400)

#define APFS_SPACEMAN_MAIN_OFFSET 48U
#define APFS_SPACEMAN_DEVICE_SIZE 48U
#define APFS_SPACEMAN_FLAGS_OFFSET 144U
#define APFS_SPACEMAN_IP_BLOCK_COUNT_OFFSET 152U
#define APFS_SPACEMAN_IP_BASE_OFFSET 176U
#define APFS_SM_FLAG_VERSIONED 0x00000001U
#define APFS_SPACEMAN_MAX_CIBS 65536U
#define APFS_ANALYSIS_MAX_RANGES 2000000U

typedef struct {
    int fd;
    uint64_t bytes;
    uint32_t block_size;
    uint64_t block_count;
} Reader;

typedef struct {
    ApfsRange *items;
    size_t count;
    size_t capacity;
} RangeVec;

typedef struct {
    uint64_t *items;
    size_t count;
    size_t capacity;
} IdVec;

typedef struct {
    uint64_t dstream;
    uint64_t logical;
    uint64_t paddr;
    uint64_t blocks;
} FileExtent;

typedef struct {
    FileExtent *items;
    size_t count;
    size_t capacity;
} ExtentVec;

typedef struct {
    uint8_t *raw;
    uint32_t block_size;
    uint32_t records;
    uint16_t flags;
    uint32_t type;
    uint32_t subtype;
    bool fixed;
} FlatNode;

typedef struct {
    const uint8_t *key;
    uint16_t key_len;
    const uint8_t *value;
    uint16_t value_len;
} FlatRecord;

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    if (error == NULL || error_size == 0U)
        return;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(error, error_size, format, args);
    va_end(args);
}

static bool power_of_two(uint32_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static uint32_t obj_type(const uint8_t *raw)
{
    return infiltratr_load_le32(raw + 24U);
}

static uint64_t obj_oid(const uint8_t *raw)
{
    return infiltratr_load_le64(raw + 8U);
}

static uint64_t obj_xid(const uint8_t *raw)
{
    return infiltratr_load_le64(raw + 16U);
}

static uint64_t fletcher64(const uint8_t *raw, size_t length)
{
    uint64_t sum1 = 0U;
    uint64_t sum2 = 0U;
    const uint64_t mod = UINT64_C(0xffffffff);
    for (size_t offset = 8U; offset + 4U <= length; offset += 4U) {
        sum1 = (sum1 + infiltratr_load_le32(raw + offset)) % mod;
        sum2 = (sum2 + sum1) % mod;
    }
    const uint64_t c1 = mod - ((sum1 + sum2) % mod);
    const uint64_t c2 = mod - ((sum1 + c1) % mod);
    return (c2 << 32U) | c1;
}

static bool checksum_valid(const uint8_t *raw, size_t length)
{
    return length >= 8U && infiltratr_load_le64(raw) == fletcher64(raw, length);
}

static int reader_open(const char *path, Reader *reader,
                       char *error, size_t error_size)
{
    memset(reader, 0, sizeof(*reader));
    reader->fd = -1;
    reader->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (reader->fd < 0) {
        set_error(error, error_size, "open: %s", strerror(errno));
        return -1;
    }
    if (ld_fd_size_bytes(reader->fd, &reader->bytes) != 0) {
        set_error(error, error_size, "cannot determine APFS target size: %s",
                  strerror(errno));
        (void)close(reader->fd);
        reader->fd = -1;
        return -1;
    }
    return 0;
}

static void reader_close(Reader *reader)
{
    if (reader->fd >= 0)
        (void)close(reader->fd);
    reader->fd = -1;
}

static int read_bytes(const Reader *reader, uint64_t offset, void *buffer,
                      size_t length, char *error, size_t error_size)
{
    if (offset > reader->bytes || length > reader->bytes - offset ||
        ld_pread_full(reader->fd, buffer, length, offset) != (ssize_t)length) {
        set_error(error, error_size, "short APFS read at byte %" PRIu64, offset);
        return -1;
    }
    return 0;
}

static int read_block(const Reader *reader, uint64_t block, uint8_t *buffer,
                      char *error, size_t error_size)
{
    if (block >= reader->block_count ||
        block > UINT64_MAX / reader->block_size) {
        set_error(error, error_size, "APFS block address is out of range");
        return -1;
    }
    return read_bytes(reader, block * (uint64_t)reader->block_size,
                      buffer, reader->block_size, error, error_size);
}

static int range_push(RangeVec *ranges, uint64_t start, uint64_t end,
                      char *error, size_t error_size)
{
    if (end <= start)
        return 0;
    if (ranges->count != 0U && start <= ranges->items[ranges->count - 1U].end) {
        if (end > ranges->items[ranges->count - 1U].end)
            ranges->items[ranges->count - 1U].end = end;
        return 0;
    }
    if (ranges->count >= APFS_ANALYSIS_MAX_RANGES) {
        set_error(error, error_size, "APFS allocation map exceeds bounded range count");
        return -1;
    }
    if (!infiltratr_array_reserve((void **)&ranges->items, &ranges->capacity,
                                  sizeof(*ranges->items), ranges->count + 1U, 64U)) {
        set_error(error, error_size, "out of memory storing APFS ranges");
        return -1;
    }
    ranges->items[ranges->count++] = (ApfsRange){start, end};
    return 0;
}

static int id_push(IdVec *ids, uint64_t value, char *error, size_t error_size)
{
    if (!infiltratr_array_reserve((void **)&ids->items, &ids->capacity,
                                  sizeof(*ids->items), ids->count + 1U, 16U)) {
        set_error(error, error_size, "out of memory storing APFS inode identities");
        return -1;
    }
    ids->items[ids->count++] = value;
    return 0;
}

static int extent_push(ExtentVec *extents, FileExtent value,
                       char *error, size_t error_size)
{
    if (!infiltratr_array_reserve((void **)&extents->items, &extents->capacity,
                                  sizeof(*extents->items), extents->count + 1U, 64U)) {
        set_error(error, error_size, "out of memory storing APFS file extents");
        return -1;
    }
    extents->items[extents->count++] = value;
    return 0;
}

static bool block_in_ranges(const RangeVec *ranges, uint64_t block)
{
    size_t left = 0U;
    size_t right = ranges->count;
    while (left < right) {
        const size_t mid = left + (right - left) / 2U;
        const ApfsRange range = ranges->items[mid];
        if (block < range.start)
            right = mid;
        else if (block >= range.end)
            left = mid + 1U;
        else
            return true;
    }
    return false;
}

static int read_initial_geometry(Reader *reader, uint8_t prefix[4096],
                                 char *error, size_t error_size)
{
    if (reader->bytes < APFS_MIN_BLOCK_SIZE ||
        read_bytes(reader, 0U, prefix, APFS_MIN_BLOCK_SIZE,
                   error, error_size) != 0)
        return -1;
    if (memcmp(prefix + APFS_NX_MAGIC_OFFSET, "NXSB", 4U) != 0) {
        set_error(error, error_size, "not an APFS container");
        return -1;
    }
    const uint32_t block_size =
        infiltratr_load_le32(prefix + APFS_NX_BLOCK_SIZE_OFFSET);
    const uint64_t block_count =
        infiltratr_load_le64(prefix + APFS_NX_BLOCK_COUNT_OFFSET);
    if (block_size < APFS_MIN_BLOCK_SIZE || block_size > APFS_MAX_BLOCK_SIZE ||
        !power_of_two(block_size) || block_count == 0U ||
        block_count > UINT64_MAX / block_size ||
        block_count * (uint64_t)block_size > reader->bytes) {
        set_error(error, error_size, "invalid APFS container geometry");
        return -1;
    }
    reader->block_size = block_size;
    reader->block_count = block_count;
    return 0;
}

int apfs_read_summary(const char *path, ApfsSummary *summary,
                      char *error, size_t error_size)
{
    if (path == NULL || summary == NULL) {
        set_error(error, error_size, "invalid APFS summary request");
        return -1;
    }
    Reader reader;
    if (reader_open(path, &reader, error, error_size) != 0)
        return -1;
    uint8_t prefix[4096];
    int result = read_initial_geometry(&reader, prefix, error, error_size);
    if (result == 0) {
        summary->block_size = reader.block_size;
        summary->block_count = reader.block_count;
        memcpy(summary->container_uuid, prefix + APFS_NX_UUID_OFFSET, 16U);
        if (error != NULL && error_size != 0U)
            error[0] = '\0';
    }
    reader_close(&reader);
    return result;
}

static int load_active_nx(Reader *reader, uint8_t **active_out,
                          uint64_t *active_block_out,
                          char *error, size_t error_size)
{
    uint8_t *backup = malloc(reader->block_size);
    uint8_t *candidate = malloc(reader->block_size);
    uint8_t *best = malloc(reader->block_size);
    if (backup == NULL || candidate == NULL || best == NULL) {
        free(backup); free(candidate); free(best);
        set_error(error, error_size, "out of memory loading APFS checkpoint");
        return -1;
    }
    if (read_block(reader, 0U, backup, error, error_size) != 0 ||
        memcmp(backup + APFS_NX_MAGIC_OFFSET, "NXSB", 4U) != 0 ||
        !checksum_valid(backup, reader->block_size)) {
        free(backup); free(candidate); free(best);
        set_error(error, error_size, "APFS block-zero NX superblock checksum is invalid");
        return -1;
    }
    memcpy(best, backup, reader->block_size);
    uint64_t best_block = 0U;
    uint64_t best_xid = obj_xid(backup);
    const uint32_t desc_blocks =
        infiltratr_load_le32(backup + APFS_NX_DESC_BLOCKS_OFFSET);
    const uint64_t desc_base =
        infiltratr_load_le64(backup + APFS_NX_DESC_BASE_OFFSET);
    if (desc_blocks == 0U || desc_blocks > reader->block_count ||
        (desc_base & APFS_DESC_NONCONTIGUOUS) != 0U ||
        desc_base >= reader->block_count ||
        desc_blocks > reader->block_count - desc_base) {
        free(backup); free(candidate); free(best);
        set_error(error, error_size,
                  "APFS exact analysis requires a bounded contiguous checkpoint descriptor ring");
        return -1;
    }
    for (uint32_t index = 0U; index < desc_blocks; ++index) {
        const uint64_t block = desc_base + index;
        if (read_block(reader, block, candidate, error, error_size) != 0) {
            free(backup); free(candidate); free(best);
            return -1;
        }
        if (memcmp(candidate + APFS_NX_MAGIC_OFFSET, "NXSB", 4U) != 0 ||
            !checksum_valid(candidate, reader->block_size))
            continue;
        if (infiltratr_load_le32(candidate + APFS_NX_BLOCK_SIZE_OFFSET) !=
                reader->block_size ||
            infiltratr_load_le64(candidate + APFS_NX_BLOCK_COUNT_OFFSET) !=
                reader->block_count ||
            memcmp(candidate + APFS_NX_UUID_OFFSET,
                   backup + APFS_NX_UUID_OFFSET, 16U) != 0)
            continue;
        const uint64_t xid = obj_xid(candidate);
        if (xid >= best_xid) {
            best_xid = xid;
            best_block = block;
            memcpy(best, candidate, reader->block_size);
        }
    }
    free(backup);
    free(candidate);
    *active_out = best;
    *active_block_out = best_block;
    return 0;
}

static int checkpoint_spaceman(const Reader *reader, const uint8_t *nx,
                               uint64_t active_block,
                               uint64_t *spaceman_block,
                               uint64_t *checkpoint_map_block,
                               char *error, size_t error_size)
{
    const uint32_t desc_blocks =
        infiltratr_load_le32(nx + APFS_NX_DESC_BLOCKS_OFFSET);
    const uint64_t desc_base =
        infiltratr_load_le64(nx + APFS_NX_DESC_BASE_OFFSET);
    const uint32_t desc_index =
        infiltratr_load_le32(nx + APFS_NX_DESC_INDEX_OFFSET);
    const uint32_t desc_len =
        infiltratr_load_le32(nx + APFS_NX_DESC_LEN_OFFSET);
    const uint64_t spaceman_oid =
        infiltratr_load_le64(nx + APFS_NX_SPACEMAN_OID_OFFSET);
    if (desc_blocks == 0U || desc_len < 2U || desc_len > desc_blocks ||
        desc_index >= desc_blocks || spaceman_oid == 0U) {
        set_error(error, error_size, "invalid APFS active checkpoint descriptor state");
        return -1;
    }
    const uint64_t expected_active =
        desc_base + ((uint64_t)desc_index + desc_len - 1U) % desc_blocks;
    if (active_block != expected_active) {
        set_error(error, error_size,
                  "APFS exact analysis requires the selected NX superblock to close the active checkpoint");
        return -1;
    }
    uint8_t *block = malloc(reader->block_size);
    if (block == NULL) {
        set_error(error, error_size, "out of memory reading APFS checkpoint maps");
        return -1;
    }
    bool found = false;
    uint64_t found_block = 0U;
    uint64_t map_block = 0U;
    for (uint32_t index = 0U; index + 1U < desc_len; ++index) {
        const uint64_t paddr =
            desc_base + ((uint64_t)desc_index + index) % desc_blocks;
        if (read_block(reader, paddr, block, error, error_size) != 0) {
            free(block);
            return -1;
        }
        if (!checksum_valid(block, reader->block_size) ||
            (obj_type(block) & APFS_OBJ_TYPE_MASK) != APFS_OBJECT_TYPE_CHECKPOINT_MAP ||
            obj_xid(block) != obj_xid(nx)) {
            free(block);
            set_error(error, error_size, "invalid APFS checkpoint-map object");
            return -1;
        }
        const uint32_t count = infiltratr_load_le32(block + 36U);
        if ((uint64_t)40U + (uint64_t)count * 40U > reader->block_size) {
            free(block);
            set_error(error, error_size, "APFS checkpoint map is truncated");
            return -1;
        }
        for (uint32_t item = 0U; item < count; ++item) {
            const uint8_t *mapping = block + 40U + (size_t)item * 40U;
            const uint32_t type = infiltratr_load_le32(mapping);
            const uint32_t size = infiltratr_load_le32(mapping + 8U);
            const uint64_t oid = infiltratr_load_le64(mapping + 24U);
            const uint64_t paddr_value = infiltratr_load_le64(mapping + 32U);
            if (oid != spaceman_oid)
                continue;
            if (found || (type & APFS_OBJ_TYPE_MASK) != APFS_OBJECT_TYPE_SPACEMAN ||
                (type & APFS_OBJ_STORAGE_MASK) != APFS_OBJ_EPHEMERAL ||
                size != reader->block_size ||
                paddr_value >= reader->block_count) {
                free(block);
                set_error(error, error_size, "invalid APFS spaceman checkpoint mapping");
                return -1;
            }
            found = true;
            found_block = paddr_value;
            map_block = paddr;
        }
    }
    free(block);
    if (!found) {
        set_error(error, error_size, "active APFS checkpoint has no spaceman mapping");
        return -1;
    }
    *spaceman_block = found_block;
    *checkpoint_map_block = map_block;
    return 0;
}

static int scan_spaceman(const Reader *reader, uint64_t spaceman_block,
                         RangeVec *used, uint64_t *free_blocks,
                         char *error, size_t error_size)
{
    uint8_t *sm = malloc(reader->block_size);
    uint8_t *cib = malloc(reader->block_size);
    uint8_t *bitmap = malloc(reader->block_size);
    if (sm == NULL || cib == NULL || bitmap == NULL) {
        free(sm); free(cib); free(bitmap);
        set_error(error, error_size, "out of memory decoding APFS spaceman");
        return -1;
    }
    if (read_block(reader, spaceman_block, sm, error, error_size) != 0 ||
        !checksum_valid(sm, reader->block_size) ||
        (obj_type(sm) & APFS_OBJ_TYPE_MASK) != APFS_OBJECT_TYPE_SPACEMAN ||
        (obj_type(sm) & APFS_OBJ_STORAGE_MASK) != APFS_OBJ_EPHEMERAL) {
        free(sm); free(cib); free(bitmap);
        set_error(error, error_size, "invalid APFS spaceman object");
        return -1;
    }
    const uint32_t sm_block_size = infiltratr_load_le32(sm + 32U);
    const uint32_t blocks_per_chunk = infiltratr_load_le32(sm + 36U);
    const uint32_t chunks_per_cib = infiltratr_load_le32(sm + 40U);
    const uint64_t main_block_count =
        infiltratr_load_le64(sm + APFS_SPACEMAN_MAIN_OFFSET);
    const uint64_t main_chunk_count =
        infiltratr_load_le64(sm + APFS_SPACEMAN_MAIN_OFFSET + 8U);
    const uint32_t cib_count =
        infiltratr_load_le32(sm + APFS_SPACEMAN_MAIN_OFFSET + 16U);
    const uint32_t cab_count =
        infiltratr_load_le32(sm + APFS_SPACEMAN_MAIN_OFFSET + 20U);
    const uint64_t recorded_free =
        infiltratr_load_le64(sm + APFS_SPACEMAN_MAIN_OFFSET + 24U);
    const uint32_t addr_offset =
        infiltratr_load_le32(sm + APFS_SPACEMAN_MAIN_OFFSET + 32U);
    const uint32_t flags =
        infiltratr_load_le32(sm + APFS_SPACEMAN_FLAGS_OFFSET);
    const uint64_t ip_count =
        infiltratr_load_le64(sm + APFS_SPACEMAN_IP_BLOCK_COUNT_OFFSET);
    const uint64_t ip_base =
        infiltratr_load_le64(sm + APFS_SPACEMAN_IP_BASE_OFFSET);

    if (sm_block_size != reader->block_size || blocks_per_chunk == 0U ||
        blocks_per_chunk > reader->block_size * 8U ||
        chunks_per_cib == 0U || cib_count == 0U ||
        cib_count > APFS_SPACEMAN_MAX_CIBS || cab_count != 0U ||
        main_block_count != reader->block_count ||
        main_chunk_count == 0U || main_chunk_count > reader->block_count ||
        (flags & ~APFS_SM_FLAG_VERSIONED) != 0U ||
        ip_base > reader->block_count || ip_count > reader->block_count - ip_base ||
        (uint64_t)addr_offset + (uint64_t)cib_count * 8U > reader->block_size) {
        free(sm); free(cib); free(bitmap);
        set_error(error, error_size,
                  "APFS exact analysis supports direct-CIB single-device spaceman layouts only");
        return -1;
    }

    uint64_t expected_block = 0U;
    uint64_t seen_chunks = 0U;
    uint64_t counted_free = 0U;
    for (uint32_t cib_index = 0U; cib_index < cib_count; ++cib_index) {
        const uint64_t cib_block =
            infiltratr_load_le64(sm + addr_offset + (size_t)cib_index * 8U);
        if (cib_block >= reader->block_count ||
            read_block(reader, cib_block, cib, error, error_size) != 0 ||
            !checksum_valid(cib, reader->block_size) ||
            (obj_type(cib) & APFS_OBJ_TYPE_MASK) != APFS_OBJECT_TYPE_SPACEMAN_CIB) {
            free(sm); free(cib); free(bitmap);
            set_error(error, error_size, "invalid APFS chunk-info block");
            return -1;
        }
        const uint32_t count = infiltratr_load_le32(cib + 36U) & 0x000fffffU;
        if (count == 0U || count > chunks_per_cib ||
            (uint64_t)40U + (uint64_t)count * 32U > reader->block_size) {
            free(sm); free(cib); free(bitmap);
            set_error(error, error_size, "invalid APFS chunk-info count");
            return -1;
        }
        for (uint32_t chunk_index = 0U; chunk_index < count; ++chunk_index) {
            const uint8_t *ci = cib + 40U + (size_t)chunk_index * 32U;
            const uint64_t address = infiltratr_load_le64(ci + 8U);
            const uint32_t block_count = infiltratr_load_le32(ci + 16U);
            const uint32_t ci_free = infiltratr_load_le32(ci + 20U);
            const uint64_t bitmap_block = infiltratr_load_le64(ci + 24U);
            if (address != expected_block || block_count == 0U ||
                block_count > blocks_per_chunk ||
                block_count > reader->block_count - address ||
                ci_free > block_count) {
                free(sm); free(cib); free(bitmap);
                set_error(error, error_size,
                          "APFS chunk-info geometry is incomplete or discontinuous");
                return -1;
            }
            uint32_t raw_free = 0U;
            if (bitmap_block == 0U) {
                if (ci_free != block_count) {
                    free(sm); free(cib); free(bitmap);
                    set_error(error, error_size, "APFS zero-bitmap chunk has inconsistent free count");
                    return -1;
                }
                memset(bitmap, 0, reader->block_size);
            } else {
                if (bitmap_block >= reader->block_count ||
                    read_block(reader, bitmap_block, bitmap,
                               error, error_size) != 0) {
                    free(sm); free(cib); free(bitmap);
                    return -1;
                }
                for (uint32_t bit = 0U; bit < block_count; ++bit)
                    if ((bitmap[bit >> 3U] & (uint8_t)(1U << (bit & 7U))) == 0U)
                        raw_free++;
                if (raw_free != ci_free) {
                    free(sm); free(cib); free(bitmap);
                    set_error(error, error_size,
                              "APFS spaceman bitmap disagrees with chunk free count");
                    return -1;
                }
            }

            uint64_t run_start = UINT64_MAX;
            for (uint32_t bit = 0U; bit < block_count; ++bit) {
                const uint64_t block = address + bit;
                const bool in_ip = block >= ip_base && block - ip_base < ip_count;
                const bool bit_used =
                    (bitmap[bit >> 3U] & (uint8_t)(1U << (bit & 7U))) != 0U;
                const bool is_used = bit_used || in_ip;
                if (!is_used) {
                    counted_free++;
                    if (run_start != UINT64_MAX) {
                        if (range_push(used, run_start, block,
                                       error, error_size) != 0) {
                            free(sm); free(cib); free(bitmap);
                            return -1;
                        }
                        run_start = UINT64_MAX;
                    }
                } else if (run_start == UINT64_MAX) {
                    run_start = block;
                }
            }
            if (run_start != UINT64_MAX &&
                range_push(used, run_start, address + block_count,
                           error, error_size) != 0) {
                free(sm); free(cib); free(bitmap);
                return -1;
            }
            expected_block += block_count;
            seen_chunks++;
        }
    }
    free(sm); free(cib); free(bitmap);
    if (seen_chunks != main_chunk_count || expected_block != reader->block_count ||
        counted_free != recorded_free) {
        set_error(error, error_size,
                  "APFS spaceman does not account for the complete main device");
        return -1;
    }
    *free_blocks = counted_free;
    return 0;
}

static int flat_node_load(const Reader *reader, uint64_t paddr,
                          uint32_t expected_subtype, bool fixed,
                          FlatNode *node, char *error, size_t error_size)
{
    memset(node, 0, sizeof(*node));
    node->raw = malloc(reader->block_size);
    if (node->raw == NULL) {
        set_error(error, error_size, "out of memory reading APFS B-tree node");
        return -1;
    }
    if (read_block(reader, paddr, node->raw, error, error_size) != 0 ||
        !checksum_valid(node->raw, reader->block_size)) {
        free(node->raw); node->raw = NULL;
        set_error(error, error_size, "APFS B-tree node checksum is invalid");
        return -1;
    }
    node->type = obj_type(node->raw);
    node->subtype = infiltratr_load_le32(node->raw + 28U);
    node->flags = infiltratr_load_le16(node->raw + 32U);
    const uint16_t level = infiltratr_load_le16(node->raw + 34U);
    node->records = infiltratr_load_le32(node->raw + 36U);
    node->block_size = reader->block_size;
    node->fixed = fixed;
    const uint16_t table_off = infiltratr_load_le16(node->raw + 40U);
    const uint16_t table_len = infiltratr_load_le16(node->raw + 42U);
    const size_t entry_size = fixed ? 4U : 8U;
    if ((node->type & APFS_OBJ_TYPE_MASK) != APFS_OBJECT_TYPE_BTREE &&
        (node->type & APFS_OBJ_TYPE_MASK) != APFS_OBJECT_TYPE_BTREE_NODE) {
        free(node->raw); node->raw = NULL;
        set_error(error, error_size, "APFS bounded tree root has the wrong object type");
        return -1;
    }
    if (node->subtype != expected_subtype || level != 0U ||
        (node->flags & (APFS_BTNODE_ROOT | APFS_BTNODE_LEAF)) !=
            (APFS_BTNODE_ROOT | APFS_BTNODE_LEAF) ||
        ((node->flags & APFS_BTNODE_FIXED_KV_SIZE) != 0U) != fixed ||
        (node->flags & ~APFS_BTNODE_MASK) != 0U ||
        (uint64_t)APFS_NODE_HEADER_SIZE + table_off + table_len >
            reader->block_size - APFS_BTREE_FOOTER_SIZE ||
        table_len < (uint64_t)node->records * entry_size) {
        free(node->raw); node->raw = NULL;
        set_error(error, error_size,
                  "APFS exact analysis requires a checksum-valid flat root/leaf B-tree");
        return -1;
    }
    return 0;
}

static void flat_node_free(FlatNode *node)
{
    free(node->raw);
    memset(node, 0, sizeof(*node));
}

static int flat_record(const FlatNode *node, uint32_t index,
                       uint16_t fixed_key_len, uint16_t fixed_value_len,
                       FlatRecord *record, char *error, size_t error_size)
{
    if (index >= node->records) {
        set_error(error, error_size, "APFS B-tree record index is out of range");
        return -1;
    }
    const uint16_t table_off = infiltratr_load_le16(node->raw + 40U);
    const uint16_t table_len = infiltratr_load_le16(node->raw + 42U);
    const uint64_t key_base =
        (uint64_t)APFS_NODE_HEADER_SIZE + table_off + table_len;
    const uint64_t value_base =
        (uint64_t)node->block_size - APFS_BTREE_FOOTER_SIZE;
    uint64_t key_offset = 0U, value_offset = 0U;
    uint16_t key_len = 0U, value_len = 0U;
    if (node->fixed) {
        const uint8_t *entry =
            node->raw + APFS_NODE_HEADER_SIZE + table_off + (size_t)index * 4U;
        key_len = fixed_key_len;
        value_len = fixed_value_len;
        key_offset = key_base + infiltratr_load_le16(entry);
        const uint16_t backwards = infiltratr_load_le16(entry + 2U);
        if (backwards > value_base)
            goto invalid;
        value_offset = value_base - backwards;
    } else {
        const uint8_t *entry =
            node->raw + APFS_NODE_HEADER_SIZE + table_off + (size_t)index * 8U;
        const uint16_t key_relative = infiltratr_load_le16(entry);
        key_len = infiltratr_load_le16(entry + 2U);
        const uint16_t value_backwards = infiltratr_load_le16(entry + 4U);
        value_len = infiltratr_load_le16(entry + 6U);
        key_offset = key_base + key_relative;
        if (value_backwards > value_base)
            goto invalid;
        value_offset = value_base - value_backwards;
    }
    if (key_len == 0U || key_offset > node->block_size ||
        key_len > node->block_size - key_offset ||
        value_offset > node->block_size ||
        value_len > node->block_size - value_offset ||
        key_offset + key_len > value_offset)
        goto invalid;
    record->key = node->raw + key_offset;
    record->key_len = key_len;
    record->value = node->raw + value_offset;
    record->value_len = value_len;
    return 0;
invalid:
    set_error(error, error_size, "APFS B-tree record lies outside its node");
    return -1;
}

static int omap_lookup(const Reader *reader, uint64_t omap_block,
                       uint64_t target_oid, uint64_t xid,
                       uint64_t *paddr_out, char *error, size_t error_size)
{
    uint8_t *omap = malloc(reader->block_size);
    if (omap == NULL) {
        set_error(error, error_size, "out of memory reading APFS object map");
        return -1;
    }
    if (read_block(reader, omap_block, omap, error, error_size) != 0 ||
        !checksum_valid(omap, reader->block_size) ||
        (obj_type(omap) & APFS_OBJ_TYPE_MASK) != APFS_OBJECT_TYPE_OMAP) {
        free(omap);
        set_error(error, error_size, "invalid APFS object-map object");
        return -1;
    }
    const uint64_t tree_block = infiltratr_load_le64(omap + 48U);
    free(omap);
    if (tree_block >= reader->block_count) {
        set_error(error, error_size, "APFS object-map root address is invalid");
        return -1;
    }
    FlatNode root;
    if (flat_node_load(reader, tree_block, APFS_OBJECT_TYPE_OMAP, true,
                       &root, error, error_size) != 0)
        return -1;
    bool found = false;
    uint64_t best_xid = 0U;
    uint64_t best_paddr = 0U;
    for (uint32_t index = 0U; index < root.records; ++index) {
        FlatRecord record;
        if (flat_record(&root, index, 16U, 16U, &record,
                        error, error_size) != 0) {
            flat_node_free(&root);
            return -1;
        }
        const uint64_t oid = infiltratr_load_le64(record.key);
        const uint64_t record_xid = infiltratr_load_le64(record.key + 8U);
        if (oid != target_oid || record_xid > xid)
            continue;
        const uint32_t flags = infiltratr_load_le32(record.value);
        const uint32_t size = infiltratr_load_le32(record.value + 4U);
        const uint64_t paddr = infiltratr_load_le64(record.value + 8U);
        if ((flags & (APFS_OMAP_VAL_DELETED | APFS_OMAP_VAL_ENCRYPTED)) != 0U ||
            size != reader->block_size || paddr >= reader->block_count) {
            flat_node_free(&root);
            set_error(error, error_size,
                      "APFS bounded object-map record is deleted, encrypted or malformed");
            return -1;
        }
        if (!found || record_xid >= best_xid) {
            found = true;
            best_xid = record_xid;
            best_paddr = paddr;
        }
    }
    flat_node_free(&root);
    if (!found) {
        set_error(error, error_size, "APFS object map does not contain the required object");
        return -1;
    }
    *paddr_out = best_paddr;
    return 0;
}

static bool id_contains(const IdVec *ids, uint64_t value)
{
    for (size_t index = 0U; index < ids->count; ++index)
        if (ids->items[index] == value)
            return true;
    return false;
}

static int extent_compare(const void *left, const void *right)
{
    const FileExtent *a = left;
    const FileExtent *b = right;
    if (a->dstream < b->dstream) return -1;
    if (a->dstream > b->dstream) return 1;
    if (a->logical < b->logical) return -1;
    if (a->logical > b->logical) return 1;
    return 0;
}

static int scan_catalog(const Reader *reader, uint64_t catalog_block,
                        const RangeVec *used, uint64_t *regular_files,
                        uint64_t *directories, uint64_t *fragmented_files,
                        RangeVec *fragmented_ranges,
                        char *error, size_t error_size)
{
    FlatNode catalog;
    if (flat_node_load(reader, catalog_block, APFS_OBJECT_TYPE_FSTREE, false,
                       &catalog, error, error_size) != 0)
        return -1;
    IdVec regular = {0};
    ExtentVec extents = {0};
    int result = -1;
    for (uint32_t index = 0U; index < catalog.records; ++index) {
        FlatRecord record;
        if (flat_record(&catalog, index, 0U, 0U, &record,
                        error, error_size) != 0)
            goto done;
        if (record.key_len < 8U)
            continue;
        const uint64_t header = infiltratr_load_le64(record.key);
        const uint64_t id = header & APFS_KEY_ID_MASK;
        const uint8_t type = (uint8_t)(header >> APFS_KEY_TYPE_SHIFT);
        if (type == APFS_TYPE_INODE) {
            if (record.value_len < 84U) {
                set_error(error, error_size, "APFS inode record is truncated");
                goto done;
            }
            const uint64_t private_id = infiltratr_load_le64(record.value + 8U);
            const uint64_t internal_flags = infiltratr_load_le64(record.value + 48U);
            const uint16_t mode = infiltratr_load_le16(record.value + 80U);
            const uint16_t kind = mode & 0170000U;
            if (kind == 0100000U) {
                if ((internal_flags & (APFS_INODE_WAS_CLONED |
                                       APFS_INODE_IS_SPARSE |
                                       APFS_INODE_WAS_EVER_CLONED)) != 0U) {
                    set_error(error, error_size,
                              "APFS bounded fragmentation analysis rejects cloned or sparse regular files");
                    goto done;
                }
                if (private_id == 0U || id_contains(&regular, private_id) ||
                    id_push(&regular, private_id, error, error_size) != 0)
                    goto done;
            } else if (kind == 0040000U) {
                (*directories)++;
            }
        } else if (type == APFS_TYPE_FILE_EXTENT) {
            if (record.key_len != 16U || record.value_len != 24U) {
                set_error(error, error_size, "APFS file extent record has invalid size");
                goto done;
            }
            const uint64_t logical = infiltratr_load_le64(record.key + 8U);
            const uint64_t len_flags = infiltratr_load_le64(record.value);
            const uint64_t length = len_flags & APFS_FILE_EXTENT_LEN_MASK;
            const uint8_t flags = (uint8_t)(len_flags >> 56U);
            const uint64_t paddr = infiltratr_load_le64(record.value + 8U);
            const uint64_t crypto_id = infiltratr_load_le64(record.value + 16U);
            if (flags != 0U || length == 0U ||
                length % reader->block_size != 0U ||
                logical % reader->block_size != 0U ||
                paddr == 0U || crypto_id != 0U) {
                set_error(error, error_size,
                          "APFS exact fragmentation analysis supports plain non-sparse extents only");
                goto done;
            }
            const uint64_t blocks = length / reader->block_size;
            if (paddr >= reader->block_count ||
                blocks > reader->block_count - paddr ||
                extent_push(&extents, (FileExtent){id, logical, paddr, blocks},
                            error, error_size) != 0)
                goto done;
        }
    }
    if (extents.count > 1U)
        qsort(extents.items, extents.count, sizeof(*extents.items), extent_compare);
    *regular_files = regular.count;
    for (size_t file = 0U; file < regular.count; ++file) {
        const uint64_t dstream = regular.items[file];
        size_t first = 0U;
        while (first < extents.count && extents.items[first].dstream < dstream)
            first++;
        size_t end = first;
        while (end < extents.count && extents.items[end].dstream == dstream)
            end++;
        if (first == end)
            continue;
        uint64_t logical = 0U;
        uint64_t previous_end = 0U;
        bool fragmented = false;
        for (size_t item = first; item < end; ++item) {
            const FileExtent *extent = &extents.items[item];
            if (extent->logical != logical) {
                set_error(error, error_size,
                          "APFS regular-file extent stream contains a hole or overlap");
                goto done;
            }
            if (item != first && extent->paddr != previous_end)
                fragmented = true;
            for (uint64_t block = 0U; block < extent->blocks; ++block) {
                if (!block_in_ranges(used, extent->paddr + block)) {
                    set_error(error, error_size,
                              "APFS catalog references a block spaceman marks free");
                    goto done;
                }
            }
            logical += extent->blocks * (uint64_t)reader->block_size;
            previous_end = extent->paddr + extent->blocks;
        }
        if (fragmented) {
            (*fragmented_files)++;
            for (size_t item = first; item < end; ++item) {
                if (range_push(fragmented_ranges, extents.items[item].paddr,
                               extents.items[item].paddr + extents.items[item].blocks,
                               error, error_size) != 0)
                    goto done;
            }
        }
    }
    for (size_t item = 0U; item < extents.count; ++item) {
        if (!id_contains(&regular, extents.items[item].dstream)) {
            set_error(error, error_size,
                      "APFS file extent has no matching regular-file data stream");
            goto done;
        }
    }
    result = 0;
done:
    free(regular.items);
    free(extents.items);
    flat_node_free(&catalog);
    return result;
}

int apfs_analyse(const char *path, ApfsAnalysis *analysis,
                 char *error, size_t error_size)
{
    if (path == NULL || analysis == NULL) {
        set_error(error, error_size, "invalid APFS analysis request");
        return -1;
    }
    memset(analysis, 0, sizeof(*analysis));
    Reader reader;
    if (reader_open(path, &reader, error, error_size) != 0)
        return -1;
    uint8_t prefix[4096];
    uint8_t *nx = NULL;
    RangeVec used = {0};
    RangeVec fragmented = {0};
    int result = -1;
    if (read_initial_geometry(&reader, prefix, error, error_size) != 0)
        goto done;
    uint64_t active_block = 0U;
    if (load_active_nx(&reader, &nx, &active_block, error, error_size) != 0)
        goto done;
    if ((obj_type(nx) & APFS_OBJ_TYPE_MASK) != APFS_OBJECT_TYPE_NX_SUPERBLOCK) {
        set_error(error, error_size, "active APFS checkpoint has wrong object type");
        goto done;
    }
    uint64_t spaceman_block = 0U;
    uint64_t checkpoint_map_block = 0U;
    if (checkpoint_spaceman(&reader, nx, active_block, &spaceman_block,
                            &checkpoint_map_block, error, error_size) != 0)
        goto done;
    uint64_t free_blocks = 0U;
    if (scan_spaceman(&reader, spaceman_block, &used, &free_blocks,
                      error, error_size) != 0)
        goto done;

    const uint32_t max_filesystems =
        infiltratr_load_le32(nx + APFS_NX_MAX_FILESYSTEMS_OFFSET);
    if (max_filesystems == 0U || max_filesystems > APFS_NX_MAX_FILESYSTEMS) {
        set_error(error, error_size, "invalid APFS filesystem-slot count");
        goto done;
    }
    uint64_t volume_oid = 0U;
    for (uint32_t index = 0U; index < max_filesystems; ++index) {
        const uint64_t oid =
            infiltratr_load_le64(nx + APFS_NX_FS_OID_OFFSET + (size_t)index * 8U);
        if (oid == 0U)
            continue;
        if (volume_oid != 0U) {
            set_error(error, error_size,
                      "APFS bounded analysis currently supports one volume per container");
            goto done;
        }
        volume_oid = oid;
    }
    if (volume_oid == 0U) {
        set_error(error, error_size, "APFS container has no volume");
        goto done;
    }

    const uint64_t container_omap =
        infiltratr_load_le64(nx + APFS_NX_OMAP_OID_OFFSET);
    uint64_t volume_block = 0U;
    if (container_omap >= reader.block_count ||
        omap_lookup(&reader, container_omap, volume_oid, obj_xid(nx),
                    &volume_block, error, error_size) != 0)
        goto done;
    uint8_t *volume = malloc(reader.block_size);
    if (volume == NULL) {
        set_error(error, error_size, "out of memory reading APFS volume superblock");
        goto done;
    }
    if (read_block(&reader, volume_block, volume, error, error_size) != 0 ||
        !checksum_valid(volume, reader.block_size) ||
        memcmp(volume + 32U, "APSB", 4U) != 0 ||
        obj_oid(volume) != volume_oid ||
        obj_xid(volume) > obj_xid(nx)) {
        free(volume);
        set_error(error, error_size, "invalid APFS volume superblock");
        goto done;
    }
    const uint64_t volume_features = infiltratr_load_le64(volume + 40U);
    const uint64_t volume_rocompat = infiltratr_load_le64(volume + 48U);
    const uint64_t volume_incompat = infiltratr_load_le64(volume + 56U);
    const uint64_t snapshot_count = infiltratr_load_le64(volume + 216U);
    const uint64_t fs_flags = infiltratr_load_le64(volume + 264U);
    const uint64_t volume_omap = infiltratr_load_le64(volume + 128U);
    const uint64_t root_oid = infiltratr_load_le64(volume + 136U);
    if (volume_features != 0U || volume_rocompat != 0U ||
        (volume_incompat & APFS_INCOMPAT_SEALED_VOLUME) != 0U ||
        snapshot_count != 0U || (fs_flags & APFS_FS_UNENCRYPTED) == 0U ||
        volume_omap >= reader.block_count || root_oid == 0U) {
        free(volume);
        set_error(error, error_size,
                  "APFS exact analysis supports one unencrypted, unsealed, snapshot-free bounded volume");
        goto done;
    }
    uint64_t catalog_block = 0U;
    if (omap_lookup(&reader, volume_omap, root_oid, obj_xid(nx),
                    &catalog_block, error, error_size) != 0) {
        free(volume);
        goto done;
    }

    const uint64_t known_blocks[] = {
        0U, active_block, checkpoint_map_block, spaceman_block,
        container_omap, volume_block, volume_omap, catalog_block
    };
    for (size_t index = 0U;
         index < sizeof(known_blocks) / sizeof(known_blocks[0]); ++index) {
        if (!block_in_ranges(&used, known_blocks[index])) {
            free(volume);
            set_error(error, error_size,
                      "APFS spaceman marks required live metadata block %" PRIu64 " free",
                      known_blocks[index]);
            goto done;
        }
    }

    uint64_t regular_files = 0U;
    uint64_t directories = 0U;
    uint64_t fragmented_files = 0U;
    if (scan_catalog(&reader, catalog_block, &used,
                     &regular_files, &directories, &fragmented_files,
                     &fragmented, error, error_size) != 0) {
        free(volume);
        goto done;
    }
    free(volume);

    analysis->block_size = reader.block_size;
    analysis->block_count = reader.block_count;
    memcpy(analysis->container_uuid, nx + APFS_NX_UUID_OFFSET, 16U);
    analysis->xid = obj_xid(nx);
    analysis->active_nx_block = active_block;
    analysis->spaceman_block = spaceman_block;
    analysis->volume_oid = volume_oid;
    analysis->volume_super_block = volume_block;
    analysis->free_blocks = free_blocks;
    analysis->used_blocks = reader.block_count - free_blocks;
    analysis->regular_files = regular_files;
    analysis->directories = directories;
    analysis->fragmented_files = fragmented_files;
    analysis->used_ranges = used.items;
    analysis->used_range_count = used.count;
    analysis->fragmented_ranges = fragmented.items;
    analysis->fragmented_range_count = fragmented.count;
    used.items = NULL;
    fragmented.items = NULL;
    if (error != NULL && error_size != 0U)
        error[0] = '\0';
    result = 0;
done:
    free(nx);
    free(used.items);
    free(fragmented.items);
    reader_close(&reader);
    if (result != 0)
        apfs_analysis_free(analysis);
    return result;
}

void apfs_analysis_free(ApfsAnalysis *analysis)
{
    if (analysis == NULL)
        return;
    free(analysis->used_ranges);
    free(analysis->fragmented_ranges);
    memset(analysis, 0, sizeof(*analysis));
}

bool apfs_probe(const char *path)
{
    ApfsSummary summary;
    return apfs_read_summary(path, &summary, NULL, 0U) == 0;
}
