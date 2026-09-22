// SPDX-License-Identifier: GPL-3.0-or-later
#include "ext_workspace.h"

#include "ld_io.h"
#include "ld_runtime.h"
#include "ld_stop.h"

#include <errno.h>
#include <inttypes.h>
#include <openssl/sha.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EXT_WORKSPACE_COPY_CHUNK (4U * 1024U * 1024U)
#define EXT_WORKSPACE_TRANSACTION_BYTES (64U * 1024U * 1024U)

static int workspace_sql_error(sqlite3 *db, char **error, const char *action) {
    ext_set_error(error, "%s: %s", action, sqlite3_errmsg(db));
    return -1;
}

static int workspace_sql_exec(sqlite3 *db, const char *sql, char **error) {
    char *message = NULL;
    int code = sqlite3_exec(db, sql, NULL, NULL, &message);
    if (code != SQLITE_OK) {
        ext_set_error(error, "EXT workspace database: %s",
                      message != NULL ? message : sqlite3_errmsg(db));
        sqlite3_free(message);
        return -1;
    }
    return 0;
}

static int original_allocated(ExtFs *fs, const ExtGeometry *geometry,
                              uint64_t block, bool *allocated, char **error) {
    if (block == 0U || block < geometry->first_data_block) {
        *allocated = true;
        return 0;
    }
    return ext_fs_block_allocated(fs, block, allocated, error);
}

static int allocated_block_count(ExtFs *fs, const ExtGeometry *geometry,
                                 uint64_t *count, char **error) {
    *count = 0U;
    for (uint64_t block = 0U; block < geometry->total_blocks; ++block) {
        bool allocated = false;
        if (original_allocated(fs, geometry, block, &allocated, error) != 0)
            return -1;
        if (allocated) (*count)++;
    }
    return 0;
}

static int plan_range_is_unused(sqlite3 *db, uint64_t start, uint64_t end,
                                bool *unused, char **error) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT "
        "(SELECT COUNT(*) FROM blocks WHERE target>=?1 AND target<?2) + "
        "(SELECT COUNT(*) FROM reserves WHERE start<?2 AND start+length>?1)";
    if (start > INT64_MAX || end > INT64_MAX ||
        sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return workspace_sql_error(db, error, "checking EXT workspace target overlap");
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)start);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)end);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return workspace_sql_error(db, error, "reading EXT workspace target overlap");
    }
    *unused = sqlite3_column_int64(stmt, 0) == 0;
    sqlite3_finalize(stmt);
    return 0;
}

static int find_high_workspace(ExtFs *fs, sqlite3 *db,
                               const ExtGeometry *geometry, uint64_t needed,
                               uint64_t *start_out, char **error) {
    if (needed == 0 || needed > geometry->free_blocks) return EXT_WORKSPACE_UNAVAILABLE;
    uint64_t run_end = geometry->total_blocks;
    uint64_t cursor = geometry->total_blocks;
    bool in_run = false;
    while (cursor > geometry->first_data_block) {
        cursor--;
        bool allocated = false;
        if (original_allocated(fs, geometry, cursor, &allocated, error) != 0)
            return -1;
        bool free_block = !allocated;
        if (free_block) {
            if (!in_run) {
                run_end = cursor + 1U;
                in_run = true;
            }
        } else if (in_run) {
            uint64_t run_start = cursor + 1U;
            uint64_t length = run_end - run_start;
            if (length >= needed) {
                uint64_t candidate = run_end - needed;
                bool unused = false;
                if (plan_range_is_unused(db, candidate, run_end, &unused, error) != 0)
                    return -1;
                if (unused) {
                    *start_out = candidate;
                    return 0;
                }
            }
            in_run = false;
        }
    }
    if (in_run) {
        uint64_t run_start = geometry->first_data_block;
        uint64_t length = run_end - run_start;
        if (length >= needed) {
            uint64_t candidate = run_end - needed;
            bool unused = false;
            if (plan_range_is_unused(db, candidate, run_end, &unused, error) != 0)
                return -1;
            if (unused) {
                *start_out = candidate;
                return 0;
            }
        }
    }
    return EXT_WORKSPACE_UNAVAILABLE;
}

static uint64_t bounded_batch_blocks(uint32_t block_size,
                                     uint64_t requested) {
    uint64_t cap = EXT_WORKSPACE_TRANSACTION_BYTES / (uint64_t)block_size;
    if (cap == 0) cap = 1;
    if (requested == 0 || requested > cap) return cap;
    return requested;
}

