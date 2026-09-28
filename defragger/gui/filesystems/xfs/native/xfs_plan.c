// SPDX-License-Identifier: GPL-3.0-or-later
#include "xfs_native.h"
#include "ld_io.h"
#include "ld_runtime.h"
#include "ld_stop.h"
#include "infiltratr/arithmetic.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *vector_grow(void *items, size_t *capacity, size_t count, size_t item_size) {
    if (count == SIZE_MAX ||
        !infiltratr_array_reserve(&items, capacity, item_size, count + 1U, 16U))
        ld_die("XFS plan vector growth failed");
    return items;
}

static void placement_push(XfsPlan *plan, XfsPlacement placement) {
    plan->items = vector_grow(plan->items, &plan->capacity, plan->count, sizeof(*plan->items));
    plan->items[plan->count++] = placement;
}

typedef struct {
    XfsObject *object;
    uint64_t span;
} ObjectOrder;

static int object_order_compare(const void *left0, const void *right0) {
    const ObjectOrder *left = left0;
    const ObjectOrder *right = right0;
    if (left->span > right->span) return -1;
    if (left->span < right->span) return 1;
    if (left->object->inode < right->object->inode) return -1;
    if (left->object->inode > right->object->inode) return 1;
    return 0;
}

static int remove_span(XfsRangeVec *ranges, uint64_t start, uint64_t end, char **error) {
    XfsRangeVec output = {0};
    bool removed = false;
    for (size_t index = 0; index < ranges->count; ++index) {
        XfsRange range = ranges->items[index];
        if (!removed && start >= range.start && end <= range.end) {
            if (range.start < start) xfs_range_push(&output, range.start, start);
            if (end < range.end) xfs_range_push(&output, end, range.end);
            removed = true;
        } else {
            xfs_range_push(&output, range.start, range.end);
        }
    }
    if (!removed) {
        xfs_range_free(&output);
        xfs_set_error(error, "XFS planner attempted to consume an unavailable block span");
        return -1;
    }
    xfs_range_free(ranges);
    *ranges = output;
    return 0;
}

static bool earliest_fit(const XfsRangeVec *ranges, uint64_t length, uint64_t *target) {
    for (size_t index = 0; index < ranges->count; ++index) {
        XfsRange range = ranges->items[index];
        if (range.end - range.start >= length) {
            *target = range.start;
            return true;
        }
    }
    return false;
}

static size_t target_extent_count(const XfsObject *object, const XfsGeometry *g,
                                  uint64_t target_start) {
    size_t count = 0U;
    uint64_t target = target_start;
    bool have = false;
    uint64_t previous_target_end = 0U;
    uint64_t previous_logical_end = 0U;
    uint64_t previous_length = 0U;
    bool previous_unwritten = false;

    for (size_t extent_index = 0U;
         extent_index < object->extents.count; ++extent_index) {
        const XfsExtent *extent = &object->extents.items[extent_index];
        uint64_t logical = extent->logical;
        uint64_t remaining = extent->length;

        while (remaining != 0U) {
            const uint64_t ag_end =
                ((target / g->agblocks) + 1U) * (uint64_t)g->agblocks;
            const bool can_extend =
                have &&
                previous_target_end == target &&
                previous_logical_end == logical &&
                previous_unwritten == extent->unwritten &&
                previous_length < XFS_NATIVE_MAX_BMBT_EXTLEN &&
                (target - 1U) / g->agblocks == target / g->agblocks;

            if (!can_extend) {
                count++;
                previous_length = 0U;
                previous_unwritten = extent->unwritten;
                have = true;
            }

            uint64_t chunk = remaining;
            const uint64_t ag_room = ag_end - target;
            if (chunk > ag_room) chunk = ag_room;
            const uint64_t extent_room =
                XFS_NATIVE_MAX_BMBT_EXTLEN - previous_length;
            if (chunk > extent_room) chunk = extent_room;
            if (chunk == 0U) {
                /* Force the next iteration to start a new encoded extent. */
                have = false;
                continue;
            }

            previous_length += chunk;
            target += chunk;
            logical += chunk;
            remaining -= chunk;
            previous_target_end = target;
            previous_logical_end = logical;

            if (previous_length == XFS_NATIVE_MAX_BMBT_EXTLEN)
                have = false;
        }
    }
    return count;
}

