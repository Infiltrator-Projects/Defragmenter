// SPDX-License-Identifier: GPL-3.0-or-later
/* White-box native XFS metadata-tree test.
 *
 * This translation unit deliberately includes the metadata implementation so
 * its private B+tree construction/verifier can be exercised without exposing
 * test-only symbols in the production ABI.
 */
#include "../gui/filesystems/xfs/native/xfs_metadata.c"

#include <stdio.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "XFS metadata test failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static void push_rmap_raw(RawVec *records, uint32_t start, uint32_t count,
                          uint64_t owner, uint64_t offset_flags) {
    uint8_t raw[24];
    XfsRmapRecord record = {start, count, owner, offset_flags};
    encode_rmap(&record, raw);
    raw_push(records, raw);
}


static int stamp_log_cycle(int fd, uint64_t start, uint64_t bb, uint32_t cycle) {
    uint8_t raw[512];
    memset(raw, 0, sizeof(raw));
    xfs_put_be32(raw, cycle);
    CHECK(pwrite(fd, raw, sizeof(raw), (off_t)(start + bb * 512U)) == (ssize_t)sizeof(raw));
    return 0;
}

static int write_log_header(int fd, uint64_t start, uint64_t bb,
                             uint32_t cycle, uint32_t numops,
                             const uint8_t uuid[16]) {
    uint8_t raw[512];
    memset(raw, 0, sizeof(raw));
    xfs_put_be32(raw, XFS_LOG_MAGIC);
    xfs_put_be32(raw + 4, cycle);
    xfs_put_be32(raw + 8, 2U);
    xfs_put_be32(raw + 12, 20U);
    xfs_put_be64(raw + 16, ((uint64_t)cycle << 32) | bb);
    xfs_put_be64(raw + 24, ((uint64_t)cycle << 32) | bb);
    xfs_put_be32(raw + 40, numops);
    xfs_put_be32(raw + 300, 1U);
    memcpy(raw + 304, uuid, 16U);
    xfs_put_be32(raw + 320, 32U * 1024U);
    CHECK(pwrite(fd, raw, sizeof(raw), (off_t)(start + bb * 512U)) == (ssize_t)sizeof(raw));
    return 0;
}

static int write_unmount_data_ex(int fd, uint64_t start, uint64_t bb,
                                  uint32_t cycle, bool clean,
                                  uint8_t client, uint32_t op_len) {
    uint8_t raw[512];
    memset(raw, 0, sizeof(raw));
    /* The first op-header word is replaced by the XFS cycle stamp on disk. */
    xfs_put_be32(raw, cycle);
    xfs_put_be32(raw + 4, op_len);
    raw[8] = client;
    raw[9] = clean ? XFS_LOG_UNMOUNT_TRANS : 0U;
    CHECK(pwrite(fd, raw, sizeof(raw), (off_t)(start + bb * 512U)) == (ssize_t)sizeof(raw));
    return 0;
}

static int write_unmount_data(int fd, uint64_t start, uint64_t bb,
                              uint32_t cycle, bool clean) {
    return write_unmount_data_ex(fd, start, bb, cycle, clean,
                                 XFS_LOG_CLIENT, 8U);
}