static int set_state_u64(sqlite3 *db, const char *key, uint64_t value,
                         char **error) {
    sqlite3_stmt *stmt = NULL;
    if (value > INT64_MAX ||
        sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO direct_state(key,value) VALUES (?,?)",
            -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return workspace_sql_error(db, error, "preparing EXT workspace state");
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)value);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return workspace_sql_error(db, error, "recording EXT workspace state");
    }
    sqlite3_finalize(stmt);
    return 0;
}

static int get_state_u64(sqlite3 *db, const char *key, uint64_t *value,
                         char **error) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT value FROM direct_state WHERE key=?", -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return workspace_sql_error(db, error, "preparing EXT workspace state read");
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        ext_set_error(error, "EXT workspace state is incomplete (%s)", key);
        return -1;
    }
    sqlite3_int64 raw = sqlite3_column_int64(stmt, 0);
    if (raw < 0) {
        sqlite3_finalize(stmt);
        ext_set_error(error, "EXT workspace state is invalid (%s)", key);
        return -1;
    }
    *value = (uint64_t)raw;
    sqlite3_finalize(stmt);
    return 0;
}

int ext_workspace_prepare(ExtFs *fs, sqlite3 *db,
                          const ExtGeometry *geometry,
                          uint64_t requested_batch_blocks,
                          ExtWorkspace *workspace, char **error) {
    memset(workspace, 0, sizeof(*workspace));
    uint64_t needed = 0U;
    if (allocated_block_count(fs, geometry, &needed, error) != 0)
        return -1;
    uint64_t start = 0;
    int found = find_high_workspace(fs, db, geometry, needed, &start, error);
    if (found != 0) return found;

    if (workspace_sql_exec(db,
        "CREATE TABLE IF NOT EXISTS direct_workspace("
        "original INTEGER PRIMARY KEY,slot INTEGER UNIQUE NOT NULL);"
        "CREATE TABLE IF NOT EXISTS direct_state("
        "key TEXT PRIMARY KEY,value BLOB NOT NULL);"
        "DELETE FROM direct_workspace;DELETE FROM direct_state;BEGIN IMMEDIATE;",
        error) != 0) return -1;

    sqlite3_stmt *insert = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO direct_workspace(original,slot) VALUES (?,?)",
            -1, &insert, NULL) != SQLITE_OK) {
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return workspace_sql_error(db, error, "preparing EXT workspace map");
    }
    uint64_t index = 0;
    for (uint64_t block = 0; block < geometry->total_blocks; ++block) {
        bool allocated = false;
        if (original_allocated(fs, geometry, block, &allocated, error) != 0) {
            sqlite3_finalize(insert);
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }
        if (!allocated) continue;
        uint64_t slot = start + index;
        if (block > INT64_MAX || slot > INT64_MAX) {
            sqlite3_finalize(insert);
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            ext_set_error(error, "EXT workspace block number exceeds SQLite limits");
            return -1;
        }
        sqlite3_reset(insert);
        sqlite3_clear_bindings(insert);
        sqlite3_bind_int64(insert, 1, (sqlite3_int64)block);
        sqlite3_bind_int64(insert, 2, (sqlite3_int64)slot);
        if (sqlite3_step(insert) != SQLITE_DONE) {
            sqlite3_finalize(insert);
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return workspace_sql_error(db, error, "recording EXT workspace map");
        }
        index++;
    }
    sqlite3_finalize(insert);
    if (index != needed || workspace_sql_exec(db, "COMMIT", error) != 0) {
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        if (error != NULL && *error == NULL)
            ext_set_error(error, "EXT workspace allocation count changed while planning");
        return -1;
    }

    workspace->start = start;
    workspace->blocks = needed;
    workspace->block_size = geometry->block_size;
    workspace->batch_blocks = bounded_batch_blocks(geometry->block_size,
                                                    requested_batch_blocks);
    if (set_state_u64(db, "workspace_start", workspace->start, error) != 0 ||
        set_state_u64(db, "workspace_blocks", workspace->blocks, error) != 0 ||
        set_state_u64(db, "block_size", workspace->block_size, error) != 0 ||
        set_state_u64(db, "batch_blocks", workspace->batch_blocks, error) != 0)
        return -1;
    return 0;
}

int ext_workspace_load(sqlite3 *db, ExtWorkspace *workspace, char **error) {
    uint64_t block_size = 0;
    memset(workspace, 0, sizeof(*workspace));
    if (get_state_u64(db, "workspace_start", &workspace->start, error) != 0 ||
        get_state_u64(db, "workspace_blocks", &workspace->blocks, error) != 0 ||
        get_state_u64(db, "block_size", &block_size, error) != 0 ||
        get_state_u64(db, "batch_blocks", &workspace->batch_blocks, error) != 0)
        return -1;
    if (block_size == 0 || block_size > UINT32_MAX || workspace->batch_blocks == 0) {
        ext_set_error(error, "EXT workspace geometry is invalid");
        return -1;
    }
    workspace->block_size = (uint32_t)block_size;
    return 0;
}

