// SPDX-License-Identifier: GPL-3.0-or-later
/* Native XFS core tests: C is the authoritative filesystem implementation. */
#include "xfs_native.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "XFS native test failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

static XfsCatalogue make_catalogue(uint64_t physical, uint64_t blocks) {
    XfsCatalogue catalogue;
    memset(&catalogue, 0, sizeof(catalogue));
    catalogue.geometry.block_size = 512U;
    catalogue.geometry.dblocks = 256U;
    catalogue.geometry.agblocks = 128U;
    catalogue.geometry.agcount = 2U;
    catalogue.geometry.agblklog = 7U;
    catalogue.geometry.inode_size = 512U;
    catalogue.objects.items = calloc(1U, sizeof(*catalogue.objects.items));
    CHECK(catalogue.objects.items != NULL);
    catalogue.objects.count = 1U;
    catalogue.objects.capacity = 1U;
    XfsObject *object = &catalogue.objects.items[0];
    object->inode = 20U;
    object->is_file = true;
    object->data_format = XFS_DINODE_FMT_EXTENTS;
    object->fork_size = 336U;
    object->nblocks = blocks;
    xfs_extent_push(&object->extents, (XfsExtent){0U, physical, blocks, false});
    xfs_range_push(&catalogue.free_ranges, 10U, 60U);
    xfs_range_push(&catalogue.free_ranges, 110U, 128U);
    xfs_range_push(&catalogue.free_ranges, 140U, 180U);
    return catalogue;
}

static void test_crc32c(void) {
    static const uint8_t sample[] = "123456789";
    uint32_t crc = xfs_crc32c_intermediate(sample, 9U, UINT32_C(0xffffffff));
    CHECK((~crc) == UINT32_C(0xe3069283));
}

static void test_growth_planner(void) {
    XfsCatalogue catalogue = make_catalogue(90U, 10U);
    XfsPlan plan;
    char *error = NULL;
    CHECK(xfs_build_plan(&catalogue, "growth-defrag", &plan, &error) == 0);
    CHECK(error == NULL);
    CHECK(plan.count == 1U);
    CHECK(plan.items[0].item->inode == 20U);
    CHECK(plan.items[0].target_start == 10U);
    CHECK(plan.items[0].reserve == 1U);
    CHECK(plan.final_block == 20U);
    xfs_plan_free(&plan);
    xfs_catalogue_free(&catalogue);
}

static void write_pattern(int fd, uint64_t block, uint8_t first, uint8_t second) {
    uint8_t payload[512];
    for (size_t index = 0; index < sizeof(payload); index += 2U) {
        payload[index] = first;
        payload[index + 1U] = second;
    }
    CHECK(pwrite(fd, payload, sizeof(payload), (off_t)(block * sizeof(payload))) == (ssize_t)sizeof(payload));
}

static void verify_pattern(int fd, uint64_t block, uint8_t first, uint8_t second) {
    uint8_t payload[512];
    CHECK(pread(fd, payload, sizeof(payload), (off_t)(block * sizeof(payload))) == (ssize_t)sizeof(payload));
    for (size_t index = 0; index < sizeof(payload); index += 2U) {
        CHECK(payload[index] == first);
        CHECK(payload[index + 1U] == second);
    }
}

static void test_payload_relocation(void) {
    XfsCatalogue catalogue = make_catalogue(90U, 3U);
    XfsPlan plan;
    char *error = NULL;
    CHECK(xfs_build_plan(&catalogue, "defrag", &plan, &error) == 0);
    CHECK(plan.count == 1U && plan.items[0].target_start == 10U);

    char image_template[] = "/tmp/defragmenter-xfs-image-XXXXXX";
    int image_fd = mkstemp(image_template);
    CHECK(image_fd >= 0);
    CHECK(ftruncate(image_fd, (off_t)(256U * 512U)) == 0);
    write_pattern(image_fd, 90U, 20U, 0U);
    write_pattern(image_fd, 91U, 20U, 1U);
    write_pattern(image_fd, 92U, 20U, 2U);
    CHECK(fsync(image_fd) == 0);
    CHECK(close(image_fd) == 0);

    char plan_template[] = "/tmp/defragmenter-xfs-plan-XXXXXX";
    int plan_fd = mkstemp(plan_template);
    CHECK(plan_fd >= 0);
    CHECK(close(plan_fd) == 0);
    CHECK(unlink(plan_template) == 0);

    sqlite3 *db = NULL;
    CHECK(xfs_open_plan_db(plan_template, true, &db, &error) == 0);
    uint64_t move_count = 0;
    CHECK(xfs_populate_plan_db(image_template, &catalogue, &plan, db, &move_count, &error) == 0);
    CHECK(move_count == 3U);
    CHECK(xfs_permute_payloads(image_template, db, catalogue.geometry.block_size, move_count, true, &error) == 0);
    CHECK(sqlite3_close(db) == SQLITE_OK);

    image_fd = open(image_template, O_RDONLY);
    CHECK(image_fd >= 0);
    verify_pattern(image_fd, 10U, 20U, 0U);
    verify_pattern(image_fd, 11U, 20U, 1U);
    verify_pattern(image_fd, 12U, 20U, 2U);
    CHECK(close(image_fd) == 0);

    CHECK(unlink(image_template) == 0);
    CHECK(unlink(plan_template) == 0);
    xfs_clear_error(&error);
    xfs_plan_free(&plan);
    xfs_catalogue_free(&catalogue);
}