int xfs_build_plan(XfsCatalogue *catalogue, const char *operation, XfsPlan *out, char **error) {
    memset(out, 0, sizeof(*out));
    bool growth = strcmp(operation, "growth-defrag") == 0;
    if (!growth && strcmp(operation, "defrag") != 0) {
        xfs_set_error(error, "unsupported XFS layout operation: %s", operation);
        return -1;
    }
    for (size_t index = 0; index < catalogue->free_ranges.count; ++index)
        xfs_range_push(&out->pool_ranges, catalogue->free_ranges.items[index].start,
                       catalogue->free_ranges.items[index].end);
    size_t active_count = 0;
    uint64_t minimum_span = UINT64_MAX;
    for (size_t index = 0; index < catalogue->objects.count; ++index) {
        XfsObject *object = &catalogue->objects.items[index];
        if (!object->is_file) continue;
        uint64_t blocks = xfs_object_block_count(object);
        if (blocks != 0) active_count++;
        for (size_t extent_index = 0; extent_index < object->extents.count; ++extent_index) {
            XfsExtent extent = object->extents.items[extent_index];
            xfs_range_push(&out->pool_ranges, extent.physical, extent.physical + extent.length);
        }
    }
    xfs_range_sort_merge(&out->pool_ranges);
    XfsRangeVec remaining = {0};
    for (size_t index = 0; index < out->pool_ranges.count; ++index)
        xfs_range_push(&remaining, out->pool_ranges.items[index].start, out->pool_ranges.items[index].end);

    ObjectOrder *order = active_count ? ld_xcalloc(active_count, sizeof(*order)) : NULL;
    size_t cursor = 0;
    for (size_t index = 0; index < catalogue->objects.count; ++index) {
        XfsObject *object = &catalogue->objects.items[index];
        if (!object->is_file) continue;
        uint64_t blocks = xfs_object_block_count(object);
        if (blocks == 0) continue;
        uint64_t reserve = growth ? (blocks * 10U + 99U) / 100U : 0;
        uint64_t span = blocks + reserve;
        if (span < minimum_span) minimum_span = span;
        order[cursor++] = (ObjectOrder){object, span};
    }
    if (active_count > 1) qsort(order, active_count, sizeof(*order), object_order_compare);

    for (size_t index = 0; index < active_count; ++index) {
        XfsObject *object = order[index].object;
        uint64_t blocks = xfs_object_block_count(object);
        uint64_t reserve = growth ? (blocks * 10U + 99U) / 100U : 0;
        uint64_t span = blocks + reserve;
        uint64_t target = 0;
        if (!earliest_fit(&remaining, span, &target)) {
            xfs_set_error(error,
                          "inode %" PRIu64 " needs a contiguous %" PRIu64
                          "-block legal run; the current XFS metadata layout has no such span",
                          object->inode, span);
            free(order);
            xfs_range_free(&remaining);
            xfs_plan_free(out);
            return -1;
        }
        size_t extents = target_extent_count(object, &catalogue->geometry, target);
        if (extents > object->fork_size / 16U) {
            xfs_set_error(error,
                          "inode %" PRIu64 " needs %zu direct extent records, but its inode data fork holds only %u",
                          object->inode, extents, object->fork_size / 16U);
            free(order);
            xfs_range_free(&remaining);
            xfs_plan_free(out);
            return -1;
        }
        placement_push(out, (XfsPlacement){object, target, reserve});
        if (remove_span(&remaining, target, target + span, error) != 0) {
            free(order);
            xfs_range_free(&remaining);
            xfs_plan_free(out);
            return -1;
        }
    }
    free(order);
    uint64_t final_block = 0;
    for (size_t index = 0; index < out->count; ++index) {
        uint64_t end = out->items[index].target_start + xfs_object_block_count(out->items[index].item);
        if (end > final_block) final_block = end;
    }
    out->final_block = final_block;
    uint64_t internal = 0;
    uint64_t largest_boundary_gap = 0;
    for (size_t index = 0; index < remaining.count; ++index) {
        uint64_t start = remaining.items[index].start;
        uint64_t end = remaining.items[index].end < final_block ? remaining.items[index].end : final_block;
        if (start < final_block && end > start) {
            uint64_t gap = end - start;
            internal += gap;
            if (gap > largest_boundary_gap) largest_boundary_gap = gap;
        }
    }
    xfs_range_free(&remaining);
    if (internal != 0) {
        /*
         * XFS fixed metadata can split the low-address data space into legal
         * islands.  First-fit packing consumes each island from its front, so
         * any residual range below final_block is a suffix immediately before
         * an immovable metadata/non-file allocation.  Such a suffix is truly
         * unavoidable when it is smaller than every complete movable object
         * span (file data plus the exact Growth Defrag reserve).  Reject larger
         * gaps because a complete object could in principle occupy them and the
         * canonical plan has not proved them unavoidable.
         */
        if (minimum_span == UINT64_MAX || largest_boundary_gap >= minimum_span) {
            xfs_set_error(error,
                          "XFS placement would leave %" PRIu64
                          " free block(s) below the final contiguous file, including a %" PRIu64
                          "-block gap large enough to hold the smallest legal %" PRIu64
                          "-block movable span",
                          internal, largest_boundary_gap, minimum_span == UINT64_MAX ? 0 : minimum_span);
            xfs_plan_free(out);
            return -1;
        }
        out->boundary_slack = internal;
    }
    return 0;
}

