// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * First-party deterministic APFS Test Media creator.
 *
 * This is deliberately independent fixture-construction code. It manufactures
 * the exact bounded on-disk subset qualified by the production APFS analyser
 * and writer: one active checkpoint, one direct spaceman CIB/bitmap, one
 * unencrypted snapshot-free volume, flat object maps, flat catalog and flat
 * extent-reference tree. A two-block regular file is intentionally fragmented.
 */
#include "test_media.h"
#include "apfs_native.h"

#include "infiltratr/endian.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define APFS_TM_BLOCK 4096U
#define APFS_TM_MAX_BYTES (UINT64_C(2) * LDTM_GIB)
#define APFS_TM_MAX_BLOCKS (APFS_TM_MAX_BYTES / APFS_TM_BLOCK)
#define APFS_TM_BLOCKS_PER_CHUNK UINT32_C(32768)
#define APFS_TM_XID UINT64_C(7)
#define APFS_TM_CPM UINT64_C(1)
#define APFS_TM_NX UINT64_C(2)
#define APFS_TM_SPACEMAN UINT64_C(9)
#define APFS_TM_CIB UINT64_C(32)
#define APFS_TM_BITMAP UINT64_C(33)
#define APFS_TM_CONTAINER_OMAP UINT64_C(40)
#define APFS_TM_CONTAINER_OMAP_TREE UINT64_C(41)
#define APFS_TM_VOLUME UINT64_C(42)
#define APFS_TM_VOLUME_OMAP UINT64_C(43)
#define APFS_TM_VOLUME_OMAP_TREE UINT64_C(44)
#define APFS_TM_EXTREF UINT64_C(45)
#define APFS_TM_CATALOG UINT64_C(46)
#define APFS_TM_DATA_A UINT64_C(512)
#define APFS_TM_DATA_B UINT64_C(520)
#define APFS_TM_FS_OID UINT64_C(200)
#define APFS_TM_CATALOG_OID UINT64_C(300)
#define APFS_TM_DSTREAM UINT64_C(500)
#define APFS_TM_SPACEMAN_OID UINT64_C(100)

#define APFS_TM_OBJ_PHYSICAL UINT32_C(0x40000000)
#define APFS_TM_OBJ_EPHEMERAL UINT32_C(0x80000000)
#define APFS_TM_TYPE_BTREE UINT32_C(2)
#define APFS_TM_TYPE_SPACEMAN UINT32_C(5)
#define APFS_TM_TYPE_CIB UINT32_C(7)
#define APFS_TM_TYPE_OMAP UINT32_C(11)
#define APFS_TM_TYPE_CPM UINT32_C(12)
#define APFS_TM_TYPE_FS UINT32_C(13)
#define APFS_TM_TYPE_FSTREE UINT32_C(14)
#define APFS_TM_TYPE_BLOCKREFTREE UINT32_C(15)
#define APFS_TM_NODE_ROOT UINT16_C(0x0001)
#define APFS_TM_NODE_LEAF UINT16_C(0x0002)
#define APFS_TM_NODE_FIXED UINT16_C(0x0004)
#define APFS_TM_KEY_TYPE_SHIFT 60U
#define APFS_TM_PEXT_KIND_SHIFT 60U

typedef struct {
    const uint8_t *key;
    uint16_t key_len;
    const uint8_t *value;
    uint16_t value_len;
} ApfsTmRecord;

static const uint8_t APFS_TM_FSID[16] = {
    0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
    0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
};

static const uint8_t APFS_TM_VOL_UUID[16] = {
    0xff,0xee,0xdd,0xcc,0xbb,0xaa,0x99,0x88,
    0x77,0x66,0x55,0x44,0x33,0x22,0x11,0x00
};

static uint64_t apfs_tm_fletcher64(const uint8_t *raw, size_t length)
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

static void apfs_tm_checksum(uint8_t block[APFS_TM_BLOCK])
{
    infiltratr_store_le64(block, apfs_tm_fletcher64(block, APFS_TM_BLOCK));
}

static void apfs_tm_object_header(uint8_t *raw, uint64_t oid,
                                  uint32_t type, uint32_t subtype)
{
    infiltratr_store_le64(raw + 8U, oid);
    infiltratr_store_le64(raw + 16U, APFS_TM_XID);
    infiltratr_store_le32(raw + 24U, type);
    infiltratr_store_le32(raw + 28U, subtype);
}