static int test_clean_log_head_detection(void) {
    XfsCatalogue catalogue;
    memset(&catalogue, 0, sizeof(catalogue));
    catalogue.geometry.block_size = 4096U;
    catalogue.geometry.logstart = 1U;
    catalogue.geometry.logblocks = 8U; /* 64 basic blocks */
    const uint8_t uuid[16] = {
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
        0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f
    };
    memcpy(catalogue.geometry.uuid, uuid, sizeof(uuid));
    uint64_t start = catalogue.geometry.logstart * catalogue.geometry.block_size;
    uint64_t log_bbs = (uint64_t)catalogue.geometry.logblocks *
                       catalogue.geometry.block_size / 512U;

    char image[] = "/tmp/defragmenter-xfs-log-XXXXXX";
    int fd = mkstemp(image);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, (off_t)(start + log_bbs * 512U)) == 0);

    /* Canonical pre-wrap clean log: the record at blocks 8-9 ends at head 10. */
    for (uint64_t bb = 0; bb < 10U; ++bb)
        CHECK(stamp_log_cycle(fd, start, bb, 7U) == 0);
    CHECK(write_log_header(fd, start, 8U, 7U, 1U, uuid) == 0);
    CHECK(write_unmount_data(fd, start, 9U, 7U, true) == 0);
    CHECK(fsync(fd) == 0);
    char *error = NULL;
    CHECK(xfs_verify_clean_log(image, &catalogue, &error) == 0);
    CHECK(error == NULL);

    /* Same physical head but no unmount flag: must remain a hard rejection. */
    CHECK(write_unmount_data(fd, start, 9U, 7U, false) == 0);
    CHECK(fsync(fd) == 0);
    CHECK(xfs_verify_clean_log(image, &catalogue, &error) != 0);
    CHECK(error != NULL);
    xfs_clear_error(&error);

    /* Wrapped clean log: cycle 8 is the new prefix and cycle 7 the old tail.
     * The unmount record at 3-4 ends exactly at physical head block 5. */
    CHECK(ftruncate(fd, 0) == 0);
    CHECK(ftruncate(fd, (off_t)(start + log_bbs * 512U)) == 0);
    for (uint64_t bb = 0; bb < 5U; ++bb)
        CHECK(stamp_log_cycle(fd, start, bb, 8U) == 0);
    for (uint64_t bb = 5U; bb < log_bbs; ++bb)
        CHECK(stamp_log_cycle(fd, start, bb, 7U) == 0);
    CHECK(write_log_header(fd, start, 3U, 8U, 1U, uuid) == 0);
    CHECK(write_unmount_data(fd, start, 4U, 8U, true) == 0);
    CHECK(fsync(fd) == 0);
    CHECK(xfs_verify_clean_log(image, &catalogue, &error) == 0);
    CHECK(error == NULL);

    /* Kernel-equivalent criterion: after the record is proven complete and
     * single-operation, XLOG_UNMOUNT_TRANS is the cleanliness marker.  The
     * client/oh_len fields are diagnostic, not additional invented gates. */
    CHECK(write_unmount_data_ex(fd, start, 4U, 8U, true, 0x69U, 0U) == 0);
    CHECK(fsync(fd) == 0);
    CHECK(xfs_verify_clean_log(image, &catalogue, &error) == 0);
    CHECK(error == NULL);

    /* Same-cycle head refinement.  All blocks carry cycle 9, so a naive
     * cycle-only implementation would call physical block zero the head.
     * A partial/stale header at block 60 cannot end at block 64, so XFS backs
     * the head up to block 60.  The immediately preceding clean unmount record
     * at 58-59 must then be recognised. */
    CHECK(ftruncate(fd, 0) == 0);
    CHECK(ftruncate(fd, (off_t)(start + log_bbs * 512U)) == 0);
    for (uint64_t bb = 0; bb < log_bbs; ++bb)
        CHECK(stamp_log_cycle(fd, start, bb, 9U) == 0);
    CHECK(write_log_header(fd, start, 58U, 9U, 1U, uuid) == 0);
    CHECK(write_unmount_data(fd, start, 59U, 9U, true) == 0);
    CHECK(write_log_header(fd, start, 60U, 9U, 2U, uuid) == 0);
    /* Header 60 describes two data BBs (h_len=20 still one BB here), so it
     * would end at 62, not physical end 64; it represents stale/partial data
     * beyond the real head and must not be mistaken for the head record. */
    CHECK(fsync(fd) == 0);
    CHECK(xfs_verify_clean_log(image, &catalogue, &error) == 0);
    CHECK(error == NULL);

    CHECK(close(fd) == 0);
    CHECK(unlink(image) == 0);
    return 0;
}

