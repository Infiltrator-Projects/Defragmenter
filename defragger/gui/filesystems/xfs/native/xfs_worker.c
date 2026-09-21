// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Defragmenter native XFS worker
 * Author: Shannon Smith
 *
 * The authoritative XFS plugin owns this C engine.  It performs read-only
 * identification/analysis and staged raw offline mutation without mounting XFS,
 * issuing XFS filesystem ioctls, or invoking xfsprogs.
 */

#include "xfs_native.h"
#include "ld_device.h"
#include "ld_io.h"
#include "ld_runtime.h"
#include "ld_protocol.h"
#include "ld_path.h"

#include "infiltratr/core.h"
#include "infiltratr/config.h"
#include "infiltratr/posix.h"
#include "ld_stop.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define PROGRAM_NAME "linux-defragger-xfs-worker"
#define COPY_CHUNK (4U * 1024U * 1024U)
#define JOURNAL_INTERVAL (64U * 1024U * 1024U)
#define JOURNAL_MAGIC "LINUX-DEFRAGGER-XFS-JOURNAL-1"

typedef struct {
    char *device;
    char *target_identity;
    char uuid[33];
    char operation[24];
    char phase[32];
    char *stage;
    char *plan;
    uint64_t physical_bytes;
    uint64_t filesystem_bytes;
    uint64_t commit_offset;
    uint64_t movable_blocks;
    uint64_t move_blocks;
} XfsJournal;

static void usage(FILE *stream) {
    fprintf(stream,
            "Usage:\n"
            "  %s identify DEVICE\n"
            "  %s analyse-json DEVICE\n"
            "  %s defrag|growth-defrag|recover DEVICE --write --confirm DEVICE --journal PATH [options]\n",
            PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME);
}



static void uuid_hex(const uint8_t uuid[16], char out[33]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t index = 0; index < 16; ++index) {
        out[index * 2] = digits[uuid[index] >> 4];
        out[index * 2 + 1] = digits[uuid[index] & 15U];
    }
    out[32] = '\0';
}