static int apfs_tm_pwrite_full(int fd, const void *buffer,
                               size_t length, uint64_t offset)
{
    const uint8_t *bytes = buffer;
    size_t done = 0U;
    while (done < length) {
        const ssize_t count =
            pwrite(fd, bytes + done, length - done,
                   (off_t)(offset + done));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (count == 0)
            return -1;
        done += (size_t)count;
    }
    return 0;
}

static int apfs_tm_pread_full(int fd, void *buffer,
                              size_t length, uint64_t offset)
{
    uint8_t *bytes = buffer;
    size_t done = 0U;
    while (done < length) {
        const ssize_t count =
            pread(fd, bytes + done, length - done,
                  (off_t)(offset + done));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (count == 0)
            return -1;
        done += (size_t)count;
    }
    return 0;
}

static int apfs_tm_write_block(int fd, uint64_t block,
                               const uint8_t raw[APFS_TM_BLOCK])
{
    return apfs_tm_pwrite_full(
        fd, raw, APFS_TM_BLOCK,
        block * (uint64_t)APFS_TM_BLOCK);
}

static uint64_t apfs_tm_key_header(uint64_t object,
                                   uint8_t type)
{
    return object | ((uint64_t)type << APFS_TM_KEY_TYPE_SHIFT);
}

static int apfs_tm_build_node(uint8_t raw[APFS_TM_BLOCK],
                              uint64_t oid, uint32_t object_type,
                              uint32_t subtype,
                              const ApfsTmRecord *records,
                              uint32_t record_count, int fixed)
{
    memset(raw, 0, APFS_TM_BLOCK);
    apfs_tm_object_header(raw, oid, object_type, subtype);
    infiltratr_store_le16(
        raw + 32U,
        (uint16_t)(APFS_TM_NODE_ROOT | APFS_TM_NODE_LEAF |
                   (fixed ? APFS_TM_NODE_FIXED : 0U)));
    infiltratr_store_le16(raw + 34U, 0U);
    infiltratr_store_le32(raw + 36U, record_count);

    const size_t entry_size = fixed ? 4U : 8U;
    if ((uint64_t)record_count * entry_size > UINT16_MAX)
        return -1;
    const uint16_t table_len =
        (uint16_t)((size_t)record_count * entry_size);
    infiltratr_store_le16(raw + 40U, 0U);
    infiltratr_store_le16(raw + 42U, table_len);

    const size_t key_base = 56U + table_len;
    const size_t value_base = APFS_TM_BLOCK - 40U;
    size_t key_cursor = key_base;
    size_t value_cursor = value_base;
    uint32_t longest_key = 0U;
    uint32_t longest_value = 0U;

    for (uint32_t index = 0U; index < record_count; ++index) {
        const ApfsTmRecord *record = &records[index];
        if (record->key_len == 0U ||
            record->value_len > value_cursor ||
            value_cursor - record->value_len < key_cursor ||
            record->key_len >
                value_cursor - record->value_len - key_cursor)
            return -1;
        value_cursor -= record->value_len;
        memcpy(raw + key_cursor, record->key, record->key_len);
        memcpy(raw + value_cursor, record->value, record->value_len);
        const size_t entry = 56U + (size_t)index * entry_size;
        infiltratr_store_le16(
            raw + entry, (uint16_t)(key_cursor - key_base));
        if (fixed) {
            infiltratr_store_le16(
                raw + entry + 2U,
                (uint16_t)(value_base - value_cursor));
        } else {
            infiltratr_store_le16(
                raw + entry + 2U, record->key_len);
            infiltratr_store_le16(
                raw + entry + 4U,
                (uint16_t)(value_base - value_cursor));
            infiltratr_store_le16(
                raw + entry + 6U, record->value_len);
        }
        key_cursor += record->key_len;
        if (record->key_len > longest_key)
            longest_key = record->key_len;
        if (record->value_len > longest_value)
            longest_value = record->value_len;
    }

    if (key_cursor > value_cursor ||
        key_cursor - key_base > UINT16_MAX ||
        value_cursor - key_cursor > UINT16_MAX)
        return -1;
    infiltratr_store_le16(
        raw + 44U, (uint16_t)(key_cursor - key_base));
    infiltratr_store_le16(
        raw + 46U, (uint16_t)(value_cursor - key_cursor));
    infiltratr_store_le16(raw + 48U, UINT16_C(0xffff));
    infiltratr_store_le16(raw + 50U, 0U);
    infiltratr_store_le16(raw + 52U, UINT16_C(0xffff));
    infiltratr_store_le16(raw + 54U, 0U);

    const size_t footer = APFS_TM_BLOCK - 40U;
    infiltratr_store_le32(raw + footer,
                          object_type == APFS_TM_TYPE_BTREE ? 0U : 0x10U);
    infiltratr_store_le32(raw + footer + 4U, APFS_TM_BLOCK);
    infiltratr_store_le32(raw + footer + 8U, fixed ? 16U : 0U);
    infiltratr_store_le32(raw + footer + 12U, fixed ? 16U : 0U);
    infiltratr_store_le32(raw + footer + 16U, longest_key);
    infiltratr_store_le32(raw + footer + 20U, longest_value);
    infiltratr_store_le64(raw + footer + 24U, record_count);
    infiltratr_store_le64(raw + footer + 32U, 1U);
    apfs_tm_checksum(raw);
    return 0;
}

