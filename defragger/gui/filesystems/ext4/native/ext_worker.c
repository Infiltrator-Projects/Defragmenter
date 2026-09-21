// SPDX-License-Identifier: GPL-3.0-or-later
#include "ext_native.h"
#include "ext_workspace.h"

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

#include <com_err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define PROGRAM_NAME "linux-defragger-ext-worker"
#define COPY_CHUNK (4U * 1024U * 1024U)
#define JOURNAL_INTERVAL (64U * 1024U * 1024U)
#define JOURNAL_MAGIC "LINUX-DEFRAGGER-EXT-JOURNAL-1"

typedef struct {
    char *device;
    char *target_identity;
    char uuid[33];
    char source_type[8];
    char operation[24];
    char phase[32];
    char *stage;
    char *plan;
    uint64_t physical_bytes;
    uint64_t filesystem_bytes;
    uint64_t commit_offset;
    uint64_t movable_blocks;
    uint64_t move_blocks;
} ExtJournal;

static void usage(FILE *stream) {
    fprintf(stream,
        "Usage:\n"
        "  %s --version\n"
        "  %s identify DEVICE\n"
        "  %s analyse-json DEVICE\n"
        "  %s defrag|growth-defrag|recover DEVICE --write --confirm DEVICE --journal PATH [options]\n",
        PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME);
}