static int ensure_directory_tree(const char *path, char **error) {
    if (ld_path_ensure_trusted_directory_tree(path) != 0) {
        xfs_set_error(error, "cannot create XFS journal directory %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}


static void journal_free(XfsJournal *state) {
    free(state->device);
    free(state->target_identity);
    free(state->stage);
    free(state->plan);
    memset(state, 0, sizeof(*state));
}

static bool safe_journal_value(const char *value) {
    return value != NULL && strchr(value, '\n') == NULL && strchr(value, '\r') == NULL;
}

static bool journal_write_stream(FILE *file, const void *user_data) {
    const XfsJournal *state = user_data;
    fprintf(file, "%s\n", JOURNAL_MAGIC);
    fprintf(file, "version=%s\n", LD_VERSION);
    fprintf(file, "filesystem=xfs\n");
    fprintf(file, "device=%s\n", state->device);
    fprintf(file, "target_identity=%s\n", state->target_identity);
    fprintf(file, "uuid=%s\n", state->uuid);
    fprintf(file, "operation=%s\n", state->operation);
    fprintf(file, "phase=%s\n", state->phase);
    fprintf(file, "stage=%s\n", state->stage);
    fprintf(file, "plan=%s\n", state->plan);
    fprintf(file, "physical_bytes=%" PRIu64 "\n", state->physical_bytes);
    fprintf(file, "filesystem_bytes=%" PRIu64 "\n", state->filesystem_bytes);
    fprintf(file, "commit_offset=%" PRIu64 "\n", state->commit_offset);
    fprintf(file, "movable_blocks=%" PRIu64 "\n", state->movable_blocks);
    fprintf(file, "move_blocks=%" PRIu64 "\n", state->move_blocks);
    return !ferror(file);
}

static int journal_save(const char *path, const XfsJournal *state, char **error) {
    if (!safe_journal_value(state->device) || !safe_journal_value(state->target_identity) ||
        !safe_journal_value(state->stage) || !safe_journal_value(state->plan)) {
        xfs_set_error(error, "XFS journal path or identity contains an unsupported newline");
        return -1;
    }
    char *parent = ld_path_parent_directory(path);
    if (ensure_directory_tree(parent, error) != 0) { free(parent); return -1; }
    free(parent);
    const int failure = infiltratr_atomic_file_write(
        path, INFILTRATR_ATOMIC_FILE_PRIVATE, journal_write_stream, state);
    if (failure != 0) {
        xfs_set_error(error, "cannot install XFS transaction journal: %s",
                      strerror(failure));
        return -1;
    }
    return 0;
}

static char *value_copy(const char *value) {
    return ld_xstrdup(value == NULL ? "" : value);
}

static int parse_u64(const char *text, uint64_t *out) {
    return infiltratr_parse_u64(text, 10U, out) ? 0 : -1;
}

static int journal_load(const char *path, XfsJournal *state, char **error) {
    memset(state, 0, sizeof(*state));
    FILE *file = fopen(path, "r");
    if (file == NULL) { xfs_set_error(error, "cannot open XFS transaction journal: %s", strerror(errno)); return -1; }
    char *line = NULL;
    size_t capacity = 0;
    if (getline(&line, &capacity, file) < 0) {
        xfs_set_error(error, "XFS transaction journal is empty");
        free(line); fclose(file); return -1;
    }
    infiltratr_trim_line_end(line);
    if (strcmp(line, JOURNAL_MAGIC) != 0) {
        xfs_set_error(error, "XFS transaction journal has an invalid header");
        free(line); fclose(file); return -1;
    }
    bool filesystem_ok = false;
    while (getline(&line, &capacity, file) >= 0) {
        char *key = NULL;
        char *equals = NULL;
        if (infiltratr_config_parse_line(line, &key, &equals) !=
            INFILTRATR_CONFIG_LINE_ENTRY)
            continue;
        if (strcmp(key, "filesystem") == 0) filesystem_ok = strcmp(equals, "xfs") == 0;
        else if (strcmp(key, "device") == 0) { free(state->device); state->device = value_copy(equals); }
        else if (strcmp(key, "target_identity") == 0) { free(state->target_identity); state->target_identity = value_copy(equals); }
        else if (strcmp(key, "uuid") == 0) infiltratr_copy_string(state->uuid, sizeof(state->uuid), equals);
        else if (strcmp(key, "operation") == 0) infiltratr_copy_string(state->operation, sizeof(state->operation), equals);
        else if (strcmp(key, "phase") == 0) infiltratr_copy_string(state->phase, sizeof(state->phase), equals);
        else if (strcmp(key, "stage") == 0) { free(state->stage); state->stage = value_copy(equals); }
        else if (strcmp(key, "plan") == 0) { free(state->plan); state->plan = value_copy(equals); }
        else if (strcmp(key, "physical_bytes") == 0 && parse_u64(equals, &state->physical_bytes) != 0) goto invalid;
        else if (strcmp(key, "filesystem_bytes") == 0 && parse_u64(equals, &state->filesystem_bytes) != 0) goto invalid;
        else if (strcmp(key, "commit_offset") == 0 && parse_u64(equals, &state->commit_offset) != 0) goto invalid;
        else if (strcmp(key, "movable_blocks") == 0 && parse_u64(equals, &state->movable_blocks) != 0) goto invalid;
        else if (strcmp(key, "move_blocks") == 0 && parse_u64(equals, &state->move_blocks) != 0) goto invalid;
    }
    free(line); fclose(file);
    if (!filesystem_ok || state->device == NULL || state->target_identity == NULL ||
        strlen(state->uuid) != 32 || state->operation[0] == '\0' || state->phase[0] == '\0' ||
        state->stage == NULL || state->plan == NULL || state->physical_bytes == 0 || state->filesystem_bytes == 0) {
        xfs_set_error(error, "XFS transaction journal is incomplete or corrupt");
        journal_free(state);
        return -1;
    }
    return 0;
invalid:
    xfs_set_error(error, "XFS transaction journal contains an invalid numeric field");
    free(line); fclose(file); journal_free(state); return -1;
}

static int journal_phase(const char *path, XfsJournal *state, const char *phase, char **error) {
    infiltratr_copy_string(state->phase, sizeof(state->phase), phase);
    return journal_save(path, state, error);
}

static void unlink_if_exists(const char *path) {
    if (path == NULL || *path == '\0') return;
    const int failure = infiltratr_unlink_durable(path, true);
    if (failure != 0)
        fprintf(stderr, "%s: warning: cannot durably remove %s: %s\n",
                PROGRAM_NAME, path, strerror(failure));
}

static void transaction_cleanup(const char *journal, const XfsJournal *state) {
    if (state != NULL) {
        unlink_if_exists(state->stage);
        unlink_if_exists(state->plan);
        if (state->plan != NULL) {
            char *wal = ld_path_append_suffix(state->plan, "-wal");
            char *shm = ld_path_append_suffix(state->plan, "-shm");
            unlink_if_exists(wal); unlink_if_exists(shm);
            free(wal); free(shm);
        }
    }
    unlink_if_exists(journal);
}

static int target_identity(const char *path, char **identity, uint64_t *size, char **error) {
    LdDevice device;
    if (ld_device_try_open(path, false, &device) != 0) {
        xfs_set_error(error, "cannot inspect XFS target: %s", strerror(errno));
        return -1;
    }
    if (device.is_block && ld_device_number_is_mounted(device.device_number)) {
        ld_device_close(&device);
        xfs_set_error(error, "refusing raw XFS writing while the target or a related block device is mounted");
        return -1;
    }
    char buffer[160];
    if (ld_device_format_identity(&device, buffer, sizeof(buffer)) != 0) {
        const int failure = errno;
        ld_device_close(&device);
        xfs_set_error(error, "cannot identify XFS target: %s", strerror(failure));
        return -1;
    }
    *size = device.size_bytes;
    *identity = ld_xstrdup(buffer);
    ld_device_close(&device);
    return 0;
}

static char *canonical_path(const char *path, char **error) {
    char *resolved = realpath(path, NULL);
    if (resolved == NULL) xfs_set_error(error, "cannot resolve XFS target path: %s", strerror(errno));
    return resolved;
}

static int capacity_preflight(const char *journal, const XfsCatalogue *catalogue, char **error) {
    uint64_t used_blocks = 0U;
    for (size_t index = 0; index < catalogue->used_ranges.count; ++index) {
        const uint64_t blocks = catalogue->used_ranges.items[index].end -
                                catalogue->used_ranges.items[index].start;
        used_blocks = infiltratr_u64_add_saturating(used_blocks, blocks);
    }
    uint64_t stage_bytes = infiltratr_u64_multiply_saturating(
        used_blocks, catalogue->geometry.block_size);
    uint64_t movable = xfs_catalogue_movable_blocks(catalogue);
    uint64_t plan_bytes = infiltratr_u64_multiply_saturating(movable, 72U);
    plan_bytes = infiltratr_u64_add_saturating(
        plan_bytes, UINT64_C(64) * 1024U * 1024U);
    uint64_t repair_margin = stage_bytes / 20U;
    if (repair_margin < UINT64_C(256) * 1024U * 1024U)
        repair_margin = UINT64_C(256) * 1024U * 1024U;
    uint64_t required = infiltratr_u64_add_saturating(stage_bytes, plan_bytes);
    required = infiltratr_u64_add_saturating(required, repair_margin);
    char *parent = ld_path_parent_directory(journal);
    struct statvfs fs;
    if (statvfs(parent, &fs) != 0) {
        xfs_set_error(error, "cannot inspect XFS staging capacity in %s: %s", parent, strerror(errno));
        free(parent); return -1;
    }
    free(parent);
    uint64_t available = (uint64_t)fs.f_bavail * fs.f_frsize;
    if (available < required) {
        xfs_set_error(error, "XFS safe staging has %" PRIu64 " MB available; at least %" PRIu64 " MB is required",
                      available / (1024U * 1024U), required / (1024U * 1024U));
        return -1;
    }
    return 0;
}

static int create_stage(const char *source_path, const char *stage_path,
                        uint64_t physical_bytes, const XfsCatalogue *catalogue, char **error) {
    unlink_if_exists(stage_path);
    int source = open(source_path, O_RDONLY | O_CLOEXEC);
    if (source < 0) { xfs_set_error(error, "cannot open XFS source for staging: %s", strerror(errno)); return -1; }
    int stage = open(stage_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (stage < 0) { xfs_set_error(error, "cannot create XFS working image: %s", strerror(errno)); close(source); return -1; }
    int result = 0;
    if (ftruncate(stage, (off_t)physical_bytes) != 0) {
        xfs_set_error(error, "cannot size sparse XFS working image: %s", strerror(errno)); result = -1; goto done;
    }
    uint8_t *buffer = ld_xmalloc(COPY_CHUNK);
    uint64_t total_blocks = 0, copied_blocks = 0;
    for (size_t index = 0; index < catalogue->used_ranges.count; ++index)
        total_blocks += catalogue->used_ranges.items[index].end - catalogue->used_ranges.items[index].start;
    for (size_t index = 0; index < catalogue->used_ranges.count && result == 0; ++index) {
        XfsRange range = catalogue->used_ranges.items[index];
        uint64_t offset = range.start * catalogue->geometry.block_size;
        uint64_t remaining = (range.end - range.start) * catalogue->geometry.block_size;
        while (remaining != 0) {
            if (ld_stop_requested()) { xfs_set_error(error, "stop requested before XFS source commit"); result = -2; break; }
            size_t amount = remaining > COPY_CHUNK ? COPY_CHUNK : (size_t)remaining;
            ssize_t got = ld_pread_full(source, buffer, amount, offset);
            if (got < 0 || (size_t)got != amount) { xfs_set_error(error, "short read while cloning allocated XFS blocks"); result = -1; break; }
            ssize_t wrote = ld_pwrite_full(stage, buffer, amount, offset);
            if (wrote < 0 || (size_t)wrote != amount) { xfs_set_error(error, "short write while cloning allocated XFS blocks"); result = -1; break; }
            offset += amount; remaining -= amount; copied_blocks += amount / catalogue->geometry.block_size;
            if (copied_blocks != 0 && copied_blocks % 65536U == 0) {
                printf("XFS working-image clone: %" PRIu64 " of %" PRIu64 " blocks.\n", copied_blocks, total_blocks);
                fflush(stdout);
            }
        }
    }
    free(buffer);
    if (result == 0 && fsync(stage) != 0) { xfs_set_error(error, "cannot sync XFS working image: %s", strerror(errno)); result = -1; }
done:
    close(stage); close(source);
    if (result != 0) unlink_if_exists(stage_path);
    return result;
}

static bool same_uuid(const XfsCatalogue *catalogue, const char *hex) {
    char current[33]; uuid_hex(catalogue->geometry.uuid, current);
    return strcmp(current, hex) == 0;
}

static int check_unchanged_target(const char *device, const XfsJournal *state,
                                  uint64_t expected_dblocks, char **error) {
    char *identity = NULL;
    uint64_t size = 0;
    if (target_identity(device, &identity, &size, error) != 0) return -1;
    bool identity_ok = strcmp(identity, state->target_identity) == 0 && size == state->physical_bytes;
    free(identity);
    if (!identity_ok) { xfs_set_error(error, "the XFS target identity or size changed while its working image was prepared"); return -1; }
    XfsCatalogue current;
    if (xfs_scan_catalogue(device, true, &current, error) != 0) return -1;
    bool ok = same_uuid(&current, state->uuid) && current.geometry.dblocks == expected_dblocks;
    xfs_catalogue_free(&current);
    if (!ok) { xfs_set_error(error, "the XFS target changed while its working image was prepared"); return -1; }
    return 0;
}

static uint64_t commit_range_bytes(const XfsCatalogue *catalogue) {
    uint64_t total = 0U;
    for (size_t index = 0; index < catalogue->used_ranges.count; ++index) {
        uint64_t blocks = catalogue->used_ranges.items[index].end - catalogue->used_ranges.items[index].start;
        uint64_t bytes = infiltratr_u64_multiply_saturating(
            blocks, (uint64_t)catalogue->geometry.block_size);
        if (bytes == UINT64_MAX ||
            !infiltratr_u64_add_checked(total, bytes, &total))
            return UINT64_MAX;
    }
    return total;
}

static uint64_t committed_range_bytes_before(const XfsCatalogue *catalogue, uint64_t offset) {
    uint64_t complete = 0U;
    const uint64_t block_size = catalogue->geometry.block_size;
    for (size_t index = 0; index < catalogue->used_ranges.count; ++index) {
        uint64_t start = catalogue->used_ranges.items[index].start * block_size;
        uint64_t end = catalogue->used_ranges.items[index].end * block_size;
        if (offset <= start) break;
        uint64_t upto = offset < end ? offset : end;
        if (upto > start) complete += upto - start;
        if (offset < end) break;
    }
    return complete;
}

static int commit_stage(const char *device_path, const char *journal_path, XfsJournal *state,
                        const XfsCatalogue *catalogue, uint64_t start_offset, char **error) {
    char *identity = NULL;
    uint64_t size = 0;
    if (target_identity(device_path, &identity, &size, error) != 0) return -1;
    if (strcmp(identity, state->target_identity) != 0 || size != state->physical_bytes) {
        free(identity);
        xfs_set_error(error, "target identity or size changed before the XFS commit");
        return -1;
    }
    free(identity);
    if (catalogue == NULL || catalogue->geometry.block_size == 0U) {
        xfs_set_error(error, "verified XFS commit catalogue is unavailable");
        return -1;
    }

    LdDevice target;
    if (ld_device_try_open(device_path, true, &target) != 0) {
        xfs_set_error(error,
                      "cannot open the journal-bound XFS target for commit: %s",
                      strerror(errno));
        return -1;
    }
    if (!ld_device_matches_identity(&target, state->target_identity,
                                    state->physical_bytes)) {
        ld_device_close(&target);
        xfs_set_error(error,
                      "XFS target identity or capacity changed before commit");
        return -1;
    }
    if (flock(target.fd, LOCK_EX) != 0) {
        xfs_set_error(error, "cannot lock XFS target for commit: %s", strerror(errno));
        ld_device_close(&target);
        return -1;
    }
    int stage = open(state->stage, O_RDONLY | O_CLOEXEC);
    if (stage < 0) {
        xfs_set_error(error, "cannot open verified XFS working image: %s", strerror(errno));
        ld_device_close(&target);
        return -1;
    }
    if (start_offset > state->filesystem_bytes) {
        xfs_set_error(error, "XFS recovery journal has an invalid commit offset");
        close(stage);
        ld_device_close(&target);
        return -1;
    }

    const uint64_t total_bytes = commit_range_bytes(catalogue);
    if (total_bytes == UINT64_MAX) {
        xfs_set_error(error, "XFS allocated-range commit size overflow");
        close(stage);
        ld_device_close(&target);
        return -1;
    }
    uint64_t completed_bytes = committed_range_bytes_before(catalogue, start_offset);
    uint64_t journal_bytes = 0U;
    uint8_t *buffer = ld_xmalloc(COPY_CHUNK);
    bool stop_announced = false;
    int result = 0;

    printf("XFS source commit: writing %" PRIu64
           " MB of verified allocated ranges instead of rewriting the full %" PRIu64
           " MB filesystem.\n",
           total_bytes / (1024U * 1024U),
           state->filesystem_bytes / (1024U * 1024U));
    fflush(stdout);

    const uint64_t block_size = catalogue->geometry.block_size;
    for (size_t range_index = 0;
         range_index < catalogue->used_ranges.count && result == 0;
         ++range_index) {
        uint64_t range_start = catalogue->used_ranges.items[range_index].start * block_size;
        uint64_t range_end = catalogue->used_ranges.items[range_index].end * block_size;
        if (range_end <= start_offset) continue;
        uint64_t offset = range_start < start_offset ? start_offset : range_start;
        while (offset < range_end) {
            size_t amount = range_end - offset > COPY_CHUNK
                ? COPY_CHUNK
                : (size_t)(range_end - offset);
            ssize_t got = ld_pread_full(stage, buffer, amount, offset);
            if (got < 0 || (size_t)got != amount) {
                xfs_set_error(error, "XFS working image ended during allocated-range source commit");
                result = -1;
                break;
            }
            ssize_t wrote = ld_pwrite_full(target.fd, buffer, amount, offset);
            if (wrote < 0 || (size_t)wrote != amount) {
                xfs_set_error(error, "short write during recoverable XFS allocated-range commit");
                result = -1;
                break;
            }
            offset += amount;
            completed_bytes += amount;
            journal_bytes += amount;

            if (journal_bytes >= JOURNAL_INTERVAL) {
                if (fsync(target.fd) != 0) {
                    xfs_set_error(error, "cannot sync XFS source commit: %s", strerror(errno));
                    result = -1;
                    break;
                }
                state->commit_offset = offset;
                if (journal_save(journal_path, state, error) != 0) {
                    result = -1;
                    break;
                }
                journal_bytes = 0U;
                if (total_bytes != 0U) {
                    printf("%.2f percent completed\n",
                           100.0 * (double)completed_bytes / (double)total_bytes);
                }
                fflush(stdout);
            }
            if (ld_stop_requested() && !stop_announced) {
                puts("Stop requested after XFS commit began; completing the verified commit so the target is never left partially written.");
                fflush(stdout);
                stop_announced = true;
            }
        }
    }

    if (result == 0) {
        if (fsync(target.fd) != 0) {
            xfs_set_error(error, "cannot sync completed XFS source commit: %s", strerror(errno));
            result = -1;
        } else {
            state->commit_offset = state->filesystem_bytes;
            if (journal_save(journal_path, state, error) != 0) {
                result = -1;
            } else {
                puts("100.00 percent completed");
                fflush(stdout);
            }
        }
    }

    free(buffer);
    close(stage);
    ld_device_close(&target);
    return result;
}
static void emit_live_reset(const XfsCatalogue *catalogue, const char *scope) {
    printf("@@LIVE_RESET {\"scope\":\"%s\",\"unit_size\":%u,\"filesystem_units\":%" PRIu64 ",\"used_ranges\":[",
           scope, catalogue->geometry.block_size, catalogue->geometry.dblocks);
    for (size_t index = 0; index < catalogue->used_ranges.count; ++index) {
        if (index != 0) putchar(',');
        uint64_t start = catalogue->used_ranges.items[index].start * catalogue->geometry.block_size;
        uint64_t length = (catalogue->used_ranges.items[index].end - catalogue->used_ranges.items[index].start) * catalogue->geometry.block_size;
        printf("[%" PRIu64 ",%" PRIu64 "]", start, length);
    }
    puts("]}");
    fflush(stdout);
}

static int build_and_commit(const char *device, const char *operation, const char *journal_path,
                            bool live_updates, char **error) {
    char *identity = NULL, *real = NULL;
    uint64_t physical_bytes = 0;
    XfsCatalogue source = {0}, staged = {0}, verified = {0};
    XfsPlan plan = {0};
    sqlite3 *db = NULL;
    XfsJournal state = {0};
    int result = 1;
    if (target_identity(device, &identity, &physical_bytes, error) != 0) goto done;
    if (xfs_scan_catalogue(device, true, &source, error) != 0 || xfs_validate_writer_support(&source, error) != 0) goto done;
    uint64_t filesystem_bytes = source.geometry.dblocks * source.geometry.block_size;
    if (filesystem_bytes > physical_bytes) { xfs_set_error(error, "XFS active capacity exceeds the target size"); goto done; }
    real = canonical_path(device, error); if (real == NULL) goto done;
    state.device = ld_xstrdup(real); state.target_identity = ld_xstrdup(identity);
    uuid_hex(source.geometry.uuid, state.uuid); snprintf(state.operation, sizeof(state.operation), "%s", operation);
    snprintf(state.phase, sizeof(state.phase), "preflight");
    state.stage = ld_path_append_suffix(journal_path, ".xfs-stage.img");
    state.plan = ld_path_append_suffix(journal_path, ".xfs-plan.sqlite");
    state.physical_bytes = physical_bytes; state.filesystem_bytes = filesystem_bytes;
    if (capacity_preflight(journal_path, &source, error) != 0 || journal_save(journal_path, &state, error) != 0) goto done;
    printf("Raw userspace native-C XFS engine %s\n", LD_VERSION); fflush(stdout);
    if (xfs_verify_clean_log(device, &source, error) != 0) goto precommit_fail;
    if (ld_stop_requested()) goto stopped;
    if (journal_phase(journal_path, &state, "cloning", error) != 0) goto precommit_fail;
    int clone_result = create_stage(device, state.stage, physical_bytes, &source, error);
    if (clone_result == -2 || ld_stop_requested()) goto stopped;
    if (clone_result != 0) goto precommit_fail;
    if (xfs_scan_catalogue(state.stage, true, &staged, error) != 0 || xfs_validate_writer_support(&staged, error) != 0) goto precommit_fail;
    if (journal_phase(journal_path, &state, "planning", error) != 0) goto precommit_fail;
    if (xfs_build_plan(&staged, operation, &plan, error) != 0) goto precommit_fail;
    if (plan.boundary_slack != 0) {
        printf("XFS fixed metadata leaves %" PRIu64
               " unavoidable low-address free block(s), each too small for any complete file%s span; preserving them.\n",
               plan.boundary_slack, strcmp(operation, "growth-defrag") == 0 ? "+reserve" : "");
        fflush(stdout);
    }
    if (xfs_plan_already_applied(&staged, &plan)) {
        transaction_cleanup(journal_path, &state);
        puts("Not needed; canonical XFS layout already verified."); ld_emit_result_event(stdout, operation, "not-needed", "");
        result = 0; goto done;
    }
    if (xfs_open_plan_db(state.plan, true, &db, error) != 0) goto precommit_fail;
    uint64_t move_count = 0;
    if (xfs_populate_plan_db(state.stage, &staged, &plan, db, &move_count, error) != 0) {
        if (ld_stop_requested()) goto stopped;
        goto precommit_fail;
    }
    state.movable_blocks = xfs_catalogue_movable_blocks(&staged); state.move_blocks = move_count;
    if (journal_phase(journal_path, &state, "arranging", error) != 0) goto precommit_fail;
    printf("Arranging %" PRIu64 " supported XFS regular-file blocks; %" PRIu64 " require relocation.\n",
           state.movable_blocks, move_count); fflush(stdout);
    /* Prove that the target allocation trees fit before doing the potentially
       lengthy payload permutation.  The stage is disposable until commit. */
    if (journal_phase(journal_path, &state, "rebuilding-metadata", error) != 0 ||
        xfs_rebuild_allocation_metadata(state.stage, &staged, db, error) != 0)
        goto precommit_fail;
    if (xfs_permute_payloads(state.stage, db, staged.geometry.block_size,
                             move_count, live_updates, error) != 0 ||
        xfs_apply_inode_mappings(state.stage, &staged, db, error) != 0) {
        if (ld_stop_requested()) goto stopped;
        goto precommit_fail;
    }
    if (journal_phase(journal_path, &state, "verifying-stage", error) != 0 ||
        xfs_verify_stage(state.stage, db, &staged, strcmp(operation, "growth-defrag") == 0, &verified, error) != 0) goto precommit_fail;
    if (ld_stop_requested()) goto stopped;
    if (check_unchanged_target(device, &state, source.geometry.dblocks, error) != 0) goto precommit_fail;
    state.commit_offset = 0;
    if (journal_phase(journal_path, &state, "commit", error) != 0) goto precommit_fail;
    /* Stage relocation strokes are a preview only.  Before any persistent
     * write begins, restore the map to the source filesystem that is
     * actually on disk.  Partial source commits are intentionally not
     * presented as authoritative because XFS is not guaranteed to be
     * self-consistent until the verified commit finishes. */
    if (live_updates) emit_live_reset(&source, "source-authoritative");
    puts("The internally verified raw XFS working image is complete. Starting the persistent source commit."); fflush(stdout);
    if (commit_stage(device, journal_path, &state, &verified, 0, error) != 0) goto commit_fail;
    if (journal_phase(journal_path, &state, "verifying-source", error) != 0) goto commit_fail;
    XfsCatalogue committed = {0};
    if (xfs_scan_catalogue(device, true, &committed, error) != 0 ||
        xfs_verify_clean_log(device, &committed, error) != 0 ||
        xfs_verify_allocation_metadata(device, &committed, db, error) != 0 ||
        !same_uuid(&committed, state.uuid) || committed.geometry.dblocks != source.geometry.dblocks) {
        if (*error == NULL) xfs_set_error(error, "committed XFS identity or active capacity changed");
        xfs_catalogue_free(&committed); goto commit_fail;
    }
    if (live_updates) emit_live_reset(&committed, "verified-authoritative");
    xfs_catalogue_free(&committed);
    transaction_cleanup(journal_path, &state);
    printf("XFS %s completed with UUID and full device capacity preserved.\n",
           strcmp(operation, "growth-defrag") == 0 ? "Growth Defrag" : "Defragment");
    ld_emit_result_event(stdout, operation, ld_stop_requested() ? "stopped" : "completed", "");
    result = ld_stop_requested() ? 130 : 0;
    goto done;
stopped:
    transaction_cleanup(journal_path, &state);
    puts("Stop requested before source commit; the original XFS filesystem is unchanged.");
    ld_emit_result_event(stdout, operation, "stopped", ""); result = 130; goto done;
precommit_fail:
    transaction_cleanup(journal_path, &state);
    goto done;
commit_fail:
    /* Journal, stage and plan are deliberately retained for Recover. */
    goto done;
done:
    if (db != NULL) sqlite3_close(db);
    xfs_plan_free(&plan); xfs_catalogue_free(&verified); xfs_catalogue_free(&staged); xfs_catalogue_free(&source);
    journal_free(&state); free(identity); free(real);
    return result;
}

static int recover_transaction(const char *device, const char *journal_path, char **error) {
    XfsJournal state;
    if (journal_load(journal_path, &state, error) != 0) return 1;
    if (!ld_path_is_derived_from(state.stage, journal_path, ".xfs-stage.img") ||
        !ld_path_is_derived_from(state.plan, journal_path, ".xfs-plan.sqlite")) {
        xfs_set_error(error,
            "XFS recovery artifacts are not derived from the selected journal path");
        journal_free(&state);
        return 1;
    }
    int result = 1;
    char *real = canonical_path(device, error), *identity = NULL;
    uint64_t size = 0;
    if (real == NULL || target_identity(device, &identity, &size, error) != 0) goto done;
    if (strcmp(real, state.device) != 0 || strcmp(identity, state.target_identity) != 0 || size != state.physical_bytes) {
        xfs_set_error(error, "recovery journal belongs to a different XFS target"); goto done;
    }
    if (strcmp(state.phase, "commit") != 0 && strcmp(state.phase, "verifying-source") != 0) {
        transaction_cleanup(journal_path, &state);
        puts("Discarded an incomplete XFS working image; the source was unchanged."); result = 0; goto done;
    }
    struct stat stage_status;
    if (stat(state.stage, &stage_status) != 0 || !S_ISREG(stage_status.st_mode) || (uint64_t)stage_status.st_size < state.filesystem_bytes) {
        xfs_set_error(error, "the persistent XFS working image is missing or truncated"); goto done;
    }
    XfsCatalogue staged = {0};
    sqlite3 *db = NULL;
    if (xfs_scan_catalogue(state.stage, true, &staged, error) != 0 || !same_uuid(&staged, state.uuid) ||
        xfs_verify_clean_log(state.stage, &staged, error) != 0 || xfs_open_plan_db(state.plan, false, &db, error) != 0 ||
        xfs_verify_allocation_metadata(state.stage, &staged, db, error) != 0) {
        if (*error == NULL) xfs_set_error(error, "persistent XFS working image has the wrong identity");
        if (db != NULL) sqlite3_close(db);
        xfs_catalogue_free(&staged);
        goto done;
    }
    snprintf(state.phase, sizeof(state.phase), "commit");
    if (journal_save(journal_path, &state, error) != 0) { sqlite3_close(db); xfs_catalogue_free(&staged); goto done; }
    printf("Resuming the XFS source commit at byte %" PRIu64 ".\n", state.commit_offset); fflush(stdout);
    if (commit_stage(device, journal_path, &state, &staged, state.commit_offset, error) != 0) { sqlite3_close(db); xfs_catalogue_free(&staged); goto done; }
    if (journal_phase(journal_path, &state, "verifying-source", error) != 0) { sqlite3_close(db); xfs_catalogue_free(&staged); goto done; }
    XfsCatalogue committed = {0};
    if (xfs_scan_catalogue(device, true, &committed, error) != 0 || !same_uuid(&committed, state.uuid) ||
        xfs_verify_clean_log(device, &committed, error) != 0 || xfs_verify_allocation_metadata(device, &committed, db, error) != 0) {
        if (*error == NULL) xfs_set_error(error, "recovered XFS target has the wrong identity");
        xfs_catalogue_free(&committed); sqlite3_close(db); xfs_catalogue_free(&staged); goto done;
    }
    xfs_catalogue_free(&committed); sqlite3_close(db); xfs_catalogue_free(&staged);
    transaction_cleanup(journal_path, &state);
    puts("XFS recovery completed successfully."); ld_emit_result_event(stdout, "recover", "completed", ""); result = 0;
done:
    free(real); free(identity); journal_free(&state); return result;
}

int main(int argc, char **argv) {
    (void)setvbuf(stdout, NULL, _IOLBF, 0);
    (void)setvbuf(stderr, NULL, _IOLBF, 0);
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        usage(stdout);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts(LD_VERSION);
        return 0;
    }
    if (argc < 3) { usage(stderr); return 2; }
    const char *operation = argv[1], *device = argv[2];
    if (strcmp(operation, "identify") == 0) {
        if (!xfs_probe_path(device)) return 1;
        puts("{\"filesystem\":\"xfs\"}");
        return 0;
    }
    if (strcmp(operation, "analyse-json") == 0) {
        char *error = NULL;
        int result = xfs_emit_analysis_json(device, &error);
        if (result != 0) fprintf(stderr, "%s: %s\n", PROGRAM_NAME, error == NULL ? "XFS analysis failed" : error);
        xfs_clear_error(&error);
        return result == 0 ? 0 : 1;
    }
    if (strcmp(operation, "defrag") != 0 && strcmp(operation, "growth-defrag") != 0 && strcmp(operation, "recover") != 0) {
        usage(stderr); return 2;
    }
    const char *confirm = NULL, *journal = NULL;
    bool write = false, live_updates = false;
    int growth_percent = 10;
    for (int index = 3; index < argc; ++index) {
        if (strcmp(argv[index], "--write") == 0) write = true;
        else if (strcmp(argv[index], "--confirm") == 0 && index + 1 < argc) confirm = argv[++index];
        else if (strcmp(argv[index], "--journal") == 0 && index + 1 < argc) journal = argv[++index];
        else if (strcmp(argv[index], "--growth-percent") == 0 && index + 1 < argc) {
            uint64_t parsed = 0U;
            if (parse_u64(argv[++index], &parsed) != 0 || parsed > 100U) {
                fprintf(stderr, "%s: --growth-percent requires an integer from 0 to 100\n", PROGRAM_NAME);
                return 2;
            }
            growth_percent = (int)parsed;
        }
        else if (strcmp(argv[index], "--live-map-cells") == 0 && index + 1 < argc) {
            uint64_t parsed = 0U;
            if (parse_u64(argv[++index], &parsed) != 0) {
                fprintf(stderr, "%s: --live-map-cells requires a non-negative integer\n", PROGRAM_NAME);
                return 2;
            }
            live_updates = parsed > 0U;
        }
        else if ((strcmp(argv[index], "--ram-buffer") == 0 || strcmp(argv[index], "--workers") == 0 || strcmp(argv[index], "--batch-clusters") == 0) && index + 1 < argc) index++;
        else { fprintf(stderr, "%s: unknown/incomplete option: %s\n", PROGRAM_NAME, argv[index]); return 2; }
    }
    if (!write || confirm == NULL || strcmp(confirm, device) != 0 || journal == NULL) {
        fprintf(stderr, "%s: --write, exact --confirm DEVICE and --journal PATH are required\n", PROGRAM_NAME); return 2;
    }
    if (strcmp(operation, "growth-defrag") == 0 && growth_percent != 10) {
        fprintf(stderr, "%s: Growth Defrag requires exactly 10%%\n", PROGRAM_NAME); return 2;
    }
    ld_stop_clear(); ld_stop_install_handlers();
    char *error = NULL;
    int result;
    if (strcmp(operation, "recover") == 0) result = recover_transaction(device, journal, &error);
    else {
        if (access(journal, F_OK) == 0) {
            xfs_set_error(&error, "an unfinished XFS journal exists; run Recover first"); result = 1;
        } else result = build_and_commit(device, operation, journal, live_updates, &error);
    }
    if (result != 0 && result != 130 && error != NULL) fprintf(stderr, "%s: %s\n", PROGRAM_NAME, error);
    xfs_clear_error(&error);
    return result;
}