static int test_allocation_tree_pool_growth(void) {
    XfsRangeVec planned_free = {0}, source_free = {0}, protected = {0};
    XfsU64Vec pool = {0};
    const uint64_t first = 1000U;
    const size_t free_records = 9500U;

    /*
     * 9,500 one-block extents at 4 KiB require 20 bnobt blocks and
     * 20 cntbt blocks.  This reproduces the field failure where a normal
     * relayout needed 40 allocation-tree blocks but the AG had only 13
     * existing tree/AGFL blocks.
     */
    for (size_t index = 0; index < free_records; ++index) {
        uint64_t block = first + (uint64_t)index * 2U;
        xfs_range_push(&planned_free, block, block + 1U);
        xfs_range_push(&source_free, block, block + 1U);
    }
    uint64_t protected_block = first + (uint64_t)(free_records - 1U) * 2U;
    xfs_range_push(&protected, protected_block, protected_block + 1U);

    for (uint64_t block = 50U; block < 63U; ++block) xfs_u64_push(&pool, block);
    CHECK(pool.count == 13U);
    CHECK(tree_required_blocks(&SPEC_BNOBT, 4096U, planned_free.count) == 20U);
    CHECK(tree_required_blocks(&SPEC_CNTBT, 4096U, planned_free.count) == 20U);

    size_t nb = 0, nc = 0, nr = 0;
    char *error = NULL;
    CHECK(ensure_allocation_tree_pool(
              4096U, false, 0U, &planned_free, &source_free, &protected,
              &pool, &nb, &nc, &nr, &error) == 0);
    CHECK(error == NULL);
    CHECK(nb + nc == 40U);
    CHECK(nr == 0U);
    CHECK(pool.count >= nb + nc);
    CHECK(xfs_range_contains(&planned_free, protected_block, protected_block + 1U));

    xfs_range_free(&planned_free);
    xfs_range_free(&source_free);
    xfs_range_free(&protected);
    xfs_u64_free(&pool);
    return 0;
}

static int test_ag_owner_rmap_regeneration(void) {
    XfsU64Vec pool = {0};
    XfsRmapVec records = {0};
    for (const uint64_t blocks[] = {4U, 5U, 8U, 9U, 10U, 20U};
         pool.count < sizeof(blocks) / sizeof(blocks[0]); )
        xfs_u64_push(&pool, blocks[pool.count]);

    char *error = NULL;
    CHECK(append_ag_owner_rmaps(&pool, 5U, &records, &error) == 0);
    CHECK(error == NULL);
    CHECK(records.count == 2U);
    CHECK(records.items[0].start == 4U && records.items[0].count == 2U);
    CHECK(records.items[1].start == 8U && records.items[1].count == 3U);
    CHECK(records.items[0].owner == XFS_RMAP_OWN_AG);
    CHECK(records.items[1].owner == XFS_RMAP_OWN_AG);
    CHECK(records.items[0].offset_flags == 0U);
    CHECK(records.items[1].offset_flags == 0U);

    xfs_u64_free(&pool);
    xfs_rmap_free(&records);
    return 0;
}