bool xfs_plan_already_applied(const XfsCatalogue *catalogue, const XfsPlan *plan) {
    for (size_t index = 0; index < plan->count; ++index) {
        const XfsPlacement *placement = &plan->items[index];
        uint64_t expected = placement->target_start;
        for (size_t extent_index = 0;
             extent_index < placement->item->extents.count; ++extent_index) {
            const XfsExtent *extent =
                &placement->item->extents.items[extent_index];
            if (extent->physical != expected)
                return false;
            if (UINT64_MAX - expected < extent->length)
                return false;
            expected += extent->length;
        }
        uint64_t blocks = xfs_object_block_count(placement->item);
        if (placement->reserve != 0 &&
            !xfs_range_contains(&catalogue->free_ranges,
                                placement->target_start + blocks,
                                placement->target_start + blocks + placement->reserve))
            return false;
    }
    return true;
}

static int sqlite_fail(sqlite3 *db, char **error, const char *action) {
    xfs_set_error(error, "%s: %s", action, sqlite3_errmsg(db));
    return -1;
}

static int checkpoint_plan_db(sqlite3 *db, char **error) {
    int log_frames = 0;
    int checkpointed_frames = 0;
    int rc = sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_PASSIVE,
                                       &log_frames, &checkpointed_frames);
    if (rc == SQLITE_OK) return 0;
    xfs_set_error(error,
                  "cannot checkpoint XFS plan (SQLite %d, %s): %s",
                  rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    return -1;
}

int xfs_open_plan_db(const char *path, bool create, sqlite3 **out, char **error) {
    *out = NULL;
    if (create) (void)unlink(path);
    sqlite3 *db = NULL;
    int flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0);
#ifdef SQLITE_OPEN_NOFOLLOW
    flags |= SQLITE_OPEN_NOFOLLOW;
#endif
    if (sqlite3_open_v2(path, &db, flags, NULL) != SQLITE_OK) {
        if (db != NULL) xfs_set_error(error, "cannot open XFS plan database: %s", sqlite3_errmsg(db));
        else xfs_set_error(error, "cannot allocate XFS plan database");
        sqlite3_close(db);
        return -1;
    }
    if (sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite_fail(db, error, "cannot configure XFS plan database");
        sqlite3_close(db);
        return -1;
    }
    if (create) {
        const char *schema =
            "CREATE TABLE objects(inode INTEGER PRIMARY KEY,target_start INTEGER NOT NULL,"
            "target_blocks INTEGER NOT NULL,reserve INTEGER NOT NULL,digest TEXT NOT NULL);"
            "CREATE TABLE blocks(old INTEGER PRIMARY KEY,target INTEGER NOT NULL UNIQUE,"
            "inode INTEGER NOT NULL,logical INTEGER NOT NULL,unwritten INTEGER NOT NULL,"
            "placed INTEGER NOT NULL DEFAULT 0);"
            "CREATE INDEX blocks_inode_logical ON blocks(inode,logical);";
        if (sqlite3_exec(db, schema, NULL, NULL, NULL) != SQLITE_OK) {
            sqlite_fail(db, error, "cannot create XFS plan schema");
            sqlite3_close(db);
            return -1;
        }
    }
    *out = db;
    return 0;
}

static int digest_object(int fd, uint32_t block_size, const XfsObject *object,
                         char hex[65], char **error) {
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (context == NULL || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(context);
        xfs_set_error(error, "cannot initialise SHA-256 for XFS payload verification");
        return -1;
    }
    const size_t chunk_limit = 4U * 1024U * 1024U;
    uint8_t *buffer = ld_xmalloc(chunk_limit);
    for (size_t extent_index = 0; extent_index < object->extents.count; ++extent_index) {
        const XfsExtent *extent = &object->extents.items[extent_index];
        uint64_t remaining = extent->length * block_size;
        uint64_t offset = extent->physical * block_size;
        while (remaining != 0) {
            size_t amount = remaining > chunk_limit ? chunk_limit : (size_t)remaining;
            ssize_t got = ld_pread_full(fd, buffer, amount, offset);
            if (got < 0 || (size_t)got != amount) {
                free(buffer);
                EVP_MD_CTX_free(context);
                xfs_set_error(error, "short payload read for XFS inode %" PRIu64, object->inode);
                return -1;
            }
            if (EVP_DigestUpdate(context, buffer, amount) != 1) {
                free(buffer);
                EVP_MD_CTX_free(context);
                xfs_set_error(error, "SHA-256 update failed for XFS inode %" PRIu64, object->inode);
                return -1;
            }
            offset += amount;
            remaining -= amount;
        }
    }
    free(buffer);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned digest_length = 0;
    if (EVP_DigestFinal_ex(context, digest, &digest_length) != 1 || digest_length != 32U) {
        EVP_MD_CTX_free(context);
        xfs_set_error(error, "SHA-256 finalisation failed for XFS inode %" PRIu64, object->inode);
        return -1;
    }
    EVP_MD_CTX_free(context);
    static const char digits[] = "0123456789abcdef";
    for (unsigned index = 0; index < 32U; ++index) {
        hex[index * 2U] = digits[digest[index] >> 4];
        hex[index * 2U + 1U] = digits[digest[index] & 15U];
    }
    hex[64] = '\0';
    return 0;
}