static int copy_bytes(int fd, uint64_t source_offset, uint64_t destination_offset,
                      uint64_t length, uint8_t *buffer, bool allow_stop,
                      char **error) {
    uint64_t done = 0;
    while (done < length) {
        size_t amount = (size_t)((length - done) > EXT_WORKSPACE_COPY_CHUNK
                                     ? EXT_WORKSPACE_COPY_CHUNK
                                     : (length - done));
        ssize_t got = ld_pread_full(fd, buffer, amount, source_offset + done);
        if (got < 0 || (size_t)got != amount) {
            ext_set_error(error, "short read while copying the EXT safety workspace");
            return -1;
        }
        ssize_t wrote = ld_pwrite_full(fd, buffer, amount, destination_offset + done);
        if (wrote < 0 || (size_t)wrote != amount) {
            ext_set_error(error, "short write while copying the EXT safety workspace");
            return -1;
        }
        done += amount;
        if (allow_stop && ld_stop_requested()) return -2;
    }
    return 0;
}

static int copy_run(int fd, uint32_t block_size, uint64_t source,
                    uint64_t destination, uint64_t blocks,
                    uint64_t batch_blocks, uint8_t *buffer,
                    bool allow_stop, uint64_t *completed,
                    uint64_t total, const char *label, char **error) {
    uint64_t offset = 0;
    while (offset < blocks) {
        uint64_t amount = blocks - offset;
        if (amount > batch_blocks) amount = batch_blocks;
        uint64_t bytes = amount * (uint64_t)block_size;
        int copied = copy_bytes(fd,
            (source + offset) * (uint64_t)block_size,
            (destination + offset) * (uint64_t)block_size,
            bytes, buffer, allow_stop, error);
        if (copied != 0) return copied;
        if (fsync(fd) != 0) {
            ext_set_error(error, "syncing EXT %s failed: %s", label, strerror(errno));
            return -1;
        }
        offset += amount;
        *completed += amount;
        printf("EXT %s: %" PRIu64 " of %" PRIu64 " blocks durable.\n",
               label, *completed, total);
        fflush(stdout);
        if (allow_stop && ld_stop_requested()) return -2;
    }
    return 0;
}

static int copy_workspace_runs(int fd, sqlite3 *db,
                               const ExtWorkspace *workspace,
                               bool restore, bool allow_stop,
                               const char *label, char **error) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT original,slot FROM direct_workspace ORDER BY original",
            -1, &stmt, NULL) != SQLITE_OK)
        return workspace_sql_error(db, error, "reading EXT workspace runs");
    uint8_t *buffer = ld_xmalloc(EXT_WORKSPACE_COPY_CHUNK);
    bool have_run = false;
    uint64_t run_source = 0, run_destination = 0, run_blocks = 0;
    uint64_t completed = 0;
    int result = 0;
    int state;
    while ((state = sqlite3_step(stmt)) == SQLITE_ROW) {
        uint64_t original = (uint64_t)sqlite3_column_int64(stmt, 0);
        uint64_t slot = (uint64_t)sqlite3_column_int64(stmt, 1);
        uint64_t source = restore ? slot : original;
        uint64_t destination = restore ? original : slot;
        if (!have_run) {
            run_source = source;
            run_destination = destination;
            run_blocks = 1;
            have_run = true;
        } else if (source == run_source + run_blocks &&
                   destination == run_destination + run_blocks) {
            run_blocks++;
        } else {
            result = copy_run(fd, workspace->block_size, run_source,
                              run_destination, run_blocks,
                              workspace->batch_blocks, buffer, allow_stop,
                              &completed, workspace->blocks, label, error);
            if (result != 0) break;
            run_source = source;
            run_destination = destination;
            run_blocks = 1;
        }
    }
    if (result == 0 && state != SQLITE_DONE)
        result = workspace_sql_error(db, error, "iterating EXT workspace runs");
    if (result == 0 && have_run)
        result = copy_run(fd, workspace->block_size, run_source,
                          run_destination, run_blocks,
                          workspace->batch_blocks, buffer, allow_stop,
                          &completed, workspace->blocks, label, error);
    free(buffer);
    sqlite3_finalize(stmt);
    return result;
}