static void apfs_tm_make_omap_object(
    uint8_t raw[APFS_TM_BLOCK],
    uint64_t paddr, uint64_t tree)
{
    memset(raw, 0, APFS_TM_BLOCK);
    apfs_tm_object_header(
        raw, paddr,
        APFS_TM_OBJ_PHYSICAL | APFS_TM_TYPE_OMAP, 0U);
    infiltratr_store_le64(raw + 48U, tree);
    apfs_tm_checksum(raw);
}

static int apfs_tm_geometry(int fd, uint64_t *block_count)
{
    if (block_count == NULL)
        return -1;
    const off_t end = lseek(fd, 0, SEEK_END);
    if (end <= 0)
        return -1;
    uint64_t bytes = (uint64_t)end;
    if (bytes > APFS_TM_MAX_BYTES)
        bytes = APFS_TM_MAX_BYTES;
    if (bytes % APFS_TM_BLOCK != 0U)
        return -1;
    const uint64_t blocks = bytes / APFS_TM_BLOCK;
    if (blocks <= APFS_TM_DATA_B || blocks > APFS_TM_MAX_BLOCKS)
        return -1;
    const uint64_t chunks =
        (blocks + APFS_TM_BLOCKS_PER_CHUNK - 1U) /
        APFS_TM_BLOCKS_PER_CHUNK;
    if (chunks == 0U || chunks > 126U)
        return -1;
    *block_count = blocks;
    return 0;
}

static int apfs_tm_expected_blocks(const char *path, uint64_t *block_count)
{
    int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = open(path, flags);
    if (fd < 0)
        return -1;
    const int result = apfs_tm_geometry(fd, block_count);
    (void)close(fd);
    return result;
}