static int insert_object(sqlite3_stmt *statement, const XfsPlacement *placement,
                         const char *digest, char **error) {
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    uint64_t blocks = xfs_object_block_count(placement->item);
    if (placement->item->inode > INT64_MAX || placement->target_start > INT64_MAX ||
        blocks > INT64_MAX || placement->reserve > INT64_MAX) {
        xfs_set_error(error, "XFS plan value exceeds SQLite signed integer range");
        return -1;
    }
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)placement->item->inode);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)placement->target_start);
    sqlite3_bind_int64(statement, 3, (sqlite3_int64)blocks);
    sqlite3_bind_int64(statement, 4, (sqlite3_int64)placement->reserve);
    sqlite3_bind_text(statement, 5, digest, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(statement) != SQLITE_DONE) {
        xfs_set_error(error, "cannot insert XFS object plan row");
        return -1;
    }
    return 0;
}

static int insert_block(sqlite3_stmt *statement, uint64_t old, uint64_t target,
                        uint64_t inode, uint64_t logical, bool unwritten, char **error) {
    if (old > INT64_MAX || target > INT64_MAX || inode > INT64_MAX || logical > INT64_MAX) {
        xfs_set_error(error, "XFS block plan value exceeds SQLite signed integer range");
        return -1;
    }
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)old);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)target);
    sqlite3_bind_int64(statement, 3, (sqlite3_int64)inode);
    sqlite3_bind_int64(statement, 4, (sqlite3_int64)logical);
    sqlite3_bind_int(statement, 5, unwritten ? 1 : 0);
    if (sqlite3_step(statement) != SQLITE_DONE) {
        xfs_set_error(error, "cannot insert XFS block plan row: %s", sqlite3_errmsg(sqlite3_db_handle(statement)));
        return -1;
    }
    return 0;
}

int xfs_populate_plan_db(const char *stage, XfsCatalogue *catalogue, XfsPlan *plan,
                         sqlite3 *db, uint64_t *move_count, char **error) {
    *move_count = 0;
    int fd = open(stage, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        xfs_set_error(error, "cannot open staged XFS image: %s", strerror(errno));
        return -1;
    }
    sqlite3_stmt *object_stmt = NULL;
    sqlite3_stmt *block_stmt = NULL;
    if (sqlite3_prepare_v2(db, "INSERT INTO objects VALUES(?,?,?,?,?)", -1, &object_stmt, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "INSERT INTO blocks(old,target,inode,logical,unwritten) VALUES(?,?,?,?,?)",
                           -1, &block_stmt, NULL) != SQLITE_OK) {
        xfs_set_error(error, "cannot prepare XFS plan statements: %s", sqlite3_errmsg(db));
        sqlite3_finalize(object_stmt);
        sqlite3_finalize(block_stmt);
        close(fd);
        return -1;
    }
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        xfs_set_error(error, "cannot begin XFS plan transaction: %s", sqlite3_errmsg(db));
        sqlite3_finalize(object_stmt);
        sqlite3_finalize(block_stmt);
        close(fd);
        return -1;
    }
    int result = 0;
    for (size_t index = 0; index < plan->count && result == 0; ++index) {
        if (ld_stop_requested()) {
            xfs_set_error(error, "stop requested before XFS source commit");
            result = -1;
            break;
        }
        XfsPlacement *placement = &plan->items[index];
        char digest[65];
        if (digest_object(fd, catalogue->geometry.block_size, placement->item, digest, error) != 0 ||
            insert_object(object_stmt, placement, digest, error) != 0) {
            result = -1;
            break;
        }
        uint64_t target = placement->target_start;
        for (size_t extent_index = 0; extent_index < placement->item->extents.count && result == 0; ++extent_index) {
            XfsExtent extent = placement->item->extents.items[extent_index];
            for (uint64_t offset = 0; offset < extent.length; ++offset, ++target) {
                uint64_t old = extent.physical + offset;
                if (insert_block(block_stmt, old, target, placement->item->inode,
                                 extent.logical + offset, extent.unwritten, error) != 0) {
                    result = -1;
                    break;
                }
                if (old != target) (*move_count)++;
            }
        }
    }
    if (result == 0) {
        if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
            xfs_set_error(error, "cannot commit XFS plan: %s", sqlite3_errmsg(db));
            result = -1;
        }
    } else {
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    }
    sqlite3_finalize(object_stmt);
    sqlite3_finalize(block_stmt);
    close(fd);
    return result;
}