static int workspace_digest(int fd, sqlite3 *db, uint32_t block_size,
                            bool use_workspace,
                            uint8_t output[SHA256_DIGEST_LENGTH], char **error) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT original,slot FROM direct_workspace ORDER BY original",
            -1, &stmt, NULL) != SQLITE_OK)
        return workspace_sql_error(db, error, "preparing EXT workspace checksum");
    SHA256_CTX digest;
    if (SHA256_Init(&digest) != 1) {
        sqlite3_finalize(stmt);
        ext_set_error(error, "initializing EXT workspace checksum failed");
        return -1;
    }
    uint8_t *buffer = ld_xmalloc(block_size);
    int state;
    while ((state = sqlite3_step(stmt)) == SQLITE_ROW) {
        uint64_t original = (uint64_t)sqlite3_column_int64(stmt, 0);
        uint64_t slot = (uint64_t)sqlite3_column_int64(stmt, 1);
        uint64_t block = use_workspace ? slot : original;
        ssize_t got = ld_pread_full(fd, buffer, block_size,
                                    block * (uint64_t)block_size);
        if (got < 0 || (size_t)got != block_size) {
            free(buffer);
            sqlite3_finalize(stmt);
            ext_set_error(error, "short read while checksumming the EXT workspace");
            return -1;
        }
        uint8_t identity[8];
        for (unsigned byte = 0; byte < 8U; ++byte)
            identity[byte] = (uint8_t)(original >> (byte * 8U));
        if (SHA256_Update(&digest, identity, sizeof(identity)) != 1 ||
            SHA256_Update(&digest, buffer, block_size) != 1) {
            free(buffer);
            sqlite3_finalize(stmt);
            ext_set_error(error, "updating EXT workspace checksum failed");
            return -1;
        }
    }
    free(buffer);
    sqlite3_finalize(stmt);
    if (state != SQLITE_DONE || SHA256_Final(output, &digest) != 1) {
        if (state != SQLITE_DONE)
            return workspace_sql_error(db, error, "reading EXT workspace checksum map");
        ext_set_error(error, "finalizing EXT workspace checksum failed");
        return -1;
    }
    return 0;
}

static int store_workspace_digest(sqlite3 *db,
                                  const uint8_t digest[SHA256_DIGEST_LENGTH],
                                  char **error) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO direct_state(key,value) VALUES ('workspace_sha256',?)",
            -1, &stmt, NULL) != SQLITE_OK)
        return workspace_sql_error(db, error, "preparing EXT workspace checksum state");
    sqlite3_bind_blob(stmt, 1, digest, SHA256_DIGEST_LENGTH, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return workspace_sql_error(db, error, "recording EXT workspace checksum");
    }
    sqlite3_finalize(stmt);
    return 0;
}

static int verify_stored_workspace_digest(int fd, sqlite3 *db,
                                          uint32_t block_size, char **error) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT value FROM direct_state WHERE key='workspace_sha256'",
            -1, &stmt, NULL) != SQLITE_OK)
        return workspace_sql_error(db, error, "preparing EXT workspace checksum recovery");
    if (sqlite3_step(stmt) != SQLITE_ROW ||
        sqlite3_column_bytes(stmt, 0) != SHA256_DIGEST_LENGTH) {
        sqlite3_finalize(stmt);
        ext_set_error(error, "EXT workspace recovery checksum is missing");
        return -1;
    }
    uint8_t expected[SHA256_DIGEST_LENGTH];
    memcpy(expected, sqlite3_column_blob(stmt, 0), sizeof(expected));
    sqlite3_finalize(stmt);
    uint8_t actual[SHA256_DIGEST_LENGTH];
    if (workspace_digest(fd, db, block_size, true, actual, error) != 0)
        return -1;
    if (memcmp(expected, actual, sizeof(expected)) != 0) {
        ext_set_error(error, "EXT durable workspace checksum changed; refusing unsafe recovery");
        return -1;
    }
    return 0;
}

int ext_workspace_stage(int fd, sqlite3 *db, const ExtWorkspace *workspace,
                        char **error) {
    int result = copy_workspace_runs(fd, db, workspace, false, true,
                                     "workspace staging", error);
    if (result != 0) return result;
    uint8_t original[SHA256_DIGEST_LENGTH];
    uint8_t staged[SHA256_DIGEST_LENGTH];
    if (workspace_digest(fd, db, workspace->block_size, false, original, error) != 0 ||
        workspace_digest(fd, db, workspace->block_size, true, staged, error) != 0)
        return -1;
    if (memcmp(original, staged, sizeof(original)) != 0) {
        ext_set_error(error, "EXT safety workspace verification failed");
        return -1;
    }
    if (store_workspace_digest(db, staged, error) != 0) return -1;
    printf("EXT workspace staging complete: %" PRIu64
           " original allocated blocks durably copied and checksummed; source metadata is still unchanged.\n",
           workspace->blocks);
    fflush(stdout);
    return 0;
}