int ldtm_format_apfs_volume(const char *path)
{
    if (path == NULL || *path == '\0')
        return -1;
    int flags = O_RDWR | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = open(path, flags);
    if (fd < 0)
        return -1;
    struct stat status;
    uint64_t total_blocks = 0U;
    if (fstat(fd, &status) != 0 ||
        (!S_ISREG(status.st_mode) && !S_ISBLK(status.st_mode)) ||
        apfs_tm_geometry(fd, &total_blocks) != 0) {
        (void)close(fd);
        return -1;
    }
    const uint32_t chunk_count = (uint32_t)(
        (total_blocks + APFS_TM_BLOCKS_PER_CHUNK - 1U) /
        APFS_TM_BLOCKS_PER_CHUNK);

    /*
     * The fixture owns only its active checkpoint and referenced objects.
     * Clear the unused descriptor-ring blocks so stale bytes from an older
     * APFS container cannot be mistaken for historical checkpoints, without
     * pointlessly streaming zeroes across the complete 2 GiB qualification
     * volume.
     */
    uint8_t raw[APFS_TM_BLOCK];
    memset(raw, 0, sizeof(raw));
    for (uint64_t block = 3U; block < 9U; ++block) {
        if (apfs_tm_write_block(fd, block, raw) != 0)
            goto fail;
    }

    /* NX block zero and the sole active NX checkpoint object. */
    memset(raw, 0, sizeof(raw));
    apfs_tm_object_header(raw, 1U,
                          APFS_TM_OBJ_PHYSICAL | 1U, 0U);
    memcpy(raw + 32U, "NXSB", 4U);
    infiltratr_store_le32(raw + 36U, APFS_TM_BLOCK);
    infiltratr_store_le64(raw + 40U, total_blocks);
    memcpy(raw + 72U, APFS_TM_FSID, sizeof(APFS_TM_FSID));
    infiltratr_store_le64(raw + 96U, APFS_TM_XID + 1U);
    infiltratr_store_le32(raw + 104U, 8U);
    infiltratr_store_le32(raw + 108U, 8U);
    infiltratr_store_le64(raw + 112U, 1U);
    infiltratr_store_le64(raw + 120U, APFS_TM_SPACEMAN);
    infiltratr_store_le32(raw + 136U, 0U);
    infiltratr_store_le32(raw + 140U, 2U);
    infiltratr_store_le64(raw + 152U, APFS_TM_SPACEMAN_OID);
    infiltratr_store_le64(raw + 160U, APFS_TM_CONTAINER_OMAP);
    infiltratr_store_le32(raw + 180U, 100U);
    infiltratr_store_le64(raw + 184U, APFS_TM_FS_OID);
    apfs_tm_checksum(raw);
    if (apfs_tm_write_block(fd, 0U, raw) != 0 ||
        apfs_tm_write_block(fd, APFS_TM_NX, raw) != 0)
        goto fail;

    /* Active checkpoint map: ephemeral spaceman oid -> block 9. */
    memset(raw, 0, sizeof(raw));
    apfs_tm_object_header(
        raw, APFS_TM_CPM,
        APFS_TM_OBJ_PHYSICAL | APFS_TM_TYPE_CPM, 0U);
    infiltratr_store_le32(raw + 32U, 1U);
    infiltratr_store_le32(raw + 36U, 1U);
    infiltratr_store_le32(
        raw + 40U,
        APFS_TM_OBJ_EPHEMERAL | APFS_TM_TYPE_SPACEMAN);
    infiltratr_store_le32(raw + 48U, APFS_TM_BLOCK);
    infiltratr_store_le64(raw + 64U, APFS_TM_SPACEMAN_OID);
    infiltratr_store_le64(raw + 72U, APFS_TM_SPACEMAN);
    apfs_tm_checksum(raw);
    if (apfs_tm_write_block(fd, APFS_TM_CPM, raw) != 0)
        goto fail;

    /* Spaceman: one direct CIB describes every chunk up to the 2 GiB cap. */
    const uint64_t used_count = 15U;
    const uint64_t free_count = total_blocks - used_count;
    memset(raw, 0, sizeof(raw));
    apfs_tm_object_header(
        raw, APFS_TM_SPACEMAN_OID,
        APFS_TM_OBJ_EPHEMERAL | APFS_TM_TYPE_SPACEMAN, 0U);
    infiltratr_store_le32(raw + 32U, APFS_TM_BLOCK);
    infiltratr_store_le32(raw + 36U, APFS_TM_BLOCKS_PER_CHUNK);
    infiltratr_store_le32(raw + 40U, 126U);
    infiltratr_store_le64(raw + 48U, total_blocks);
    infiltratr_store_le64(raw + 56U, chunk_count);
    infiltratr_store_le32(raw + 64U, 1U);
    infiltratr_store_le32(raw + 68U, 0U);
    infiltratr_store_le64(raw + 72U, free_count);
    infiltratr_store_le32(raw + 80U, 1024U);
    infiltratr_store_le64(raw + 1024U, APFS_TM_CIB);
    apfs_tm_checksum(raw);
    if (apfs_tm_write_block(fd, APFS_TM_SPACEMAN, raw) != 0)
        goto fail;

    memset(raw, 0, sizeof(raw));
    const uint64_t used_blocks[] = {
        0U, APFS_TM_CPM, APFS_TM_NX, APFS_TM_SPACEMAN,
        APFS_TM_CIB, APFS_TM_BITMAP,
        APFS_TM_CONTAINER_OMAP, APFS_TM_CONTAINER_OMAP_TREE,
        APFS_TM_VOLUME, APFS_TM_VOLUME_OMAP,
        APFS_TM_VOLUME_OMAP_TREE, APFS_TM_EXTREF,
        APFS_TM_CATALOG, APFS_TM_DATA_A, APFS_TM_DATA_B
    };
    for (size_t index = 0U;
         index < sizeof(used_blocks) / sizeof(used_blocks[0]);
         ++index) {
        const uint64_t block = used_blocks[index];
        raw[block >> 3U] |=
            (uint8_t)(1U << (unsigned int)(block & 7U));
    }
    if (apfs_tm_write_block(fd, APFS_TM_BITMAP, raw) != 0)
        goto fail;

    memset(raw, 0, sizeof(raw));
    apfs_tm_object_header(
        raw, APFS_TM_CIB,
        APFS_TM_OBJ_PHYSICAL | APFS_TM_TYPE_CIB, 0U);
    infiltratr_store_le32(raw + 32U, 0U);
    infiltratr_store_le32(raw + 36U, chunk_count);
    for (uint32_t chunk = 0U; chunk < chunk_count; ++chunk) {
        uint8_t *record = raw + 40U + (size_t)chunk * 32U;
        const uint64_t address =
            (uint64_t)chunk * APFS_TM_BLOCKS_PER_CHUNK;
        const uint64_t remaining = total_blocks - address;
        const uint32_t block_count =
            remaining > APFS_TM_BLOCKS_PER_CHUNK
                ? APFS_TM_BLOCKS_PER_CHUNK : (uint32_t)remaining;
        const uint32_t chunk_used =
            chunk == 0U ? (uint32_t)used_count : 0U;
        infiltratr_store_le64(record, APFS_TM_XID);
        infiltratr_store_le64(record + 8U, address);
        infiltratr_store_le32(record + 16U, block_count);
        infiltratr_store_le32(
            record + 20U, block_count - chunk_used);
        infiltratr_store_le64(
            record + 24U, chunk == 0U ? APFS_TM_BITMAP : 0U);
    }
    apfs_tm_checksum(raw);
    if (apfs_tm_write_block(fd, APFS_TM_CIB, raw) != 0)
        goto fail;

    /* Container object map and its flat root. */
    apfs_tm_make_omap_object(
        raw, APFS_TM_CONTAINER_OMAP,
        APFS_TM_CONTAINER_OMAP_TREE);
    if (apfs_tm_write_block(fd, APFS_TM_CONTAINER_OMAP, raw) != 0)
        goto fail;
    uint8_t omap_key[16];
    uint8_t omap_value[16];
    infiltratr_store_le64(omap_key, APFS_TM_FS_OID);
    infiltratr_store_le64(omap_key + 8U, APFS_TM_XID);
    infiltratr_store_le32(omap_value, 0U);
    infiltratr_store_le32(omap_value + 4U, APFS_TM_BLOCK);
    infiltratr_store_le64(omap_value + 8U, APFS_TM_VOLUME);
    const ApfsTmRecord container_omap_record = {
        omap_key, 16U, omap_value, 16U
    };
    if (apfs_tm_build_node(
            raw, APFS_TM_CONTAINER_OMAP_TREE,
            APFS_TM_OBJ_PHYSICAL | APFS_TM_TYPE_BTREE,
            APFS_TM_TYPE_OMAP,
            &container_omap_record, 1U, 1) != 0 ||
        apfs_tm_write_block(
            fd, APFS_TM_CONTAINER_OMAP_TREE, raw) != 0)
        goto fail;

    /* Volume superblock. */
    memset(raw, 0, sizeof(raw));
    apfs_tm_object_header(raw, APFS_TM_FS_OID,
                          APFS_TM_TYPE_FS, 0U);
    memcpy(raw + 32U, "APSB", 4U);
    infiltratr_store_le64(raw + 128U, APFS_TM_VOLUME_OMAP);
    infiltratr_store_le64(raw + 136U, APFS_TM_CATALOG_OID);
    infiltratr_store_le64(raw + 144U, APFS_TM_EXTREF);
    infiltratr_store_le64(raw + 184U, 1U);
    infiltratr_store_le64(raw + 192U, 1U);
    memcpy(raw + 240U, APFS_TM_VOL_UUID, sizeof(APFS_TM_VOL_UUID));
    infiltratr_store_le64(raw + 264U, 1U);
    memcpy(raw + 704U, "LD_APFS", 8U);
    apfs_tm_checksum(raw);
    if (apfs_tm_write_block(fd, APFS_TM_VOLUME, raw) != 0)
        goto fail;

    /* Volume object map. */
    apfs_tm_make_omap_object(
        raw, APFS_TM_VOLUME_OMAP,
        APFS_TM_VOLUME_OMAP_TREE);
    if (apfs_tm_write_block(fd, APFS_TM_VOLUME_OMAP, raw) != 0)
        goto fail;
    infiltratr_store_le64(omap_key, APFS_TM_CATALOG_OID);
    infiltratr_store_le64(omap_key + 8U, APFS_TM_XID);
    infiltratr_store_le32(omap_value, 0U);
    infiltratr_store_le32(omap_value + 4U, APFS_TM_BLOCK);
    infiltratr_store_le64(omap_value + 8U, APFS_TM_CATALOG);
    const ApfsTmRecord volume_omap_record = {
        omap_key, 16U, omap_value, 16U
    };
    if (apfs_tm_build_node(
            raw, APFS_TM_VOLUME_OMAP_TREE,
            APFS_TM_OBJ_PHYSICAL | APFS_TM_TYPE_BTREE,
            APFS_TM_TYPE_OMAP,
            &volume_omap_record, 1U, 1) != 0 ||
        apfs_tm_write_block(
            fd, APFS_TM_VOLUME_OMAP_TREE, raw) != 0)
        goto fail;

    /* Catalog: root directory + one regular dstream with two fragments. */
    uint8_t inode_root_key[8], inode_file_key[8];
    uint8_t inode_root_value[92] = {0};
    uint8_t inode_file_value[92] = {0};
    uint8_t extent_key_a[16], extent_key_b[16];
    uint8_t extent_value_a[24] = {0};
    uint8_t extent_value_b[24] = {0};
    infiltratr_store_le64(
        inode_root_key, apfs_tm_key_header(2U, 3U));
    infiltratr_store_le64(
        inode_file_key, apfs_tm_key_header(16U, 3U));
    infiltratr_store_le64(inode_root_value + 8U, 2U);
    infiltratr_store_le16(inode_root_value + 80U, 0040755U);
    infiltratr_store_le64(
        inode_file_value + 8U, APFS_TM_DSTREAM);
    infiltratr_store_le16(inode_file_value + 80U, 0100644U);
    infiltratr_store_le64(
        extent_key_a,
        apfs_tm_key_header(APFS_TM_DSTREAM, 8U));
    infiltratr_store_le64(extent_key_a + 8U, 0U);
    infiltratr_store_le64(
        extent_key_b,
        apfs_tm_key_header(APFS_TM_DSTREAM, 8U));
    infiltratr_store_le64(extent_key_b + 8U, APFS_TM_BLOCK);
    infiltratr_store_le64(extent_value_a, APFS_TM_BLOCK);
    infiltratr_store_le64(extent_value_a + 8U, APFS_TM_DATA_A);
    infiltratr_store_le64(extent_value_b, APFS_TM_BLOCK);
    infiltratr_store_le64(extent_value_b + 8U, APFS_TM_DATA_B);
    const ApfsTmRecord catalog_records[] = {
        {inode_root_key, 8U, inode_root_value, 92U},
        {inode_file_key, 8U, inode_file_value, 92U},
        {extent_key_a, 16U, extent_value_a, 24U},
        {extent_key_b, 16U, extent_value_b, 24U},
    };
    if (apfs_tm_build_node(
            raw, APFS_TM_CATALOG_OID,
            APFS_TM_TYPE_BTREE, APFS_TM_TYPE_FSTREE,
            catalog_records, 4U, 0) != 0 ||
        apfs_tm_write_block(fd, APFS_TM_CATALOG, raw) != 0)
        goto fail;

    /* Physical extent-reference root. */
    uint8_t pext_key_a[8], pext_key_b[8];
    uint8_t pext_value_a[20] = {0};
    uint8_t pext_value_b[20] = {0};
    infiltratr_store_le64(
        pext_key_a,
        apfs_tm_key_header(APFS_TM_DATA_A, 2U));
    infiltratr_store_le64(
        pext_key_b,
        apfs_tm_key_header(APFS_TM_DATA_B, 2U));
    infiltratr_store_le64(
        pext_value_a,
        (UINT64_C(1) << APFS_TM_PEXT_KIND_SHIFT) | 1U);
    infiltratr_store_le64(pext_value_a + 8U, APFS_TM_DSTREAM);
    infiltratr_store_le32(pext_value_a + 16U, 1U);
    infiltratr_store_le64(
        pext_value_b,
        (UINT64_C(1) << APFS_TM_PEXT_KIND_SHIFT) | 1U);
    infiltratr_store_le64(pext_value_b + 8U, APFS_TM_DSTREAM);
    infiltratr_store_le32(pext_value_b + 16U, 1U);
    const ApfsTmRecord pext_records[] = {
        {pext_key_a, 8U, pext_value_a, 20U},
        {pext_key_b, 8U, pext_value_b, 20U},
    };
    if (apfs_tm_build_node(
            raw, APFS_TM_EXTREF,
            APFS_TM_OBJ_PHYSICAL | APFS_TM_TYPE_BTREE,
            APFS_TM_TYPE_BLOCKREFTREE,
            pext_records, 2U, 0) != 0 ||
        apfs_tm_write_block(fd, APFS_TM_EXTREF, raw) != 0)
        goto fail;

    memset(raw, 'A', sizeof(raw));
    if (apfs_tm_write_block(fd, APFS_TM_DATA_A, raw) != 0)
        goto fail;
    memset(raw, 'B', sizeof(raw));
    if (apfs_tm_write_block(fd, APFS_TM_DATA_B, raw) != 0)
        goto fail;

    if (fsync(fd) != 0) goto fail;
    (void)close(fd);
    return 0;

fail:
    (void)close(fd);
    return -1;
}