static int copy_block(int fd, uint32_t block_size, uint64_t source, uint64_t target, char **error) {
    uint8_t *buffer = ld_xmalloc(block_size);
    ssize_t got = ld_pread_full(fd, buffer, block_size, source * block_size);
    if (got < 0 || (size_t)got != block_size) {
        free(buffer);
        xfs_set_error(error, "short XFS source-block read");
        return -1;
    }
    ssize_t wrote = ld_pwrite_full(fd, buffer, block_size, target * block_size);
    free(buffer);
    if (wrote < 0 || (size_t)wrote != block_size) {
        xfs_set_error(error, "short XFS destination-block write");
        return -1;
    }
    return 0;
}

static int query_one_i64(sqlite3 *db, const char *sql, sqlite3_int64 argument,
                         bool bind, bool *found, sqlite3_int64 *value, char **error) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return sqlite_fail(db, error, "cannot prepare XFS move query");
    if (bind) sqlite3_bind_int64(stmt, 1, argument);
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
        *found = true;
        *value = sqlite3_column_int64(stmt, 0);
    } else if (step == SQLITE_DONE) {
        *found = false;
    } else {
        sqlite3_finalize(stmt);
        return sqlite_fail(db, error, "cannot execute XFS move query");
    }
    sqlite3_finalize(stmt);
    return 0;
}

static int mark_placed(sqlite3 *db, uint64_t old, char **error) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE blocks SET placed=1 WHERE old=?", -1, &stmt, NULL) != SQLITE_OK)
        return sqlite_fail(db, error, "cannot prepare XFS placed update");
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)old);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return sqlite_fail(db, error, "cannot update XFS placed block");
    }
    sqlite3_finalize(stmt);
    return 0;
}

typedef struct {
    uint64_t source;
    uint64_t target;
} XfsLiveMove;

#define XFS_LIVE_BATCH 128U

static void emit_live_moves(const XfsLiveMove *moves, size_t count, uint32_t block_size,
                            uint64_t placed, uint64_t *sequence) {
    if (count == 0U) return;
    printf("@@LIVE_RANGES {\"ranges\":[");
    for (size_t index = 0; index < count; ++index) {
        if (index != 0U) putchar(',');
        printf("[%" PRIu64 ",%" PRIu64 ",%u]",
               moves[index].source * (uint64_t)block_size,
               moves[index].target * (uint64_t)block_size,
               block_size);
    }
    printf("],\"moved_total_bytes\":%" PRIu64
           ",\"pass\":1,\"objects_done\":0,\"objects_total\":0,"
           "\"scope\":\"stage-preview\",\"sequence\":%" PRIu64 "}\n",
           placed * (uint64_t)block_size, ++(*sequence));
    fflush(stdout);
}

static void queue_live_move(bool enabled, XfsLiveMove *moves, size_t *count,
                            uint64_t source, uint64_t target, uint32_t block_size,
                            uint64_t placed, uint64_t *sequence) {
    if (!enabled || source == target) return;
    moves[(*count)++] = (XfsLiveMove){source, target};
    if (*count == XFS_LIVE_BATCH) {
        emit_live_moves(moves, *count, block_size, placed, sequence);
        *count = 0U;
    }
}