static int ensure_directory_tree(const char *path, char **error) {
    if (ld_path_ensure_trusted_directory_tree(path) != 0) {
        ext_set_error(error, "cannot create EXT journal directory %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}


static void unlink_if_exists(const char *path) {
    if (path == NULL || *path == '\0') return;
    const int failure = infiltratr_unlink_durable(path, true);
    if (failure != 0)
        fprintf(stderr, "%s: warning: cannot durably remove %s: %s\n",
                PROGRAM_NAME, path, strerror(failure));
}


static void journal_free(ExtJournal *state) {
    if (state == NULL) return;
    free(state->device); free(state->target_identity); free(state->stage); free(state->plan);
    memset(state, 0, sizeof(*state));
}

static bool safe_journal_value(const char *value) {
    return value != NULL && strchr(value, '\n') == NULL && strchr(value, '\r') == NULL && strchr(value, '=') == NULL;
}

static bool journal_write_stream(FILE *file, const void *user_data) {
    const ExtJournal *state = user_data;
    fprintf(file, "%s\n", JOURNAL_MAGIC);
    fprintf(file, "device=%s\n", state->device);
    fprintf(file, "target_identity=%s\n", state->target_identity);
    fprintf(file, "uuid=%s\n", state->uuid);
    fprintf(file, "source_type=%s\n", state->source_type);
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

static int journal_save(const char *path, const ExtJournal *state, char **error) {
    if (!safe_journal_value(state->device) || !safe_journal_value(state->target_identity) ||
        !safe_journal_value(state->stage) || !safe_journal_value(state->plan)) {
        ext_set_error(error, "EXT transaction paths contain unsupported journal characters");
        return -1;
    }
    char *parent = ld_path_parent_directory(path);
    if (ensure_directory_tree(parent, error) != 0) { free(parent); return -1; }
    free(parent);
    const int failure = infiltratr_atomic_file_write(
        path, INFILTRATR_ATOMIC_FILE_PRIVATE, journal_write_stream, state);
    if (failure != 0) {
        ext_set_error(error, "cannot publish EXT journal: %s", strerror(failure));
        return -1;
    }
    return 0;
}

static char *value_copy(const char *value) {
    size_t length = strlen(value);
    while (length != 0 && (value[length - 1] == '\n' || value[length - 1] == '\r')) length--;
    return ld_xstrndup(value, length);
}

static int parse_u64(const char *text, uint64_t *value) {
    return infiltratr_parse_u64(text, 10U, value) ? 0 : -1;
}

static int journal_load(const char *path, ExtJournal *state, char **error) {
    memset(state, 0, sizeof(*state));
    FILE *file = fopen(path, "r");
    if (file == NULL) { ext_set_error(error, "cannot open EXT recovery journal: %s", strerror(errno)); return -1; }
    char *line = NULL; size_t capacity = 0;
    if (getline(&line, &capacity, file) < 0) goto invalid;
    infiltratr_trim_line_end(line);
    if (strcmp(line, JOURNAL_MAGIC) != 0) goto invalid;
    while (getline(&line, &capacity, file) >= 0) {
        char *key = NULL;
        char *equals = NULL;
        if (infiltratr_config_parse_line(line, &key, &equals) !=
            INFILTRATR_CONFIG_LINE_ENTRY)
            goto invalid;
        if (strcmp(key, "device") == 0) { free(state->device); state->device = value_copy(equals); }
        else if (strcmp(key, "target_identity") == 0) { free(state->target_identity); state->target_identity = value_copy(equals); }
        else if (strcmp(key, "uuid") == 0) infiltratr_copy_string(state->uuid, sizeof(state->uuid), equals);
        else if (strcmp(key, "source_type") == 0) infiltratr_copy_string(state->source_type, sizeof(state->source_type), equals);
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
    if (state->device == NULL || state->target_identity == NULL || state->stage == NULL || state->plan == NULL ||
        state->uuid[0] == '\0' || state->source_type[0] == '\0' || state->operation[0] == '\0' || state->phase[0] == '\0' ||
        state->physical_bytes == 0 || state->filesystem_bytes == 0) goto invalid_state;
    return 0;
invalid:
    free(line); fclose(file);
invalid_state:
    journal_free(state); ext_set_error(error, "EXT recovery journal is malformed or incomplete"); return -1;
}

static int journal_phase(const char *path, ExtJournal *state, const char *phase, char **error) {
    infiltratr_copy_string(state->phase, sizeof(state->phase), phase);
    return journal_save(path, state, error);
}

static void transaction_cleanup(const char *journal, const ExtJournal *state) {
    if (state != NULL) {
        unlink_if_exists(state->stage); unlink_if_exists(state->plan);
        if (state->plan != NULL) {
            char *wal = ld_path_append_suffix(state->plan, "-wal"); char *shm = ld_path_append_suffix(state->plan, "-shm");
            unlink_if_exists(wal); unlink_if_exists(shm); free(wal); free(shm);
        }
    }
    unlink_if_exists(journal);
}

static void uuid_hex(const uint8_t uuid[16], char output[33]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t index = 0; index < 16U; ++index) {
        output[index * 2U] = digits[uuid[index] >> 4];
        output[index * 2U + 1U] = digits[uuid[index] & 15U];
    }
    output[32] = '\0';
}

static int capacity_preflight(const char *journal_path, const ExtGeometry *geometry, char **error) {
    char *parent = ld_path_parent_directory(journal_path);
    struct statvfs info;
    if (statvfs(parent, &info) != 0) { ext_set_error(error, "cannot inspect EXT staging filesystem capacity: %s", strerror(errno)); free(parent); return -1; }
    free(parent);
    uint64_t available = (uint64_t)info.f_bavail * (uint64_t)info.f_frsize;
    uint64_t allocated_blocks = geometry->total_blocks - geometry->free_blocks;
    uint64_t stage_bytes = infiltratr_u64_multiply_saturating(
        allocated_blocks, geometry->block_size);
    uint64_t plan_bytes = infiltratr_u64_multiply_saturating(allocated_blocks, 96U);
    plan_bytes = infiltratr_u64_add_saturating(
        plan_bytes, UINT64_C(64) * 1024U * 1024U);
    uint64_t required = infiltratr_u64_add_saturating(stage_bytes, plan_bytes);
    required = infiltratr_u64_add_saturating(required, required / 20U);
    if (available < required) {
        ext_set_error(error,
            "EXT safe staging has only %llu MB available; approximately %llu MB is required",
            (unsigned long long)(available / (1024U * 1024U)),
            (unsigned long long)(required / (1024U * 1024U)));
        return -1;
    }
    return 0;
}

static int copy_range(int source, int target, uint64_t offset, uint64_t length,
                      uint8_t *buffer, char **error) {
    uint64_t done = 0;
    while (done < length) {
        size_t amount = length - done > COPY_CHUNK ? COPY_CHUNK : (size_t)(length - done);
        ssize_t got = ld_pread_full(source, buffer, amount, offset + done);
        if (got < 0 || (size_t)got != amount) { ext_set_error(error, "short read cloning EXT working image"); return -1; }
        ssize_t wrote = ld_pwrite_full(target, buffer, amount, offset + done);
        if (wrote < 0 || (size_t)wrote != amount) { ext_set_error(error, "short write cloning EXT working image"); return -1; }
        done += amount;
    }
    return 0;
}

static int create_stage(const char *source_path, const char *stage_path,
                        const ExtGeometry *geometry, char **error) {
    unlink_if_exists(stage_path);
    LdDevice source = ld_device_open(source_path, false);
    int stage = open(stage_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (stage < 0) { ext_set_error(error, "cannot create EXT working image: %s", strerror(errno)); ld_device_close(&source); return -1; }
    if (ftruncate(stage, (off_t)geometry->physical_bytes) != 0) {
        ext_set_error(error, "cannot size EXT working image: %s", strerror(errno)); close(stage); ld_device_close(&source); unlink_if_exists(stage_path); return -1;
    }
    ext2_filsys fs = NULL;
    if (ext_open_fs(source_path, false, &fs, error) != 0) { close(stage); ld_device_close(&source); unlink_if_exists(stage_path); return -1; }
    uint8_t *buffer = ld_xmalloc(COPY_CHUNK);
    uint64_t run_start = 0, run_length = 0, copied = 0;
    int result = 0;
    for (uint64_t block = 0; block < geometry->total_blocks; ++block) {
        bool allocated = block < geometry->first_data_block ||
            ext2fs_test_block_bitmap2(fs->block_map, (blk64_t)block) != 0;
        if (allocated) {
            if (run_length == 0) run_start = block;
            run_length++;
        }
        bool flush = run_length != 0 && (!allocated || block + 1U == geometry->total_blocks || run_length * geometry->block_size >= COPY_CHUNK);
        if (flush) {
            uint64_t bytes = run_length * geometry->block_size;
            if (copy_range(source.fd, stage, run_start * geometry->block_size, bytes, buffer, error) != 0) { result = -1; break; }
            copied += run_length; run_length = 0;
            if ((copied % 65536U) < 1024U) {
                printf("EXT working-image clone: %llu allocated blocks copied.\n", (unsigned long long)copied); fflush(stdout);
            }
            if (ld_stop_requested()) { ext_set_error(error, "stop requested before EXT source commit"); result = -2; break; }
        }
    }
    free(buffer); (void)ext2fs_close(fs);
    if (result == 0 && fsync(stage) != 0) { ext_set_error(error, "cannot sync EXT working image: %s", strerror(errno)); result = -1; }
    close(stage); ld_device_close(&source);
    if (result != 0) unlink_if_exists(stage_path);
    return result;
}

static bool same_uuid(const ExtGeometry *geometry, const char *uuid) {
    char found[33]; uuid_hex(geometry->uuid, found); return strcmp(found, uuid) == 0;
}

static int check_unchanged_target(const char *device, const ExtJournal *state,
                                  const ExtGeometry *expected, char **error) {
    char *canonical = NULL, *identity = NULL; uint64_t size = 0;
    if (ld_device_capture_binding(device, &canonical, &identity, &size) != 0) {
        ext_set_error(error, "cannot rebind EXT target: %s", strerror(errno));
        return -1;
    }
    bool identity_ok = strcmp(canonical, state->device) == 0 &&
        strcmp(identity, state->target_identity) == 0 && size == state->physical_bytes;
    free(canonical); free(identity);
    if (!identity_ok) { ext_set_error(error, "the EXT target path, identity or size changed while its working image was prepared"); return -1; }
    ExtGeometry current;
    if (ext_read_geometry(device, &current, error) != 0) return -1;
    if (!same_uuid(&current, state->uuid) || current.total_blocks != expected->total_blocks ||
        strcmp(current.filesystem, state->source_type) != 0) {
        ext_set_error(error, "the EXT target changed while its working image was prepared"); return -1;
    }
    return 0;
}

static int collect_allocated_ranges(const char *stage_path, const ExtGeometry *geometry,
                                    ExtRangeVec *ranges, uint64_t *allocated_blocks,
                                    char **error) {
    ext2_filsys fs = NULL;
    memset(ranges, 0, sizeof(*ranges));
    *allocated_blocks = 0;
    if (ext_open_fs(stage_path, false, &fs, error) != 0) return -1;

    uint64_t run_start = 0, run_length = 0;
    for (uint64_t block = 0; block < geometry->total_blocks; ++block) {
        bool allocated = block < geometry->first_data_block ||
            ext2fs_test_block_bitmap2(fs->block_map, (blk64_t)block) != 0;
        if (allocated) {
            if (run_length == 0) run_start = block;
            run_length++;
            (*allocated_blocks)++;
        }
        if (run_length != 0 && (!allocated || block + 1U == geometry->total_blocks)) {
            ext_range_push(ranges, run_start * geometry->block_size,
                           (run_start + run_length) * geometry->block_size);
            run_length = 0;
        }
    }
    (void)ext2fs_close(fs);
    return 0;
}

static uint64_t committed_allocated_bytes(const ExtRangeVec *ranges, uint64_t cursor) {
    uint64_t committed = 0;
    for (size_t index = 0; index < ranges->count; ++index) {
        ExtRange range = ranges->items[index];
        if (cursor <= range.start) break;
        uint64_t end = cursor < range.end ? cursor : range.end;
        if (end > range.start) committed += end - range.start;
        if (cursor < range.end) break;
    }
    return committed;
}

static int commit_stage(const char *device_path, const char *journal_path,
                        ExtJournal *state, uint64_t start_offset, char **error) {
    char *canonical = NULL, *identity = NULL; uint64_t size = 0;
    if (ld_device_capture_binding(device_path, &canonical, &identity, &size) != 0) {
        ext_set_error(error, "cannot rebind EXT target before commit: %s", strerror(errno));
        return -1;
    }
    if (strcmp(canonical, state->device) != 0 ||
        strcmp(identity, state->target_identity) != 0 || size != state->physical_bytes) {
        free(canonical); free(identity); ext_set_error(error, "target path, identity or size changed before the EXT commit"); return -1;
    }
    free(canonical); free(identity);
    if (start_offset > state->filesystem_bytes) {
        ext_set_error(error, "EXT recovery journal has an invalid commit offset");
        return -1;
    }

    ExtGeometry staged_geometry;
    ExtRangeVec allocated_ranges = {0};
    uint64_t allocated_blocks = 0;
    if (ext_read_geometry(state->stage, &staged_geometry, error) != 0 ||
        staged_geometry.total_blocks * staged_geometry.block_size != state->filesystem_bytes ||
        collect_allocated_ranges(state->stage, &staged_geometry, &allocated_ranges,
                                 &allocated_blocks, error) != 0) {
        ext_range_free(&allocated_ranges);
        return -1;
    }
    uint64_t allocated_bytes = allocated_blocks * staged_geometry.block_size;
    printf("EXT source commit: writing %.1f MB of verified allocated blocks instead of rewriting the full %.1f MB filesystem.\n",
           (double)allocated_bytes / (1024.0 * 1024.0),
           (double)state->filesystem_bytes / (1024.0 * 1024.0));
    fflush(stdout);

    LdDevice target;
    if (ld_device_try_open(device_path, true, &target) != 0) {
        ext_set_error(error,
                      "cannot open the journal-bound EXT target for commit: %s",
                      strerror(errno));
        ext_range_free(&allocated_ranges);
        return -1;
    }
    if (!ld_device_matches_identity(&target, state->target_identity,
                                    state->physical_bytes)) {
        ld_device_close(&target);
        ext_set_error(error,
                      "EXT target identity or capacity changed before commit");
        ext_range_free(&allocated_ranges);
        return -1;
    }
    if (flock(target.fd, LOCK_EX) != 0) {
        ext_set_error(error, "cannot lock EXT target for commit: %s", strerror(errno));
        ext_range_free(&allocated_ranges); ld_device_close(&target); return -1;
    }
    int stage = open(state->stage, O_RDONLY | O_CLOEXEC);
    if (stage < 0) {
        ext_set_error(error, "cannot open verified EXT working image: %s", strerror(errno));
        ext_range_free(&allocated_ranges); ld_device_close(&target); return -1;
    }

    uint8_t *buffer = ld_xmalloc(COPY_CHUNK);
    uint64_t committed = committed_allocated_bytes(&allocated_ranges, start_offset);
    uint64_t since_journal = 0;
    bool stop_announced = false; int result = 0;
    for (size_t index = 0; index < allocated_ranges.count && result == 0; ++index) {
        ExtRange range = allocated_ranges.items[index];
        if (range.end <= start_offset) continue;
        uint64_t offset = range.start < start_offset ? start_offset : range.start;
        while (offset < range.end) {
            size_t amount = range.end - offset > COPY_CHUNK ? COPY_CHUNK : (size_t)(range.end - offset);
            ssize_t got = ld_pread_full(stage, buffer, amount, offset);
            if (got < 0 || (size_t)got != amount) {
                ext_set_error(error, "EXT working image ended during allocated-range source commit");
                result = -1; break;
            }
            ssize_t wrote = ld_pwrite_full(target.fd, buffer, amount, offset);
            if (wrote < 0 || (size_t)wrote != amount) {
                ext_set_error(error, "short write during recoverable EXT allocated-range commit");
                result = -1; break;
            }
            offset += amount; committed += amount; since_journal += amount;
            bool last_write = index + 1U == allocated_ranges.count && offset == range.end;
            if (since_journal >= JOURNAL_INTERVAL || last_write) {
                if (fsync(target.fd) != 0) {
                    ext_set_error(error, "cannot sync EXT source commit: %s", strerror(errno));
                    result = -1; break;
                }
                state->commit_offset = offset;
                if (journal_save(journal_path, state, error) != 0) { result = -1; break; }
                since_journal = 0;
                double percent = allocated_bytes == 0 ? 100.0 : 100.0 * (double)committed / (double)allocated_bytes;
                if (percent > 100.0) percent = 100.0;
                printf("EXT allocated-range commit: %.2f percent completed\n", percent); fflush(stdout);
            }
            if (ld_stop_requested() && !stop_announced) {
                puts("Stop requested after EXT commit began; completing the verified allocated-range commit so the target is never left partially written.");
                fflush(stdout); stop_announced = true;
            }
        }
    }
    if (result == 0) {
        state->commit_offset = state->filesystem_bytes;
        if (fsync(target.fd) != 0 || journal_save(journal_path, state, error) != 0) {
            if (error != NULL && *error == NULL)
                ext_set_error(error, "cannot sync completed EXT source commit: %s", strerror(errno));
            result = -1;
        }
    }
    free(buffer); close(stage); ld_device_close(&target); ext_range_free(&allocated_ranges);
    return result;
}

static void emit_ranges(const ExtRangeVec *ranges) {
    putchar('[');
    for (size_t index = 0; index < ranges->count; ++index) {
        if (index != 0) putchar(',');
        printf("[%" PRIu64 ",%" PRIu64 "]", ranges->items[index].start, ranges->items[index].end);
    }
    putchar(']');
}

static int analyse_json(const char *path, char **error) {
    ExtGeometry geometry; ExtCatalogue catalogue;
    if (ext_scan_catalogue(path, &geometry, &catalogue, error) != 0) return -1;
    char uuid[33]; uuid_hex(geometry.uuid, uuid);
    printf("{\"filesystem\":\"%s\",\"block_size\":%u,\"total_blocks\":%" PRIu64
           ",\"physical_blocks\":%" PRIu64 ",\"free_blocks\":%" PRIu64
           ",\"uuid\":\"%s\",\"regular_files\":%" PRIu64
           ",\"directories\":%" PRIu64 ",\"fragmented_files\":%" PRIu64
           ",\"fragmented_directories\":%" PRIu64 ",\"inodes_scanned\":%" PRIu64
           ",\"malformed_inodes\":%" PRIu64 ",\"growth_10_satisfied\":%s,\"free_ranges\":",
           geometry.filesystem, geometry.block_size, geometry.total_blocks, geometry.physical_blocks,
           geometry.free_blocks, uuid, catalogue.regular_files, catalogue.directories,
           catalogue.fragmented_files, catalogue.fragmented_directories, catalogue.inodes_scanned,
           catalogue.malformed_inodes, catalogue.growth_10_satisfied ? "true" : "false");
    emit_ranges(&catalogue.free_ranges); fputs(",\"fragmented_ranges\":", stdout);
    emit_ranges(&catalogue.fragmented_ranges); fputs(",\"directory_ranges\":", stdout);
    emit_ranges(&catalogue.directory_ranges); puts("}");
    ext_catalogue_free(&catalogue); return 0;
}

static void emit_live_reset(const ExtGeometry *geometry, const ExtCatalogue *catalogue) {
    printf("@@LIVE_RESET {\"unit_size\":%u,\"filesystem_units\":%" PRIu64 ",\"used_ranges\":[",
           geometry->block_size, geometry->total_blocks);
    uint64_t cursor = 0; bool first = true;
    for (size_t index = 0; index < catalogue->free_ranges.count; ++index) {
        ExtRange free_range = catalogue->free_ranges.items[index];
        if (cursor < free_range.start) {
            if (!first) {
                putchar(',');
            }
            first = false;
            printf("[%" PRIu64 ",%" PRIu64 "]", cursor * geometry->block_size,
                   (free_range.start - cursor) * geometry->block_size);
        }
        if (free_range.end > cursor) cursor = free_range.end;
    }
    if (cursor < geometry->total_blocks) {
        if (!first) putchar(',');
        printf("[%" PRIu64 ",%" PRIu64 "]", cursor * geometry->block_size,
               (geometry->total_blocks - cursor) * geometry->block_size);
    }
    puts("]}"); fflush(stdout);
}


static void discard_plan_files(const char *plan) {
    unlink_if_exists(plan);
    if (plan != NULL) {
        char *wal = ld_path_append_suffix(plan, "-wal");
        char *shm = ld_path_append_suffix(plan, "-shm");
        unlink_if_exists(wal);
        unlink_if_exists(shm);
        free(wal);
        free(shm);
    }
}

static int validate_restored_ext(const char *device, char **error) {
    ext2_filsys fs = NULL;
    if (ext_open_fs(device, false, &fs, error) != 0) return -1;
    int result = ext_validate_metadata(fs, true, error);
    (void)ext2fs_close(fs);
    return result;
}

/* Return 0/130 for a completed or stopped direct operation, 2 when the complete
   original allocation set cannot fit in a safe high-address workspace and the
   verified working-image fallback should be used, or -1 for a hard failure. */
static int try_workspace_relayout(const char *device, const char *operation,
                                  const char *journal_path, bool live_updates,
                                  uint64_t requested_batch_blocks,
                                  ExtJournal *state,
                                  const ExtGeometry *source_geometry,
                                  char **error) {
    ext2_filsys fs = NULL;
    sqlite3 *db = NULL;
    int fd = -1;
    ExtWorkspace workspace = {0};
    bool growth = strcmp(operation, "growth-defrag") == 0;
    bool source_touched = false;

    if (ext_open_plan_db(state->plan, true, &db, error) != 0) goto fail_clean;
    if (ext_open_fs(device, false, &fs, error) != 0) goto fail_clean;
    fd = ld_device_open_verified_fd(device, true, state->target_identity,
                                    state->physical_bytes);
    if (fd < 0) {
        ext_set_error(error,
                      "cannot open the journal-bound EXT source for direct relayout: %s",
                      strerror(errno));
        goto fail_clean;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ext_set_error(error, "cannot lock EXT source for direct relayout: %s", strerror(errno));
        goto fail_clean;
    }
    if (ext_catalog_plan(fs, fd, db, source_geometry,
                         &state->movable_blocks, error) != 0 ||
        ext_assign_targets(fs, db, source_geometry, growth, error) != 0 ||
        ext_plan_move_count(db, &state->move_blocks, error) != 0)
        goto fail_clean;

    if (state->move_blocks == 0) {
        ExtCatalogue current = {0};
        ExtGeometry current_geometry;
        if (ext_scan_catalogue(device, &current_geometry, &current, error) != 0)
            goto fail_clean;
        bool okay = !growth || current.growth_10_satisfied;
        ext_catalogue_free(&current);
        if (okay) {
            (void)ext2fs_close(fs); fs = NULL;
            (void)flock(fd, LOCK_UN); close(fd); fd = -1;
            sqlite3_close(db); db = NULL;
            transaction_cleanup(journal_path, state);
            puts("Not needed; canonical EXT layout already verified.");
            ld_emit_result_event(stdout, operation, "not-needed", "");
            return 0;
        }
        goto fallback;
    }

    int prepared = ext_workspace_prepare(fs, db, source_geometry,
                                         requested_batch_blocks,
                                         &workspace, error);
    if (prepared == EXT_WORKSPACE_UNAVAILABLE) goto fallback;
    if (prepared != 0) goto fail_clean;
    (void)ext2fs_close(fs); fs = NULL;

    if (journal_phase(journal_path, state, "direct-staging", error) != 0)
        goto fail_clean;
    printf("EXT relayout preflight: %" PRIu64 " movable allocation blocks; %" PRIu64
           " require relocation; canonical reserve policy %s.\n",
           state->movable_blocks, state->move_blocks, growth ? "10%" : "0%");
    printf("EXT phase 1: staging the complete original allocated set into a %" PRIu64
           "-block high-address safety workspace beginning at block %" PRIu64 ".\n",
           workspace.blocks, workspace.start);
    printf("EXT adaptive transaction budget: up to %" PRIu64
           " blocks (%.1f MiB) per durable copy boundary.\n",
           workspace.batch_blocks,
           (double)(workspace.batch_blocks * (uint64_t)workspace.block_size) /
               (1024.0 * 1024.0));
    fflush(stdout);

    int staged = ext_workspace_stage(fd, db, &workspace, error);
    if (staged == -2 || ld_stop_requested()) {
        (void)flock(fd, LOCK_UN); close(fd); fd = -1;
        sqlite3_close(db); db = NULL;
        transaction_cleanup(journal_path, state);
        puts("Stop requested during EXT workspace staging; original filesystem metadata and payload remain unchanged.");
        ld_emit_result_event(stdout, operation, "stopped", "");
        return 130;
    }
    if (staged != 0) goto fail_clean;
    if (journal_phase(journal_path, state, "direct-staged", error) != 0)
        goto fail_clean;
    if (ld_stop_requested()) {
        (void)flock(fd, LOCK_UN); close(fd); fd = -1;
        sqlite3_close(db); db = NULL;
        transaction_cleanup(journal_path, state);
        puts("Stop requested after EXT workspace staging; original filesystem remains unchanged.");
        ld_emit_result_event(stdout, operation, "stopped", "");
        return 130;
    }
    if (check_unchanged_target(device, state, source_geometry, error) != 0)
        goto fail_clean;

    if (journal_phase(journal_path, state, "direct-placing", error) != 0)
        goto fail_clean;
    source_touched = true;
    puts("EXT phase 2: placing the canonical layout directly from the durable safety workspace; no filesystem-sized working image is used.");
    fflush(stdout);
    int placed = ext_workspace_place(fd, db, &workspace, error);
    if (placed == -2 || ld_stop_requested()) goto stop_restore;
    if (placed != 0) goto fail_restore;

    if (journal_phase(journal_path, state, "direct-metadata", error) != 0)
        goto fail_restore;
    puts("EXT phase 3: committing canonical inode mappings, allocation bitmaps and filesystem metadata.");
    fflush(stdout);
    /* Once direct metadata mutation begins it is intentionally uninterruptible;
       Stop is honoured at the next complete metadata boundary. */
    if (ext_apply_mappings_under_lock(device, db, false, error) != 0)
        goto fail_restore;

    if (journal_phase(journal_path, state, "direct-verifying", error) != 0)
        goto fail_restore;
    ExtCatalogue committed = {0};
    if (ext_verify_stage(device, db, source_geometry, growth,
                         &committed, error) != 0) {
        ext_catalogue_free(&committed);
        goto fail_restore;
    }
    printf("Layout verification:      %" PRIu64 " files and %" PRIu64
           " directories contiguous; canonical %s policy verified.\n",
           committed.regular_files, committed.directories, growth ? "10%" : "0%");
    if (live_updates) emit_live_reset(source_geometry, &committed);
    ext_catalogue_free(&committed);

    printf("EXT unified workspace layout: %" PRIu64
           " original allocated blocks protected, %" PRIu64
           " relocated directly, without a filesystem-sized working image.\n",
           workspace.blocks, state->move_blocks);
    printf("%s %s completed with UUID and active filesystem capacity preserved.\n",
           source_geometry->filesystem,
           growth ? "Growth Defrag" : "Defragment");
    fflush(stdout);
    (void)flock(fd, LOCK_UN); close(fd); fd = -1;
    sqlite3_close(db); db = NULL;
    transaction_cleanup(journal_path, state);
    if (ld_stop_requested()) {
        ld_emit_result_event(stdout, operation, "stopped", "");
        return 130;
    }
    ld_emit_result_event(stdout, operation, "completed", "");
    return 0;

stop_restore:
    {
        char *restore_error = NULL;
        if (ext_workspace_restore(fd, db, &workspace, &restore_error) != 0) {
            free(*error); *error = restore_error;
            goto keep_recovery;
        }
        (void)flock(fd, LOCK_UN); close(fd); fd = -1;
        if (validate_restored_ext(device, &restore_error) != 0) {
            free(*error); *error = restore_error;
            goto keep_recovery;
        }
        free(restore_error);
    }
    source_touched = false;
    sqlite3_close(db); db = NULL;
    transaction_cleanup(journal_path, state);
    puts("Growth/Defrag Stop restored the original EXT allocation from the durable workspace at a complete transaction boundary.");
    ld_emit_result_event(stdout, operation, "stopped", "");
    return 130;

fail_restore:
    if (source_touched) {
        char *restore_error = NULL;
        if (ext_workspace_restore(fd, db, &workspace, &restore_error) != 0) {
            free(*error); *error = restore_error;
            goto keep_recovery;
        }
        (void)flock(fd, LOCK_UN); close(fd); fd = -1;
        if (validate_restored_ext(device, &restore_error) != 0) {
            free(*error); *error = restore_error;
            goto keep_recovery;
        }
        free(restore_error);
        source_touched = false;
    }
    goto fail_clean;

fallback:
    if (fs != NULL) { (void)ext2fs_close(fs); fs = NULL; }
    if (fd >= 0) { (void)flock(fd, LOCK_UN); close(fd); fd = -1; }
    if (db != NULL) { sqlite3_close(db); db = NULL; }
    discard_plan_files(state->plan);
    puts("EXT complete live allocation set does not fit a safe high-address workspace; using the verified working-image fallback.");
    fflush(stdout);
    return 2;

fail_clean:
    if (fs != NULL) (void)ext2fs_close(fs);
    if (fd >= 0) { (void)flock(fd, LOCK_UN); close(fd); }
    if (db != NULL) sqlite3_close(db);
    transaction_cleanup(journal_path, state);
    return -1;

keep_recovery:
    if (fs != NULL) (void)ext2fs_close(fs);
    if (fd >= 0) { (void)flock(fd, LOCK_UN); close(fd); }
    if (db != NULL) sqlite3_close(db);
    puts("EXT direct relayout needs Recover; the durable workspace and plan have been retained.");
    fflush(stdout);
    return -1;
}

static int build_and_commit(const char *device, const char *operation,
                            const char *journal_path, bool live_updates,
                            uint64_t batch_blocks, char **error) {
    if (ld_path_is_mounted(device)) { ext_set_error(error, "refusing EXT mutation while the block device is mounted"); return 1; }
    char *identity = NULL, *real = NULL; uint64_t physical_bytes = 0;
    ExtGeometry source_geometry, staged_geometry; ExtCatalogue verified = {0};
    ext2_filsys source_fs = NULL, stage_fs = NULL; sqlite3 *db = NULL;
    ExtJournal state = {0}; int result = 1;
    if (ld_device_capture_binding(device, &real, &identity, &physical_bytes) != 0) {
        ext_set_error(error, "cannot bind EXT target %s: %s", device, strerror(errno));
        goto done;
    }
    if (ext_read_geometry(device, &source_geometry, error) != 0) goto done;
    if (ext_open_fs(device, false, &source_fs, error) != 0) goto done;
    if (ext_validate_writer_support(source_fs, &source_geometry, error) != 0) goto done;
    (void)ext2fs_close(source_fs); source_fs = NULL;
    state.device = ld_xstrdup(real); state.target_identity = ld_xstrdup(identity);
    uuid_hex(source_geometry.uuid, state.uuid); infiltratr_copy_string(state.source_type, sizeof(state.source_type), source_geometry.filesystem);
    snprintf(state.operation, sizeof(state.operation), "%s", operation); snprintf(state.phase, sizeof(state.phase), "preflight");
    state.stage = ld_path_append_suffix(journal_path, ".ext-stage.img"); state.plan = ld_path_append_suffix(journal_path, ".ext-plan.sqlite");
    state.physical_bytes = physical_bytes; state.filesystem_bytes = source_geometry.total_blocks * source_geometry.block_size;
    if (journal_save(journal_path, &state, error) != 0) goto done;
    printf("Raw userspace native-C %s relayout engine %s\n", source_geometry.filesystem, LD_VERSION); fflush(stdout);
    if (ld_stop_requested()) goto stopped;
    int direct = try_workspace_relayout(device, operation, journal_path, live_updates,
                                        batch_blocks, &state, &source_geometry, error);
    if (direct != 2) { result = direct < 0 ? 1 : direct; goto done; }
    if (capacity_preflight(journal_path, &source_geometry, error) != 0) goto precommit_fail;
    if (journal_phase(journal_path, &state, "cloning", error) != 0) goto precommit_fail;
    int clone = create_stage(device, state.stage, &source_geometry, error);
    if (clone == -2 || ld_stop_requested()) goto stopped;
    if (clone != 0) goto precommit_fail;
    if (ext_read_geometry(state.stage, &staged_geometry, error) != 0 ||
        !same_uuid(&staged_geometry, state.uuid) || strcmp(staged_geometry.filesystem, state.source_type) != 0) {
        if (*error == NULL) ext_set_error(error, "EXT working image does not preserve source identity");
        goto precommit_fail;
    }
    if (ext_open_fs(state.stage, false, &stage_fs, error) != 0 || ext_validate_writer_support(stage_fs, &staged_geometry, error) != 0) goto precommit_fail;
    if (journal_phase(journal_path, &state, "planning", error) != 0) goto precommit_fail;
    if (ext_open_plan_db(state.plan, true, &db, error) != 0) goto precommit_fail;
    int stage_fd = open(state.stage, O_RDONLY | O_CLOEXEC);
    if (stage_fd < 0) { ext_set_error(error, "cannot open EXT stage for planning: %s", strerror(errno)); goto precommit_fail; }
    if (ext_catalog_plan(stage_fs, stage_fd, db, &staged_geometry, &state.movable_blocks, error) != 0) { close(stage_fd); goto precommit_fail; }
    close(stage_fd);
    if (ext_assign_targets(stage_fs, db, &staged_geometry, strcmp(operation, "growth-defrag") == 0, error) != 0 ||
        ext_plan_move_count(db, &state.move_blocks, error) != 0) goto precommit_fail;
    (void)ext2fs_close(stage_fs); stage_fs = NULL;
    if (state.move_blocks == 0) {
        ExtCatalogue already = {0};
        ExtGeometry current;
        if (ext_scan_catalogue(device, &current, &already, error) != 0) goto precommit_fail;
        bool okay = strcmp(operation, "growth-defrag") != 0 || already.growth_10_satisfied;
        ext_catalogue_free(&already);
        if (okay) {
            transaction_cleanup(journal_path, &state);
            puts("Not needed; canonical EXT layout already verified."); ld_emit_result_event(stdout, operation, "not-needed", "");
            result = 0; goto done;
        }
    }
    if (journal_phase(journal_path, &state, "arranging", error) != 0) goto precommit_fail;
    printf("Arranging %" PRIu64 " EXT allocation blocks; %" PRIu64 " require relocation.\n",
           state.movable_blocks, state.move_blocks); fflush(stdout);
    if (ext_permute_payloads(state.stage, db, staged_geometry.block_size, state.move_blocks, error) != 0 ||
        ext_apply_mappings(state.stage, db, true, error) != 0) goto precommit_fail;
    if (journal_phase(journal_path, &state, "verifying-stage", error) != 0 ||
        ext_verify_stage(state.stage, db, &staged_geometry, strcmp(operation, "growth-defrag") == 0, &verified, error) != 0) goto precommit_fail;
    if (ld_stop_requested()) goto stopped;
    if (check_unchanged_target(device, &state, &source_geometry, error) != 0) goto precommit_fail;
    state.commit_offset = 0;
    if (journal_phase(journal_path, &state, "commit", error) != 0) goto precommit_fail;
    puts("The internally verified EXT working image is complete. Starting the persistent source commit."); fflush(stdout);
    if (commit_stage(device, journal_path, &state, 0, error) != 0) goto commit_fail;
    if (journal_phase(journal_path, &state, "verifying-source", error) != 0) goto commit_fail;
    ExtCatalogue committed = {0};
    if (ext_verify_stage(device, db, &staged_geometry, strcmp(operation, "growth-defrag") == 0, &committed, error) != 0) goto commit_fail;
    if (live_updates) emit_live_reset(&staged_geometry, &committed);
    ext_catalogue_free(&committed);
    transaction_cleanup(journal_path, &state);
    printf("%s %s completed with UUID and active filesystem capacity preserved.\n",
           source_geometry.filesystem,
           strcmp(operation, "growth-defrag") == 0 ? "Growth Defrag" : "Defragment");
    ld_emit_result_event(stdout, operation, ld_stop_requested() ? "stopped" : "completed", "");
    result = ld_stop_requested() ? 130 : 0; goto done;
stopped:
    transaction_cleanup(journal_path, &state);
    puts("Stop requested before source commit; the original EXT filesystem is unchanged.");
    ld_emit_result_event(stdout, operation, "stopped", ""); result = 130; goto done;
precommit_fail:
    transaction_cleanup(journal_path, &state); goto done;
commit_fail:
    /* Keep the verified stage, plan and journal for Recover. */
    goto done;
done:
    if (source_fs != NULL) (void)ext2fs_close(source_fs);
    if (stage_fs != NULL) (void)ext2fs_close(stage_fs);
    if (db != NULL) sqlite3_close(db);
    ext_catalogue_free(&verified); free(identity); free(real); journal_free(&state);
    return result;
}

static int recover(const char *device, const char *journal_path, char **error) {
    if (ld_path_is_mounted(device)) { ext_set_error(error, "refusing EXT recovery while the block device is mounted"); return 1; }
    ExtJournal state;
    if (journal_load(journal_path, &state, error) != 0) return 1;
    if (!ld_path_is_derived_from(state.stage, journal_path, ".ext-stage.img") ||
        !ld_path_is_derived_from(state.plan, journal_path, ".ext-plan.sqlite")) {
        ext_set_error(error,
            "EXT recovery artifacts are not derived from the selected journal path");
        journal_free(&state);
        return 1;
    }
    char *real = NULL, *identity = NULL; uint64_t size = 0;
    if (ld_device_capture_binding(device, &real, &identity, &size) != 0) {
        ext_set_error(error, "cannot bind EXT recovery target: %s", strerror(errno));
        journal_free(&state); return 1;
    }
    if (strcmp(real, state.device) != 0 || strcmp(identity, state.target_identity) != 0 || size != state.physical_bytes) {
        ext_set_error(error, "EXT recovery journal belongs to a different target"); free(real); free(identity); journal_free(&state); return 1;
    }
    free(real); free(identity);
    if (infiltratr_string_starts_with(state.phase, "direct-")) {
        sqlite3 *direct_db = NULL;
        if (ext_open_plan_db(state.plan, false, &direct_db, error) != 0) {
            journal_free(&state);
            return 1;
        }
        if (strcmp(state.phase, "direct-staging") == 0 ||
            strcmp(state.phase, "direct-staged") == 0) {
            sqlite3_close(direct_db);
            transaction_cleanup(journal_path, &state);
            puts("Discarded an incomplete EXT workspace stage; source filesystem was unchanged.");
            journal_free(&state);
            ld_emit_result_event(stdout, "recover", "completed", "");
            return 0;
        }
        ExtWorkspace workspace;
        if (ext_workspace_load(direct_db, &workspace, error) != 0) {
            sqlite3_close(direct_db);
            journal_free(&state);
            return 1;
        }
        if (strcmp(state.phase, "direct-verifying") == 0) {
            ExtGeometry current_geometry;
            ExtCatalogue verified = {0};
            if (ext_read_geometry(device, &current_geometry, error) == 0 &&
                ext_verify_stage(device, direct_db, &current_geometry,
                                 strcmp(state.operation, "growth-defrag") == 0,
                                 &verified, error) == 0) {
                ext_catalogue_free(&verified);
                sqlite3_close(direct_db);
                transaction_cleanup(journal_path, &state);
                puts("EXT direct workspace recovery verified the completed canonical layout.");
                journal_free(&state);
                ld_emit_result_event(stdout, "recover", "completed", "");
                return 0;
            }
            ext_catalogue_free(&verified);
            free(*error); *error = NULL;
        }
        int direct_fd = ld_device_open_verified_fd(
            device, true, state.target_identity, state.physical_bytes);
        if (direct_fd < 0 || flock(direct_fd, LOCK_EX | LOCK_NB) != 0) {
            if (direct_fd >= 0) close(direct_fd);
            sqlite3_close(direct_db);
            ext_set_error(error, "cannot lock EXT source for direct workspace recovery: %s", strerror(errno));
            journal_free(&state);
            return 1;
        }
        if (ext_workspace_restore(direct_fd, direct_db, &workspace, error) != 0) {
            (void)flock(direct_fd, LOCK_UN); close(direct_fd);
            sqlite3_close(direct_db);
            journal_free(&state);
            return 1;
        }
        (void)flock(direct_fd, LOCK_UN); close(direct_fd);
        if (validate_restored_ext(device, error) != 0) {
            sqlite3_close(direct_db);
            journal_free(&state);
            return 1;
        }
        sqlite3_close(direct_db);
        transaction_cleanup(journal_path, &state);
        puts("EXT direct workspace recovery restored the exact original allocated filesystem state.");
        journal_free(&state);
        ld_emit_result_event(stdout, "recover", "completed", "");
        return 0;
    }
    if (strcmp(state.phase, "commit") != 0 && strcmp(state.phase, "verifying-source") != 0) {
        transaction_cleanup(journal_path, &state);
        puts("Discarded an incomplete EXT working image; the source filesystem was unchanged.");
        journal_free(&state); ld_emit_result_event(stdout, "recover", "completed", ""); return 0;
    }
    struct stat stage_status;
    if (stat(state.stage, &stage_status) != 0 || (uint64_t)stage_status.st_size != state.physical_bytes) {
        ext_set_error(error, "persistent EXT working image is missing or has the wrong size"); journal_free(&state); return 1;
    }
    ExtGeometry geometry; if (ext_read_geometry(state.stage, &geometry, error) != 0 || !same_uuid(&geometry, state.uuid)) { journal_free(&state); return 1; }
    sqlite3 *db = NULL; if (ext_open_plan_db(state.plan, false, &db, error) != 0) { journal_free(&state); return 1; }
    ExtCatalogue verified = {0};
    if (ext_verify_stage(state.stage, db, &geometry, strcmp(state.operation, "growth-defrag") == 0, &verified, error) != 0) {
        sqlite3_close(db); journal_free(&state); return 1;
    }
    ext_catalogue_free(&verified);
    snprintf(state.phase, sizeof(state.phase), "commit");
    if (journal_save(journal_path, &state, error) != 0 || commit_stage(device, journal_path, &state, state.commit_offset, error) != 0) {
        sqlite3_close(db); journal_free(&state); return 1;
    }
    snprintf(state.phase, sizeof(state.phase), "verifying-source"); (void)journal_save(journal_path, &state, NULL);
    ExtCatalogue committed = {0};
    if (ext_verify_stage(device, db, &geometry, strcmp(state.operation, "growth-defrag") == 0, &committed, error) != 0) {
        sqlite3_close(db); journal_free(&state); return 1;
    }
    ext_catalogue_free(&committed); sqlite3_close(db);
    transaction_cleanup(journal_path, &state); journal_free(&state);
    puts("EXT recovery completed successfully."); ld_emit_result_event(stdout, "recover", "completed", ""); return 0;
}

int main(int argc, char **argv) {
    ld_runtime_set_program_name(PROGRAM_NAME); ld_stop_install_handlers();
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) { usage(stdout); return 0; }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) { printf("%s %s\n", PROGRAM_NAME, LD_VERSION); return 0; }
    if (argc == 3 && strcmp(argv[1], "identify") == 0) {
        ExtGeometry geometry; char *error = NULL;
        if (ext_read_geometry(argv[2], &geometry, &error) != 0) { free(error); return 1; }
        printf("{\"filesystem\":\"%s\"}\n", geometry.filesystem); return 0;
    }
    if (argc == 3 && strcmp(argv[1], "analyse-json") == 0) {
        char *error = NULL; int result = analyse_json(argv[2], &error);
        if (result != 0) { fprintf(stderr, "%s: %s\n", PROGRAM_NAME, error != NULL ? error : "analysis failed"); free(error); return 1; }
        return 0;
    }
    if (argc < 3 || (strcmp(argv[1], "defrag") != 0 && strcmp(argv[1], "growth-defrag") != 0 && strcmp(argv[1], "recover") != 0)) {
        usage(stderr); return 2;
    }
    const char *operation = argv[1], *device = argv[2], *confirm = NULL, *journal = NULL;
    bool write = false, live_updates = false; int growth_percent = 10;
    uint64_t batch_blocks = 0;
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
        else if (strcmp(argv[index], "--batch-clusters") == 0 && index + 1 < argc) {
            if (parse_u64(argv[++index], &batch_blocks) != 0 || batch_blocks == 0) {
                fprintf(stderr, "%s: --batch-clusters requires a positive integer\n", PROGRAM_NAME); return 2;
            }
        }
        else if ((strcmp(argv[index], "--workers") == 0 || strcmp(argv[index], "--ram-buffer") == 0) && index + 1 < argc) { index++; }
        else { fprintf(stderr, "%s: unknown or incomplete option: %s\n", PROGRAM_NAME, argv[index]); return 2; }
    }
    if (!write || confirm == NULL || strcmp(confirm, device) != 0 || journal == NULL) {
        fprintf(stderr, "%s: --write, exact --confirm DEVICE and --journal PATH are required\n", PROGRAM_NAME); return 2;
    }
    if (strcmp(operation, "growth-defrag") == 0 && growth_percent != 10) {
        fprintf(stderr, "%s: Growth Defrag requires exactly 10%%\n", PROGRAM_NAME); return 2;
    }
    char *error = NULL; int result;
    if (strcmp(operation, "recover") == 0) result = recover(device, journal, &error);
    else {
        if (access(journal, F_OK) == 0) { fprintf(stderr, "%s: an unfinished EXT journal exists; run Recover first\n", PROGRAM_NAME); return 1; }
        result = build_and_commit(device, operation, journal, live_updates, batch_blocks, &error);
    }
    if (result != 0 && result != 130 && error != NULL) fprintf(stderr, "%s: %s\n", PROGRAM_NAME, error);
    free(error); return result;
}