static int verify_apfs_payload_state(
    const char *path, int expect_fragmented,
    char *detail, size_t detail_capacity)
{
    if (detail != NULL && detail_capacity != 0U)
        detail[0] = '\0';

    uint64_t expected_blocks = 0U;
    ApfsAnalysis analysis;
    char error[512] = {0};
    if (apfs_tm_expected_blocks(path, &expected_blocks) != 0 ||
        apfs_analyse(path, &analysis, error, sizeof(error)) != 0) {
        if (detail != NULL && detail_capacity != 0U)
            (void)snprintf(
                detail, detail_capacity,
                "APFS analyser rejected fixture: %s",
                error[0] != '\0' ? error : "invalid capped media geometry");
        return -1;
    }
    const int shape_ok =
        analysis.block_size == APFS_TM_BLOCK &&
        analysis.block_count == expected_blocks &&
        analysis.regular_files == 1U &&
        analysis.fragmented_files ==
            (expect_fragmented != 0 ? 1U : 0U);
    apfs_analysis_free(&analysis);
    if (!shape_ok) {
        if (detail != NULL && detail_capacity != 0U)
            (void)snprintf(
                detail, detail_capacity,
                "APFS fixture geometry/fragmentation does not match the qualification contract");
        return -1;
    }

    int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = open(path, flags);
    if (fd < 0)
        return -1;
    uint8_t block[APFS_TM_BLOCK];
    const uint64_t first =
        expect_fragmented != 0 ? APFS_TM_DATA_A : UINT64_C(10);
    const uint64_t second =
        expect_fragmented != 0 ? APFS_TM_DATA_B : UINT64_C(11);
    if (apfs_tm_pread_full(
            fd, block, sizeof(block),
            first * APFS_TM_BLOCK) != 0) {
        (void)close(fd);
        return -1;
    }
    for (size_t index = 0U; index < sizeof(block); ++index) {
        if (block[index] != (uint8_t)'A') {
            (void)close(fd);
            if (detail != NULL && detail_capacity != 0U)
                (void)snprintf(
                    detail, detail_capacity,
                    "APFS fixture first payload block changed");
            return -1;
        }
    }
    if (apfs_tm_pread_full(
            fd, block, sizeof(block),
            second * APFS_TM_BLOCK) != 0) {
        (void)close(fd);
        return -1;
    }
    (void)close(fd);
    for (size_t index = 0U; index < sizeof(block); ++index) {
        if (block[index] != (uint8_t)'B') {
            if (detail != NULL && detail_capacity != 0U)
                (void)snprintf(
                    detail, detail_capacity,
                    "APFS fixture second payload block changed");
            return -1;
        }
    }
    if (detail != NULL && detail_capacity != 0U)
        (void)snprintf(
            detail, detail_capacity,
            expect_fragmented != 0
                ? "bounded APFS fixture verified across capped media: one deliberately fragmented two-block regular file"
                : "bounded APFS post-defrag fixture verified byte-for-byte; production analyser reports zero fragmented files");
    return 0;
}

int ldtm_verify_apfs_payload(
    const char *path, char *detail, size_t detail_capacity)
{
    return verify_apfs_payload_state(
        path, 1, detail, detail_capacity);
}

int ldtm_verify_apfs_payload_after_defrag(
    const char *path, char *detail, size_t detail_capacity)
{
    return verify_apfs_payload_state(
        path, 0, detail, detail_capacity);
}