int xfs_permute_payloads(const char *stage, sqlite3 *db, uint32_t block_size,
                         uint64_t move_count, bool live_updates, char **error) {
    int fd = open(stage, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        xfs_set_error(error, "cannot open XFS stage for relocation: %s", strerror(errno));
        return -1;
    }
    uint64_t placed = 0;
    XfsLiveMove live_moves[XFS_LIVE_BATCH];
    size_t live_count = 0U;
    uint64_t live_sequence = 0U;
    sqlite3_stmt *terminals = NULL;
    const char *terminal_sql =
        "SELECT b.target FROM blocks b LEFT JOIN blocks s ON s.old=b.target "
        "WHERE b.old<>b.target AND s.old IS NULL AND b.target>? ORDER BY b.target";
    sqlite3_int64 terminal_floor = -1;
    if (sqlite3_prepare_v2(db, terminal_sql, -1, &terminals, NULL) != SQLITE_OK) {
        close(fd);
        return sqlite_fail(db, error, "cannot find XFS terminal moves");
    }
    sqlite3_bind_int64(terminals, 1, terminal_floor);
    while (sqlite3_step(terminals) == SQLITE_ROW) {
        terminal_floor = sqlite3_column_int64(terminals, 0);
        uint64_t free_block = (uint64_t)terminal_floor;
        for (;;) {
            bool found = false;
            sqlite3_int64 old0 = 0;
            if (query_one_i64(db,
                              "SELECT old FROM blocks WHERE target=? AND old<>target AND placed=0",
                              (sqlite3_int64)free_block, true, &found, &old0, error) != 0) {
                sqlite3_finalize(terminals); close(fd); return -1;
            }
            if (!found) break;
            uint64_t old = (uint64_t)old0;
            if (copy_block(fd, block_size, old, free_block, error) != 0 || mark_placed(db, old, error) != 0) {
                sqlite3_finalize(terminals); close(fd); return -1;
            }
            placed++;
            queue_live_move(live_updates, live_moves, &live_count, old, free_block,
                            block_size, placed, &live_sequence);
            free_block = old;
            if (placed % 8192U == 0) {
                if (ld_stop_requested()) { sqlite3_finalize(terminals); close(fd); xfs_set_error(error, "stop requested before XFS source commit"); return -1; }
                /* A WAL checkpoint cannot run while the terminal SELECT is active on
                 * this connection.  Finalize it, checkpoint, then resume strictly
                 * after the last terminal target already consumed. */
                sqlite3_finalize(terminals);
                terminals = NULL;
                if (checkpoint_plan_db(db, error) != 0) { close(fd); return -1; }
                if (sqlite3_prepare_v2(db, terminal_sql, -1, &terminals, NULL) != SQLITE_OK) {
                    close(fd); return sqlite_fail(db, error, "cannot resume XFS terminal moves");
                }
                sqlite3_bind_int64(terminals, 1, terminal_floor);
                printf("XFS placement: %" PRIu64 " of %" PRIu64 " blocks.\n", placed, move_count);
            }
        }
    }
    sqlite3_finalize(terminals);

    for (;;) {
        bool found = false;
        sqlite3_int64 start0 = 0;
        if (query_one_i64(db, "SELECT old FROM blocks WHERE old<>target AND placed=0 LIMIT 1",
                          0, false, &found, &start0, error) != 0) { close(fd); return -1; }
        if (!found) break;
        uint64_t start = (uint64_t)start0;
        uint8_t *saved = ld_xmalloc(block_size);
        ssize_t got = ld_pread_full(fd, saved, block_size, start * block_size);
        if (got < 0 || (size_t)got != block_size) {
            free(saved); close(fd); xfs_set_error(error, "short read while opening an XFS block cycle"); return -1;
        }
        uint64_t free_block = start;
        for (;;) {
            bool predecessor_found = false;
            sqlite3_int64 old0 = 0;
            if (query_one_i64(db,
                              "SELECT old FROM blocks WHERE target=? AND old<>target AND placed=0",
                              (sqlite3_int64)free_block, true, &predecessor_found, &old0, error) != 0) {
                free(saved); close(fd); return -1;
            }
            if (!predecessor_found) {
                free(saved); close(fd); xfs_set_error(error, "broken XFS block-permutation cycle"); return -1;
            }
            uint64_t old = (uint64_t)old0;
            if (old == start) {
                ssize_t wrote = ld_pwrite_full(fd, saved, block_size, free_block * block_size);
                if (wrote < 0 || (size_t)wrote != block_size || mark_placed(db, start, error) != 0) {
                    free(saved); close(fd); if (error && *error == NULL) xfs_set_error(error, "short write while closing an XFS block cycle"); return -1;
                }
                placed++;
                queue_live_move(live_updates, live_moves, &live_count, start, free_block,
                                block_size, placed, &live_sequence);
                break;
            }
            if (copy_block(fd, block_size, old, free_block, error) != 0 || mark_placed(db, old, error) != 0) {
                free(saved); close(fd); return -1;
            }
            placed++;
            queue_live_move(live_updates, live_moves, &live_count, old, free_block,
                            block_size, placed, &live_sequence);
            free_block = old;
            if (placed % 8192U == 0) {
                if (ld_stop_requested()) {
                    free(saved); close(fd); xfs_set_error(error, "stop requested before XFS source commit"); return -1;
                }
                if (checkpoint_plan_db(db, error) != 0) {
                    free(saved); close(fd); return -1;
                }
                printf("XFS placement: %" PRIu64 " of %" PRIu64 " blocks.\n", placed, move_count);
            }
        }
        free(saved);
    }
    if (live_updates && live_count != 0U) {
        emit_live_moves(live_moves, live_count, block_size, placed, &live_sequence);
        live_count = 0U;
    }
    if (fsync(fd) != 0) {
        xfs_set_error(error, "cannot sync relocated XFS stage: %s", strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    if (placed != move_count) {
        xfs_set_error(error, "placed %" PRIu64 " of %" PRIu64 " XFS block moves", placed, move_count);
        return -1;
    }
    return 0;
}

static int encode_extent(const XfsExtent *extent, const XfsGeometry *g, uint8_t out[16], char **error) {
    if (extent->length == 0 || extent->length > XFS_NATIVE_MAX_BMBT_EXTLEN) {
        xfs_set_error(error, "XFS extent length is outside the bmbt field width");
        return -1;
    }
    uint64_t agno = extent->physical / g->agblocks;
    uint64_t agbno = extent->physical % g->agblocks;
    if (agbno + extent->length > g->agblocks) {
        xfs_set_error(error, "XFS extent crosses an allocation-group boundary");
        return -1;
    }
    uint64_t fsblock = (agno << g->agblklog) | agbno;
    uint64_t l0 = ((uint64_t)(extent->unwritten ? 1U : 0U) << 63) |
                  (extent->logical << 9) | ((fsblock >> 43) & UINT64_C(0x1ff));
    uint64_t l1 = ((fsblock & ((UINT64_C(1) << 43) - 1)) << 21) | extent->length;
    xfs_put_be64(out, l0);
    xfs_put_be64(out + 8, l1);
    return 0;
}

static void update_inode_crc(uint8_t *inode, uint32_t inode_size) {
    if (inode_size < 104U || inode[4] < 3U) return;
    memset(inode + 100, 0, 4);
    uint32_t final = xfs_crc_field(inode, inode_size, 100);
    inode[100] = (uint8_t)final;
    inode[101] = (uint8_t)(final >> 8);
    inode[102] = (uint8_t)(final >> 16);
    inode[103] = (uint8_t)(final >> 24);
}

static int target_extents_for_inode(sqlite3 *db, uint64_t inode, const XfsGeometry *g,
                                    XfsExtentVec *extents, char **error) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT logical,target,unwritten FROM blocks WHERE inode=? ORDER BY logical",
            -1, &stmt, NULL) != SQLITE_OK)
        return sqlite_fail(db, error, "cannot query XFS target extents");
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)inode);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        uint64_t logical = (uint64_t)sqlite3_column_int64(stmt, 0);
        uint64_t physical = (uint64_t)sqlite3_column_int64(stmt, 1);
        bool unwritten = sqlite3_column_int(stmt, 2) != 0;
        if (extents->count != 0) {
            XfsExtent *old = &extents->items[extents->count - 1];
            bool same_ag = old->physical / g->agblocks == physical / g->agblocks;
            if (old->logical + old->length == logical && old->physical + old->length == physical &&
                old->unwritten == unwritten && same_ag && old->length < XFS_NATIVE_MAX_BMBT_EXTLEN) {
                old->length++;
                continue;
            }
        }
        xfs_extent_push(extents, (XfsExtent){logical, physical, 1, unwritten});
    }
    sqlite3_finalize(stmt);
    return 0;
}