static int flush_target_run(int fd, const ExtWorkspace *workspace,
                            uint8_t *buffer, uint64_t start,
                            uint64_t count, uint64_t *completed,
                            uint64_t total, char **error) {
    if (count == 0) return 0;
    uint64_t bytes = count * (uint64_t)workspace->block_size;
    ssize_t wrote = ld_pwrite_full(fd, buffer, (size_t)bytes,
                                   start * (uint64_t)workspace->block_size);
    if (wrote < 0 || (uint64_t)wrote != bytes) {
        ext_set_error(error, "short write placing EXT canonical target blocks");
        return -1;
    }
    if (fsync(fd) != 0) {
        ext_set_error(error, "syncing EXT canonical placement failed: %s", strerror(errno));
        return -1;
    }
    *completed += count;
    printf("EXT canonical placement: %" PRIu64 " of %" PRIu64 " moved blocks durable.\n",
           *completed, total);
    fflush(stdout);
    if (ld_stop_requested()) return -2;
    return 0;
}

int ext_workspace_place(int fd, sqlite3 *db, const ExtWorkspace *workspace,
                        char **error) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT b.target,w.slot FROM blocks b "
        "JOIN direct_workspace w ON w.original=b.old "
        "WHERE b.old<>b.target ORDER BY b.target";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return workspace_sql_error(db, error, "preparing EXT canonical workspace placement");
    uint64_t buffer_bytes = workspace->batch_blocks * (uint64_t)workspace->block_size;
    if (buffer_bytes == 0 || buffer_bytes > SIZE_MAX) {
        sqlite3_finalize(stmt);
        ext_set_error(error, "EXT workspace RAM buffer size is invalid");
        return -1;
    }
    uint8_t *buffer = ld_xmalloc((size_t)buffer_bytes);
    uint64_t run_start = 0, run_count = 0, previous_target = 0;
    uint64_t completed = 0, total = 0;
    sqlite3_stmt *count_stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM blocks WHERE old<>target",
            -1, &count_stmt, NULL) != SQLITE_OK ||
        sqlite3_step(count_stmt) != SQLITE_ROW) {
        sqlite3_finalize(count_stmt);
        free(buffer);
        sqlite3_finalize(stmt);
        return workspace_sql_error(db, error, "counting EXT workspace placements");
    }
    total = (uint64_t)sqlite3_column_int64(count_stmt, 0);
    sqlite3_finalize(count_stmt);

    int result = 0;
    int state;
    while ((state = sqlite3_step(stmt)) == SQLITE_ROW) {
        uint64_t target = (uint64_t)sqlite3_column_int64(stmt, 0);
        uint64_t slot = (uint64_t)sqlite3_column_int64(stmt, 1);
        if (run_count != 0 &&
            (target != previous_target + 1U || run_count == workspace->batch_blocks)) {
            result = flush_target_run(fd, workspace, buffer, run_start, run_count,
                                      &completed, total, error);
            if (result != 0) break;
            run_count = 0;
        }
        if (run_count == 0) run_start = target;
        ssize_t got = ld_pread_full(fd,
            buffer + (size_t)(run_count * (uint64_t)workspace->block_size),
            workspace->block_size,
            slot * (uint64_t)workspace->block_size);
        if (got < 0 || (size_t)got != workspace->block_size) {
            ext_set_error(error, "short read from the EXT durable workspace");
            result = -1;
            break;
        }
        run_count++;
        previous_target = target;
    }
    if (result == 0 && state != SQLITE_DONE)
        result = workspace_sql_error(db, error, "reading EXT canonical workspace placement");
    if (result == 0 && run_count != 0)
        result = flush_target_run(fd, workspace, buffer, run_start, run_count,
                                  &completed, total, error);
    free(buffer);
    sqlite3_finalize(stmt);
    return result;
}

int ext_workspace_restore(int fd, sqlite3 *db, const ExtWorkspace *workspace,
                          char **error) {
    if (verify_stored_workspace_digest(fd, db, workspace->block_size, error) != 0)
        return -1;
    int result = copy_workspace_runs(fd, db, workspace, true, false,
                                     "workspace restore", error);
    if (result == 0) {
        puts("EXT durable workspace restore completed; original allocated blocks are back in place.");
        fflush(stdout);
    }
    return result;
}