static void test_payload_checkpoint_boundary(void) {
    enum { MOVE_COUNT = 9000, SOURCE_BASE = 10000 };
    char image_template[] = "/tmp/defragmenter-xfs-checkpoint-image-XXXXXX";
    int image_fd = mkstemp(image_template);
    CHECK(image_fd >= 0);
    CHECK(ftruncate(image_fd, (off_t)((SOURCE_BASE + MOVE_COUNT) * 512U)) == 0);
    CHECK(close(image_fd) == 0);

    char plan_template[] = "/tmp/defragmenter-xfs-checkpoint-plan-XXXXXX";
    int plan_fd = mkstemp(plan_template);
    CHECK(plan_fd >= 0);
    CHECK(close(plan_fd) == 0);
    CHECK(unlink(plan_template) == 0);

    char *error = NULL;
    sqlite3 *db = NULL;
    CHECK(xfs_open_plan_db(plan_template, true, &db, &error) == 0);
    CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_stmt *insert = NULL;
    CHECK(sqlite3_prepare_v2(
              db,
              "INSERT INTO blocks(old,target,inode,logical,unwritten) VALUES(?,?,?,?,0)",
              -1, &insert, NULL) == SQLITE_OK);
    for (sqlite3_int64 index = 0; index < MOVE_COUNT; ++index) {
        sqlite3_bind_int64(insert, 1, SOURCE_BASE + index);
        sqlite3_bind_int64(insert, 2, index);
        sqlite3_bind_int64(insert, 3, 100U);
        sqlite3_bind_int64(insert, 4, index);
        CHECK(sqlite3_step(insert) == SQLITE_DONE);
        CHECK(sqlite3_reset(insert) == SQLITE_OK);
        CHECK(sqlite3_clear_bindings(insert) == SQLITE_OK);
    }
    CHECK(sqlite3_finalize(insert) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);

    CHECK(xfs_permute_payloads(image_template, db, 512U, MOVE_COUNT, false, &error) == 0);
    CHECK(error == NULL);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    CHECK(unlink(image_template) == 0);
    CHECK(unlink(plan_template) == 0);
    xfs_clear_error(&error);
}


static void test_growth_metadata_boundary_slack(void) {
    XfsCatalogue catalogue;
    memset(&catalogue, 0, sizeof(catalogue));
    catalogue.geometry.block_size = 512U;
    catalogue.geometry.dblocks = 256U;
    catalogue.geometry.agblocks = 128U;
    catalogue.geometry.agcount = 2U;
    catalogue.geometry.agblklog = 7U;
    catalogue.geometry.inode_size = 512U;

    catalogue.objects.items = calloc(2U, sizeof(*catalogue.objects.items));
    CHECK(catalogue.objects.items != NULL);
    catalogue.objects.count = 2U;
    catalogue.objects.capacity = 2U;
    for (size_t index = 0; index < 2U; ++index) {
        XfsObject *object = &catalogue.objects.items[index];
        object->inode = 30U + index;
        object->is_file = true;
        object->data_format = XFS_DINODE_FMT_EXTENTS;
        object->fork_size = 336U;
        object->nblocks = 1U;
        xfs_extent_push(&object->extents,
                        (XfsExtent){0U, 40U + index, 1U, false});
    }
    /* [10,13) is followed by fixed metadata.  Growth span for either one-block
     * file is two blocks (one data + one exact reserve), so the final block at
     * 12 is genuinely unusable without splitting a file or losing its reserve. */
    xfs_range_push(&catalogue.free_ranges, 10U, 13U);
    xfs_range_push(&catalogue.free_ranges, 20U, 30U);

    XfsPlan plan;
    char *error = NULL;
    CHECK(xfs_build_plan(&catalogue, "growth-defrag", &plan, &error) == 0);
    CHECK(error == NULL);
    CHECK(plan.count == 2U);
    CHECK(plan.boundary_slack == 1U);
    CHECK(plan.items[0].target_start == 10U);
    CHECK(plan.items[1].target_start == 20U);
    CHECK(plan.items[0].reserve == 1U && plan.items[1].reserve == 1U);
    xfs_plan_free(&plan);
    xfs_catalogue_free(&catalogue);
}

static void test_writer_feature_gates(void) {
    XfsCatalogue catalogue = make_catalogue(90U, 1U);
    catalogue.geometry.v5 = true;
    catalogue.geometry.logstart = 5U;
    char *error = NULL;
    catalogue.geometry.incompat = XFS_SB_FEAT_INCOMPAT_PARENT;
    CHECK(xfs_validate_writer_support(&catalogue, &error) != 0);
    CHECK(error != NULL);
    xfs_clear_error(&error);
    catalogue.geometry.incompat = 0U;
    CHECK(xfs_validate_writer_support(&catalogue, &error) == 0);
    CHECK(error == NULL);
    xfs_catalogue_free(&catalogue);
}

int main(void) {
    test_crc32c();
    test_growth_planner();
    test_payload_relocation();
    test_payload_checkpoint_boundary();
    test_growth_metadata_boundary_slack();
    test_writer_feature_gates();
    puts("native XFS CRC, planner, checkpoint, metadata-boundary slack, relocation and feature-gate tests passed");
    return 0;
}