static const XfsObject *find_object(const XfsCatalogue *catalogue, uint64_t inode) {
    for (size_t index = 0; index < catalogue->objects.count; ++index)
        if (catalogue->objects.items[index].inode == inode)
            return &catalogue->objects.items[index];
    return NULL;
}

int xfs_apply_inode_mappings(const char *stage, const XfsCatalogue *catalogue,
                             sqlite3 *db, char **error) {
    int fd = open(stage, O_RDWR | O_CLOEXEC);
    if (fd < 0) { xfs_set_error(error, "cannot open XFS stage for inode rewrite: %s", strerror(errno)); return -1; }
    sqlite3_stmt *objects = NULL;
    if (sqlite3_prepare_v2(db, "SELECT inode FROM objects ORDER BY inode", -1, &objects, NULL) != SQLITE_OK) {
        close(fd); return sqlite_fail(db, error, "cannot query XFS planned objects");
    }
    int result = 0;
    while (sqlite3_step(objects) == SQLITE_ROW) {
        if (ld_stop_requested()) { xfs_set_error(error, "stop requested before XFS source commit"); result = -1; break; }
        uint64_t inode_number = (uint64_t)sqlite3_column_int64(objects, 0);
        const XfsObject *item = find_object(catalogue, inode_number);
        if (item == NULL) { xfs_set_error(error, "planned XFS inode %" PRIu64 " is missing", inode_number); result = -1; break; }
        XfsExtentVec extents = {0};
        if (target_extents_for_inode(db, inode_number, &catalogue->geometry, &extents, error) != 0) {
            xfs_extent_free(&extents); result = -1; break;
        }
        if (extents.count * 16U > item->fork_size) {
            xfs_set_error(error, "inode %" PRIu64 " no longer fits its direct extent fork", inode_number);
            xfs_extent_free(&extents); result = -1; break;
        }
        uint8_t *raw = ld_xmalloc(catalogue->geometry.inode_size);
        ssize_t got = ld_pread_full(fd, raw, catalogue->geometry.inode_size, item->inode_offset);
        if (got < 0 || (size_t)got != catalogue->geometry.inode_size) {
            free(raw); xfs_extent_free(&extents); xfs_set_error(error, "short inode read for XFS inode %" PRIu64, inode_number); result = -1; break;
        }
        raw[5] = XFS_DINODE_FMT_EXTENTS;
        uint64_t new_nblocks = item->nblocks >= item->bmap_blocks.count ? item->nblocks - item->bmap_blocks.count : 0;
        if (new_nblocks < xfs_object_block_count(item)) {
            free(raw); xfs_extent_free(&extents); xfs_set_error(error, "inode %" PRIu64 " has an invalid allocated-block count", inode_number); result = -1; break;
        }
        xfs_put_be64(raw + 64, new_nblocks);
        if ((item->flags2 & XFS_DIFLAG2_NREXT64) != 0) {
            xfs_put_be64(raw + 24, extents.count);
        } else {
            if (extents.count > UINT32_C(0x7fffffff)) {
                free(raw); xfs_extent_free(&extents); xfs_set_error(error, "inode %" PRIu64 " exceeds the small extent counter", inode_number); result = -1; break;
            }
            xfs_put_be32(raw + 76, (uint32_t)extents.count);
        }
        memset(raw + item->core_size, 0, item->fork_size);
        for (size_t index = 0; index < extents.count; ++index) {
            uint8_t encoded[16];
            if (encode_extent(&extents.items[index], &catalogue->geometry, encoded, error) != 0) {
                free(raw); xfs_extent_free(&extents); result = -1; goto done;
            }
            memcpy(raw + item->core_size + index * 16U, encoded, sizeof(encoded));
        }
        update_inode_crc(raw, catalogue->geometry.inode_size);
        ssize_t wrote = ld_pwrite_full(fd, raw, catalogue->geometry.inode_size, item->inode_offset);
        free(raw);
        xfs_extent_free(&extents);
        if (wrote < 0 || (size_t)wrote != catalogue->geometry.inode_size) {
            xfs_set_error(error, "short inode write for XFS inode %" PRIu64, inode_number); result = -1; break;
        }
    }
done:
    sqlite3_finalize(objects);
    if (result == 0 && fsync(fd) != 0) { xfs_set_error(error, "cannot sync rewritten XFS inodes: %s", strerror(errno)); result = -1; }
    close(fd);
    return result;
}