int main(void) {
    CHECK(test_clean_log_head_detection() == 0);
    CHECK(test_allocation_tree_pool_growth() == 0);
    CHECK(test_ag_owner_rmap_regeneration() == 0);
    XfsGeometry geometry;
    memset(&geometry, 0, sizeof(geometry));
    geometry.block_size = 4096U;
    geometry.sector_size = 512U;
    geometry.dblocks = 4096U;
    geometry.agblocks = 4096U;
    geometry.agcount = 1U;
    geometry.agblklog = 12U;
    const uint8_t uuid[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
        0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
    };
    memcpy(geometry.uuid, uuid, sizeof(uuid));
    memcpy(geometry.meta_uuid, uuid, sizeof(uuid));

    XfsRmapRecord inode = {100U, 4U, 12345U, 7U | XFS_RMAP_UNWRITTEN};
    XfsRmapRecord special = {100U, 4U, UINT64_C(0xfffffffffffffffc), 0U};
    uint8_t low[20], high[20];
    rmap_low_key(&inode, low);
    rmap_high_key(&inode, high);
    CHECK(xfs_be32(low) == 100U);
    CHECK(xfs_be64(low + 4) == 12345U);
    CHECK(xfs_be64(low + 12) == 7U);
    CHECK(xfs_be32(high) == 103U);
    CHECK(xfs_be64(high + 12) == 10U);
    rmap_high_key(&special, high);
    CHECK(xfs_be32(high) == 103U);
    CHECK(xfs_be64(high + 4) == UINT64_C(0xfffffffffffffffc));
    CHECK(xfs_be64(high + 12) == 0U);

    char image[] = "/tmp/defragmenter-xfs-rmap-XXXXXX";
    int fd = mkstemp(image);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, (off_t)(geometry.dblocks * geometry.block_size)) == 0);
    RawMeta meta = {.fd = fd, .g = &geometry, .writable = true};

    RawVec records;
    raw_init(&records, 24U);
    push_rmap_raw(&records, 0U, 16U, UINT64_C(0xfffffffffffffffc), 0U);
    for (uint32_t index = 0; index < 400U; ++index)
        push_rmap_raw(&records, 1000U + index, 1U, 10000U + index, 0U);

    uint64_t pool[16];
    for (size_t index = 0; index < 16U; ++index) pool[index] = 6U + index;
    size_t required = tree_required_blocks(&SPEC_RMAPBT, geometry.block_size, records.count);
    CHECK(required >= 4U);
    CHECK(required <= 16U);
    uint32_t root = 0, levels = 0;
    XfsU64Vec used = {0};
    char *error = NULL;
    CHECK(build_tree(&meta, 0U, &SPEC_RMAPBT, &records, pool, 16U,
                     &root, &levels, &used, &error) == 0);
    CHECK(error == NULL);
    CHECK(levels >= 2U);
    CHECK(used.count == required);
    CHECK(fsync(fd) == 0);

    XfsU64Vec read_blocks = {0};
    RawVec decoded = {0};
    meta.writable = false;
    CHECK(read_tree(&meta, 0U, root, &SPEC_RMAPBT, &read_blocks, &decoded, &error) == 0);
    CHECK(error == NULL);
    CHECK(read_blocks.count == required);
    CHECK(decoded.count == 401U);
    CHECK(xfs_be32(raw_at_const(&decoded, 0U)) == 0U);
    CHECK(xfs_be32(raw_at_const(&decoded, 400U)) == 1399U);

    /* Corrupt one byte of the internal root separator. The native reader must
     * reject either the CRC or the interleaved low/high separator pair. */
    uint8_t corrupt = 0;
    CHECK(pread(fd, &corrupt, 1U,
                (off_t)(root * geometry.block_size + XFS_META_HEADER)) == 1);
    corrupt ^= 0x01U;
    CHECK(pwrite(fd, &corrupt, 1U,
                 (off_t)(root * geometry.block_size + XFS_META_HEADER)) == 1);
    CHECK(fsync(fd) == 0);
    xfs_u64_free(&read_blocks);
    raw_free(&decoded);
    CHECK(read_tree(&meta, 0U, root, &SPEC_RMAPBT, &read_blocks, &decoded, &error) != 0);
    CHECK(error != NULL);

    xfs_clear_error(&error);
    xfs_u64_free(&read_blocks);
    raw_free(&decoded);
    xfs_u64_free(&used);
    raw_free(&records);
    CHECK(close(fd) == 0);
    CHECK(unlink(image) == 0);
    puts("native XFS multi-level rmap separator, CRC and sibling verifier tests passed");
    return 0;
}