int xfs_verify_stage(const char *stage, sqlite3 *db, const XfsCatalogue *source,
                     bool growth, XfsCatalogue *verified, char **error) {
    if (xfs_verify_allocation_metadata(stage, source, db, error) != 0) return -1;
    if (xfs_scan_catalogue(stage, true, verified, error) != 0) return -1;
    if (memcmp(verified->geometry.uuid, source->geometry.uuid, 16) != 0 ||
        verified->geometry.dblocks != source->geometry.dblocks) {
        xfs_catalogue_free(verified);
        xfs_set_error(error, "staged XFS identity or active capacity changed");
        return -1;
    }
    int fd = open(stage, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { xfs_catalogue_free(verified); xfs_set_error(error, "cannot open verified XFS stage: %s", strerror(errno)); return -1; }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT inode,target_start,target_blocks,reserve,digest FROM objects ORDER BY inode",
            -1, &stmt, NULL) != SQLITE_OK) {
        close(fd); xfs_catalogue_free(verified); return sqlite_fail(db, error, "cannot query XFS verification objects");
    }
    int result = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        uint64_t inode = (uint64_t)sqlite3_column_int64(stmt, 0);
        uint64_t target_start = (uint64_t)sqlite3_column_int64(stmt, 1);
        uint64_t target_blocks = (uint64_t)sqlite3_column_int64(stmt, 2);
        uint64_t reserve = (uint64_t)sqlite3_column_int64(stmt, 3);
        const char *wanted_digest = (const char *)sqlite3_column_text(stmt, 4);
        const XfsObject *item = find_object(verified, inode);
        if (item == NULL) { xfs_set_error(error, "arranged XFS inode %" PRIu64 " disappeared", inode); result = -1; break; }
        uint64_t expected = target_start;
        uint64_t seen = 0;
        for (size_t extent_index = 0;
             extent_index < item->extents.count; ++extent_index) {
            XfsExtent extent = item->extents.items[extent_index];
            if (extent.physical != expected ||
                UINT64_MAX - expected < extent.length ||
                UINT64_MAX - seen < extent.length) {
                xfs_set_error(error,
                              "XFS inode %" PRIu64
                              " did not retain its canonical run",
                              inode);
                result = -1;
                break;
            }
            expected += extent.length;
            seen += extent.length;
        }
        if (result != 0) break;
        if (seen != target_blocks) { xfs_set_error(error, "XFS inode %" PRIu64 " changed allocated length", inode); result = -1; break; }
        char digest[65];
        if (digest_object(fd, verified->geometry.block_size, item, digest, error) != 0) { result = -1; break; }
        if (wanted_digest == NULL || strcmp(digest, wanted_digest) != 0) { xfs_set_error(error, "XFS inode %" PRIu64 " payload checksum changed", inode); result = -1; break; }
        if (growth && reserve != 0 && !xfs_range_contains(&verified->free_ranges,
                target_start + target_blocks, target_start + target_blocks + reserve)) {
            xfs_set_error(error, "XFS inode %" PRIu64 " lost its exact 10%% growth reserve", inode); result = -1; break;
        }
    }
    sqlite3_finalize(stmt);
    close(fd);
    if (result != 0) xfs_catalogue_free(verified);
    return result;
}
