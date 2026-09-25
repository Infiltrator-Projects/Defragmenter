// SPDX-License-Identifier: GPL-3.0-or-later
#include "sfs_native.h"
#include "version.h"
#include "infiltratr/arithmetic.h"
#include "infiltratr/core.h"
#include "infiltratr/config.h"
#include "infiltratr/posix.h"
#include "ld_device.h"
#include "ld_io.h"
#include "ld_path.h"
#include "ld_runtime.h"
#include "ld_protocol.h"
#include "ld_stop.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <openssl/evp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define PROG "linux-defragger-sfs-worker"
#define JOURNAL_MAGIC "LINUX-DEFRAGGER-SFS-JOURNAL-1"
#define STOPPED 130
#define SFS_WORKER_IO_BYTES (1024U * 1024U)

static void usage(FILE *stream)
{
    (void)fprintf(stream,
        "Usage: %s --version | identify DEVICE | analyse-json DEVICE | map DEVICE --cells COUNT | "
        "defrag|growth-defrag|recover DEVICE --write --confirm DEVICE --journal PATH "
        "[--growth-percent 10] [--live-updates]\n", PROG);
}

static const char *json_bool(bool value) { return value ? "true" : "false"; }

static int parse_cells(const char *text, uint64_t *cells)
{
    uint64_t value = 0U;
    if (!infiltratr_parse_u64_range(text, 10U, 1U, UINT64_MAX, &value))
        return -1;
    *cells = value;
    return 0;
}

static int analyse(const char *path, SfsAnalysis *analysis)
{
    char error[512] = {0};
    if (sfs_analyse(path, analysis, NULL, 0U, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s: %s\n", PROG,
                      error[0] != '\0' ? error : "SFS analysis failed");
        return -1;
    }
    return 0;
}

static const char *format_name(const SfsAnalysis *analysis)
{
    return analysis != NULL && analysis->structure_version == 4U ? "SFS2" : "SFS0";
}

static void print_identify(const SfsAnalysis *analysis)
{
    (void)printf("{\"filesystem\":\"sfs\",\"format\":\"%s\"}\n",
                 format_name(analysis));
}

static void print_analysis_json(const SfsAnalysis *analysis)
{
    const double fragmentation = infiltratr_percent_u64(
        analysis->fragmented_files, analysis->regular_files);
    (void)printf(
        "{\"filesystem\":\"sfs\",\"format\":\"%s\","
        "\"structure_version\":%u,\"sequence_number\":%u,"
        "\"block_size\":%u,\"total_blocks\":%u,\"filesystem_bytes\":%" PRIu64 ","
        "\"physical_bytes\":%" PRIu64 ",\"bitmap_base\":%u,\"bitmap_blocks\":%u,"
        "\"used_blocks\":%" PRIu64 ",\"free_blocks\":%" PRIu64 ","
        "\"data_blocks\":%" PRIu64 ",\"regular_files\":%" PRIu64 ","
        "\"directories\":%" PRIu64 ",\"fragmented_files\":%" PRIu64 ","
        "\"fragmented_directories\":0,\"fragmentation_percent\":%.6f,"
        "\"growth_10_satisfied\":%s,"
        "\"primary_root_valid\":%s,\"backup_root_valid\":%s,"
        "\"transaction_pending\":%s,\"fragmentation_available\":true}\n",
        format_name(analysis),
        analysis->structure_version, analysis->sequence_number,
        analysis->block_size, analysis->total_blocks, analysis->filesystem_bytes,
        analysis->physical_bytes, analysis->bitmap_base, analysis->bitmap_blocks,
        analysis->used_blocks, analysis->free_blocks,
        analysis->data_blocks, analysis->regular_files,
        analysis->directories, analysis->fragmented_files, fragmentation,
        json_bool(analysis->growth_10_satisfied),
        json_bool(analysis->primary_root_valid), json_bool(analysis->backup_root_valid),
        json_bool(analysis->transaction_pending));
}

static int print_map(const char *path, uint64_t requested_cells)
{
    SfsAnalysis summary;
    if (analyse(path, &summary) != 0)
        return -1;

    const uint64_t physical_units =
        (summary.physical_bytes + summary.block_size - 1U) / summary.block_size;
    const uint64_t total_units = physical_units > summary.total_blocks
                               ? physical_units : summary.total_blocks;
    uint64_t cells = requested_cells;
    if (cells > total_units) cells = total_units;
    if (cells == 0U) cells = 1U;
    size_t map_bytes = 0U;
    if (cells > SIZE_MAX ||
        !infiltratr_size_multiply_checked((size_t)cells,
                                          sizeof(SfsMapCell), &map_bytes)) {
        (void)fprintf(stderr, "%s: allocation map is too large\n", PROG);
        return -1;
    }
    (void)map_bytes;
    SfsMapCell *map = calloc((size_t)cells, sizeof(*map));
    if (map == NULL) {
        (void)fprintf(stderr, "%s: out of memory building SFS allocation map\n", PROG);
        return -1;
    }
    SfsAnalysis analysis;
    char error[512] = {0};
    if (sfs_analyse(path, &analysis, map, cells, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s: %s\n", PROG,
                      error[0] != '\0' ? error : "SFS allocation scan failed");
        free(map);
        return -1;
    }

    const double fragmentation = infiltratr_percent_u64(
        analysis.fragmented_files, analysis.regular_files);
    const uint64_t outside_units = total_units > analysis.total_blocks
                                 ? total_units - analysis.total_blocks : 0U;
    (void)printf(
        "{\"schema\":1,\"backend\":\"read-only-domain\",\"filesystem\":\"sfs\","
        "\"map_accuracy\":\"exact-allocation\",\"unit_size\":%u,"
        "\"total_units\":%" PRIu64 ",\"cell_count\":%" PRIu64 ","
        "\"total_bytes\":%" PRIu64 ",\"filesystem_units\":%u,"
        "\"filesystem_bytes\":%" PRIu64 ",\"outside_bytes\":%" PRIu64 ","
        "\"free_bytes\":%" PRIu64 ",\"used_bytes\":%" PRIu64 ","
        "\"unknown_bytes\":0,\"regular_files\":%" PRIu64 ","
        "\"directories\":%" PRIu64 ",\"fragmented_files\":%" PRIu64 ","
        "\"fragmented_directories\":0,\"fragmentation_percent\":%.6f,"
        "\"growth_10_satisfied\":%s,\"cells\":[",
        analysis.block_size, total_units, cells,
        total_units * analysis.block_size, analysis.total_blocks,
        analysis.filesystem_bytes, outside_units * analysis.block_size,
        analysis.free_blocks * analysis.block_size,
        analysis.used_blocks * analysis.block_size,
        analysis.regular_files, analysis.directories, analysis.fragmented_files,
        fragmentation, json_bool(analysis.growth_10_satisfied));
    for (uint64_t i = 0U; i < cells; ++i) {
        if (i != 0U) (void)putchar(',');
        (void)printf(
            "{\"start\":%" PRIu64 ",\"end\":%" PRIu64 ","
            "\"free\":%" PRIu64 ",\"used\":%" PRIu64 ","
            "\"unknown\":0,\"bad\":0,\"fragmented\":%" PRIu64 ",\"directory\":0,"
            "\"outside\":%" PRIu64 "}",
            map[i].start, map[i].end, map[i].free_count, map[i].used_count,
            map[i].fragmented_count, map[i].outside_count);
    }
    (void)printf(
        "],\"details\":{\"format\":\"%s\",\"structure_version\":%u,"
        "\"sequence_number\":%u,\"bitmap_base\":%u,\"bitmap_blocks\":%u,"
        "\"primary_root_valid\":%s,\"backup_root_valid\":%s,"
        "\"transaction_pending\":%s,\"fragmentation_available\":true,"
        "\"fragmentation_basis\":\"validated SFS object containers and extent B-tree chains\","
        "\"allocation_basis\":\"validated SFS BTMP free-space bitmap\"}}\n",
        format_name(&analysis),
        analysis.structure_version, analysis.sequence_number,
        analysis.bitmap_base, analysis.bitmap_blocks,
        json_bool(analysis.primary_root_valid), json_bool(analysis.backup_root_valid),
        json_bool(analysis.transaction_pending));
    free(map);
    return 0;
}



typedef struct {
    char *device;
    char *target_identity;
    char *stage;
    char operation[24];
    char phase[24];
    char volume_token[65];
    char source_sha256[65];
    char stage_sha256[65];
    uint64_t physical_bytes;
    uint64_t filesystem_bytes;
    uint32_t total_blocks;
    uint32_t block_size;
    uint16_t sequence_number;
} SfsJournal;

static void worker_error(char *error, size_t error_size, const char *format, ...)
{
    if (error == NULL || error_size == 0U)
        return;
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}



static bool parse_unsigned(const char *text, unsigned *value)
{
    uint64_t parsed = 0U;
    if (!infiltratr_parse_u64_range(text, 10U, 0U, UINT_MAX, &parsed))
        return false;
    *value = (unsigned)parsed;
    return true;
}

static bool safe_value(const char *value)
{
    return value != NULL && strchr(value, '\n') == NULL &&
           strchr(value, '\r') == NULL && strchr(value, '=') == NULL;
}

static void unlink_if_exists(const char *path)
{
    if (path == NULL || *path == '\0')
        return;
    const int failure = infiltratr_unlink_durable(path, true);
    if (failure != 0)
        (void)fprintf(stderr, "%s: warning: cannot durably remove %s: %s\n",
                      PROG, path, strerror(failure));
}

static void journal_free(SfsJournal *state)
{
    if (state == NULL)
        return;
    free(state->device);
    free(state->target_identity);
    free(state->stage);
    memset(state, 0, sizeof(*state));
}

static int ensure_directory_tree(const char *path, char *error, size_t error_size)
{
    if (ld_path_ensure_trusted_directory_tree(path) != 0) {
        worker_error(error, error_size,
                     "cannot create SFS journal directory %s: %s",
                     path, strerror(errno));
        return -1;
    }
    return 0;
}

static bool journal_write_stream(FILE *file, const void *user_data)
{
    const SfsJournal *state = user_data;
    (void)fprintf(file, "%s\n", JOURNAL_MAGIC);
    (void)fprintf(file, "device=%s\n", state->device);
    (void)fprintf(file, "target_identity=%s\n", state->target_identity);
    (void)fprintf(file, "stage=%s\n", state->stage);
    (void)fprintf(file, "operation=%s\n", state->operation);
    (void)fprintf(file, "phase=%s\n", state->phase);
    (void)fprintf(file, "volume_token=%s\n", state->volume_token);
    (void)fprintf(file, "source_sha256=%s\n", state->source_sha256);
    (void)fprintf(file, "stage_sha256=%s\n", state->stage_sha256);
    (void)fprintf(file, "physical_bytes=%" PRIu64 "\n", state->physical_bytes);
    (void)fprintf(file, "filesystem_bytes=%" PRIu64 "\n", state->filesystem_bytes);
    (void)fprintf(file, "total_blocks=%u\n", state->total_blocks);
    (void)fprintf(file, "block_size=%u\n", state->block_size);
    (void)fprintf(file, "sequence_number=%u\n", (unsigned)state->sequence_number);
    return !ferror(file);
}

static int journal_save(const char *path, const SfsJournal *state,
                        char *error, size_t error_size)
{
    if (!safe_value(state->device) || !safe_value(state->target_identity) ||
        !safe_value(state->stage)) {
        worker_error(error, error_size,
                     "SFS transaction paths contain unsupported journal characters");
        return -1;
    }
    char *parent = ld_path_parent_directory(path);
    if (ensure_directory_tree(parent, error, error_size) != 0) {
        free(parent);
        return -1;
    }
    free(parent);
    const int failure = infiltratr_atomic_file_write(
        path, INFILTRATR_ATOMIC_FILE_PRIVATE, journal_write_stream, state);
    if (failure != 0) {
        worker_error(error, error_size,
                     "cannot publish SFS recovery journal: %s",
                     strerror(failure));
        return -1;
    }
    return 0;
}

static int parse_u64(const char *text, uint64_t *value)
{
    return infiltratr_parse_u64(text, 10U, value) ? 0 : -1;
}

static int journal_load(const char *path, SfsJournal *state,
                        char *error, size_t error_size)
{
    memset(state, 0, sizeof(*state));
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        worker_error(error, error_size,
                     "cannot open SFS recovery journal: %s", strerror(errno));
        return -1;
    }
    char *line = NULL;
    size_t capacity = 0U;
    if (getline(&line, &capacity, file) < 0)
        goto invalid;
    infiltratr_trim_line_end(line);
    if (strcmp(line, JOURNAL_MAGIC) != 0)
        goto invalid;
    while (getline(&line, &capacity, file) >= 0) {
        char *key = NULL;
        char *equals = NULL;
        if (infiltratr_config_parse_line(line, &key, &equals) !=
            INFILTRATR_CONFIG_LINE_ENTRY)
            goto invalid;
        if (strcmp(key, "device") == 0) {
            free(state->device);
            state->device = ld_xstrdup(equals);
        } else if (strcmp(key, "target_identity") == 0) {
            free(state->target_identity);
            state->target_identity = ld_xstrdup(equals);
        } else if (strcmp(key, "stage") == 0) {
            free(state->stage);
            state->stage = ld_xstrdup(equals);
        } else if (strcmp(key, "operation") == 0) {
            infiltratr_copy_string(state->operation, sizeof(state->operation), equals);
        } else if (strcmp(key, "phase") == 0) {
            infiltratr_copy_string(state->phase, sizeof(state->phase), equals);
        } else if (strcmp(key, "volume_token") == 0) {
            infiltratr_copy_string(state->volume_token, sizeof(state->volume_token), equals);
        } else if (strcmp(key, "source_sha256") == 0) {
            infiltratr_copy_string(state->source_sha256, sizeof(state->source_sha256), equals);
        } else if (strcmp(key, "stage_sha256") == 0) {
            infiltratr_copy_string(state->stage_sha256, sizeof(state->stage_sha256), equals);
        } else if (strcmp(key, "physical_bytes") == 0) {
            if (parse_u64(equals, &state->physical_bytes) != 0) goto invalid;
        } else if (strcmp(key, "filesystem_bytes") == 0) {
            if (parse_u64(equals, &state->filesystem_bytes) != 0) goto invalid;
        } else if (strcmp(key, "total_blocks") == 0) {
            uint64_t value = 0U;
            if (parse_u64(equals, &value) != 0 || value > UINT32_MAX) goto invalid;
            state->total_blocks = (uint32_t)value;
        } else if (strcmp(key, "block_size") == 0) {
            uint64_t value = 0U;
            if (parse_u64(equals, &value) != 0 || value > UINT32_MAX) goto invalid;
            state->block_size = (uint32_t)value;
        } else if (strcmp(key, "sequence_number") == 0) {
            uint64_t value = 0U;
            if (parse_u64(equals, &value) != 0 || value > UINT16_MAX) goto invalid;
            state->sequence_number = (uint16_t)value;
        }
    }
    free(line);
    (void)fclose(file);
    if (state->device == NULL || state->target_identity == NULL ||
        state->stage == NULL || state->operation[0] == '\0' ||
        state->phase[0] == '\0' || strlen(state->volume_token) != 64U ||
        strlen(state->source_sha256) != 64U ||
        strlen(state->stage_sha256) != 64U ||
        state->physical_bytes == 0U || state->filesystem_bytes == 0U ||
        state->total_blocks == 0U || state->block_size == 0U)
        goto invalid_state;
    return 0;

invalid:
    free(line);
    (void)fclose(file);
invalid_state:
    journal_free(state);
    worker_error(error, error_size,
                 "SFS recovery journal is malformed or incomplete");
    return -1;
}

static int journal_phase(const char *path, SfsJournal *state,
                         const char *phase, char *error, size_t error_size)
{
    infiltratr_copy_string(state->phase, sizeof(state->phase), phase);
    return journal_save(path, state, error, error_size);
}

static void transaction_cleanup(const char *journal, const SfsJournal *state)
{
    if (state != NULL)
        unlink_if_exists(state->stage);
    unlink_if_exists(journal);
}

static char *stage_name(const char *journal)
{
    return ld_path_append_suffix(journal, ".sfs-stage");
}

static int digest_final_hex(EVP_MD_CTX *context, char output[65],
                            char *error, size_t error_size)
{
    unsigned char digest[32];
    unsigned int length = 0U;
    if (EVP_DigestFinal_ex(context, digest, &length) != 1 || length != 32U) {
        worker_error(error, error_size, "finalising SFS SHA-256 failed");
        return -1;
    }
    static const char digits[] = "0123456789abcdef";
    for (size_t index = 0U; index < sizeof(digest); ++index) {
        output[index * 2U] = digits[digest[index] >> 4U];
        output[index * 2U + 1U] = digits[digest[index] & 15U];
    }
    output[64] = '\0';
    return 0;
}

static int hash_prefix(const char *path, uint64_t bytes, bool stoppable,
                       char output[65], char *error, size_t error_size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        worker_error(error, error_size,
                     "cannot open SFS data for SHA-256: %s", strerror(errno));
        return -1;
    }
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    uint8_t *buffer = malloc(SFS_WORKER_IO_BYTES);
    if (context == NULL || buffer == NULL ||
        EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
        free(buffer);
        EVP_MD_CTX_free(context);
        (void)close(fd);
        worker_error(error, error_size, "initialising SFS SHA-256 failed");
        return -1;
    }
    int rc = 0;
    for (uint64_t offset = 0U; offset < bytes;) {
        if (stoppable && ld_stop_requested()) {
            rc = STOPPED;
            break;
        }
        const uint64_t remaining = bytes - offset;
        const size_t count = remaining > SFS_WORKER_IO_BYTES ?
                             SFS_WORKER_IO_BYTES : (size_t)remaining;
        if (ld_pread_full(fd, buffer, count, offset) != (ssize_t)count ||
            EVP_DigestUpdate(context, buffer, count) != 1) {
            worker_error(error, error_size,
                         "hashing SFS bytes at offset %" PRIu64 " failed", offset);
            rc = -1;
            break;
        }
        offset += count;
    }
    if (rc == 0)
        rc = digest_final_hex(context, output, error, error_size);
    free(buffer);
    EVP_MD_CTX_free(context);
    (void)close(fd);
    return rc;
}

static int root_token(const char *path, uint32_t block_size,
                      uint32_t total_blocks, char output[65],
                      char *error, size_t error_size)
{
    if (block_size == 0U || total_blocks < 2U) {
        worker_error(error, error_size, "invalid SFS root-token geometry");
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    uint8_t *block = malloc(block_size);
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (fd < 0 || block == NULL || context == NULL ||
        EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
        if (fd >= 0) (void)close(fd);
        free(block);
        EVP_MD_CTX_free(context);
        worker_error(error, error_size, "cannot initialise SFS root identity");
        return -1;
    }
    int rc = 0;
    if (ld_pread_full(fd, block, block_size, 0U) != (ssize_t)block_size ||
        EVP_DigestUpdate(context, block, block_size) != 1 ||
        ld_pread_full(fd, block, block_size,
                      (uint64_t)(total_blocks - 1U) * block_size) !=
            (ssize_t)block_size ||
        EVP_DigestUpdate(context, block, block_size) != 1) {
        worker_error(error, error_size, "cannot read SFS redundant roots for identity");
        rc = -1;
    }
    if (rc == 0)
        rc = digest_final_hex(context, output, error, error_size);
    (void)close(fd);
    free(block);
    EVP_MD_CTX_free(context);
    return rc;
}

static int capture_target(const char *device, SfsJournal *state,
                          char *error, size_t error_size)
{
    if (ld_device_capture_binding(device, &state->device,
                                  &state->target_identity,
                                  &state->physical_bytes) != 0) {
        worker_error(error, error_size,
                     "cannot bind SFS target: %s", strerror(errno));
        return -1;
    }
    SfsAnalysis analysis;
    if (sfs_analyse(state->device, &analysis, NULL, 0U,
                    error, error_size) != 0)
        return -1;
    if (analysis.transaction_pending) {
        worker_error(error, error_size,
                     "SFS filesystem has an unfinished native transaction marker");
        return -1;
    }
    state->filesystem_bytes = analysis.filesystem_bytes;
    state->total_blocks = analysis.total_blocks;
    state->block_size = analysis.block_size;
    state->sequence_number = analysis.sequence_number;
    if (state->filesystem_bytes > state->physical_bytes) {
        worker_error(error, error_size, "SFS filesystem exceeds target capacity");
        return -1;
    }
    if (root_token(state->device, state->block_size, state->total_blocks,
                   state->volume_token, error, error_size) != 0)
        return -1;
    return hash_prefix(state->device, state->filesystem_bytes, true,
                       state->source_sha256, error, error_size);
}

static int check_target_identity(const char *device, const SfsJournal *state,
                                 char *error, size_t error_size)
{
    char *canonical = NULL;
    char *identity = NULL;
    uint64_t physical_bytes = 0U;
    int rc = ld_device_capture_binding(
        device, &canonical, &identity, &physical_bytes);
    if (rc != 0) {
        worker_error(error, error_size,
                     "cannot rebind SFS target: %s", strerror(errno));
    }
    if (rc == 0 &&
        (strcmp(canonical, state->device) != 0 ||
         strcmp(identity, state->target_identity) != 0 ||
         physical_bytes != state->physical_bytes)) {
        worker_error(error, error_size,
                     "SFS target path, identity or capacity changed before commit");
        rc = -1;
    }
    char token[65];
    if (rc == 0 &&
        (root_token(canonical, state->block_size, state->total_blocks,
                    token, error, error_size) != 0 ||
         strcmp(token, state->volume_token) != 0)) {
        if (error[0] == '\0')
            worker_error(error, error_size,
                         "SFS redundant-root identity changed before commit");
        rc = -1;
    }
    free(identity);
    free(canonical);
    return rc;
}

static int check_source_unchanged(const char *device, const SfsJournal *state,
                                  char *error, size_t error_size)
{
    if (check_target_identity(device, state, error, error_size) != 0)
        return -1;
    char digest[65];
    const int rc = hash_prefix(state->device, state->filesystem_bytes, true,
                               digest, error, error_size);
    if (rc != 0)
        return rc;
    if (strcmp(digest, state->source_sha256) != 0) {
        worker_error(error, error_size,
                     "SFS source changed after preflight; refusing source writes");
        return -1;
    }
    return 0;
}

static int stage_sha256(const char *stage, const SfsJournal *state,
                        char output[65], char *error, size_t error_size)
{
    SfsAnalysis analysis;
    if (sfs_analyse(stage, &analysis, NULL, 0U, error, error_size) != 0)
        return -1;
    if (analysis.filesystem_bytes != state->filesystem_bytes ||
        analysis.total_blocks != state->total_blocks ||
        analysis.block_size != state->block_size) {
        worker_error(error, error_size,
                     "verified SFS stage geometry differs from source");
        return -1;
    }
    return hash_prefix(stage, state->filesystem_bytes, true,
                       output, error, error_size);
}

static int stop_commit(int target, char *error, size_t error_size)
{
    if (fsync(target) != 0) {
        worker_error(error, error_size,
                     "cannot sync SFS target at Stop boundary: %s",
                     strerror(errno));
        return -1;
    }
    return STOPPED;
}

static int safe_commit_stage(const char *stage_path, const char *target_path,
                             const SfsJournal *state, uint64_t *written,
                             char *error, size_t error_size)
{
    SfsAnalysis analysis;
    if (sfs_analyse(stage_path, &analysis, NULL, 0U,
                    error, error_size) != 0)
        return -1;
    if (analysis.filesystem_bytes != state->filesystem_bytes) {
        worker_error(error, error_size, "SFS stage size changed before commit");
        return -1;
    }
    int stage = open(stage_path, O_RDONLY | O_CLOEXEC);
    int target = ld_device_open_verified_fd(target_path, true,
                                            state->target_identity,
                                            state->physical_bytes);
    if (stage < 0 || target < 0) {
        if (stage >= 0) (void)close(stage);
        if (target >= 0) (void)close(target);
        worker_error(error, error_size,
                     "cannot open SFS stage or source for commit: %s",
                     strerror(errno));
        return -1;
    }
    if (flock(target, LOCK_EX | LOCK_NB) != 0) {
        worker_error(error, error_size,
                     "cannot lock SFS source for commit: %s", strerror(errno));
        (void)close(stage);
        (void)close(target);
        return -1;
    }
    uint8_t *buffer = malloc(SFS_WORKER_IO_BYTES);
    if (buffer == NULL) {
        worker_error(error, error_size,
                     "out of memory batching SFS source commit");
        (void)flock(target, LOCK_UN);
        (void)close(stage);
        (void)close(target);
        return -1;
    }
    uint64_t total_written = 0U;
    int rc = 0;
    for (uint64_t offset = 0U; offset < state->filesystem_bytes;) {
        if (ld_stop_requested()) {
            rc = stop_commit(target, error, error_size);
            break;
        }
        const uint64_t remaining = state->filesystem_bytes - offset;
        const size_t count = remaining > SFS_WORKER_IO_BYTES ?
                             SFS_WORKER_IO_BYTES : (size_t)remaining;
        if (ld_pread_full(stage, buffer, count, offset) != (ssize_t)count ||
            ld_pwrite_full(target, buffer, count, offset) != (ssize_t)count) {
            worker_error(error, error_size,
                         "short I/O committing SFS bytes at offset %" PRIu64,
                         offset);
            rc = -1;
            break;
        }
        total_written += count;
        offset += count;
    }
    if (rc == 0 && ld_stop_requested())
        rc = stop_commit(target, error, error_size);
    if (rc == 0 && fsync(target) != 0) {
        worker_error(error, error_size,
                     "cannot sync SFS source: %s", strerror(errno));
        rc = -1;
    }
    free(buffer);
    (void)flock(target, LOCK_UN);
    (void)close(stage);
    (void)close(target);
    if (written != NULL)
        *written = total_written;
    return rc;
}

static bool valid_operation(const char *operation)
{
    return strcmp(operation, "defrag") == 0 ||
           strcmp(operation, "growth-defrag") == 0;
}

static bool valid_phase(const char *phase)
{
    return strcmp(phase, "staged") == 0 ||
           strcmp(phase, "committing") == 0 ||
           strcmp(phase, "committed") == 0;
}

static int handle_recovery(const char *device, const char *journal,
                           bool live, char *error, size_t error_size)
{
    SfsJournal state;
    if (journal_load(journal, &state, error, error_size) != 0)
        return 1;
    if (!ld_path_is_derived_from(state.stage, journal, ".sfs-stage")) {
        worker_error(error, error_size,
                     "SFS recovery stage is not derived from the selected journal path");
        journal_free(&state);
        return 1;
    }
    if (!valid_operation(state.operation) || !valid_phase(state.phase)) {
        worker_error(error, error_size,
                     "SFS recovery journal contains an unsupported operation or phase");
        journal_free(&state);
        return 1;
    }
    int identity_rc = strcmp(state.phase, "staged") == 0
        ? check_source_unchanged(device, &state, error, error_size)
        : check_target_identity(device, &state, error, error_size);
    if (identity_rc == STOPPED) {
        ld_emit_result_event(stdout, "recover", "stopped",
               "Stopped before recovery source writes; artifacts remain intact.");
        journal_free(&state);
        return STOPPED;
    }
    if (identity_rc != 0) {
        journal_free(&state);
        return 1;
    }
    char digest[65];
    int digest_rc = stage_sha256(state.stage, &state, digest,
                                 error, error_size);
    if (digest_rc == STOPPED) {
        ld_emit_result_event(stdout, "recover", "stopped",
               "Stopped before recovery source writes; artifacts remain intact.");
        journal_free(&state);
        return STOPPED;
    }
    if (digest_rc != 0 || strcmp(digest, state.stage_sha256) != 0) {
        if (digest_rc == 0)
            worker_error(error, error_size,
                         "SFS recovery stage SHA-256 does not match the journal");
        journal_free(&state);
        return 1;
    }
    const bool growth = strcmp(state.operation, "growth-defrag") == 0;
    if (sfs_verify_layout(state.stage, growth, 10U,
                          error, error_size) != 0) {
        journal_free(&state);
        return 1;
    }
    if (strcmp(state.phase, "committed") == 0) {
        if (sfs_verify_layout(device, growth, 10U,
                              error, error_size) != 0) {
            journal_free(&state);
            return 1;
        }
        transaction_cleanup(journal, &state);
        ld_emit_result_event(stdout, "recover", "completed",
               "Verified an already committed SFS transaction.");
        journal_free(&state);
        return 0;
    }
    if (journal_phase(journal, &state, "committing",
                      error, error_size) != 0) {
        journal_free(&state);
        return 1;
    }
    uint64_t written = 0U;
    const int commit_rc = safe_commit_stage(
        state.stage, state.device, &state, &written, error, error_size);
    if (commit_rc == STOPPED) {
        ld_emit_result_event(stdout, "recover", "stopped",
               "Recovery stopped at a durable boundary and can be resumed.");
        journal_free(&state);
        return STOPPED;
    }
    if (commit_rc != 0 ||
        sfs_verify_layout(state.device, growth, 10U,
                          error, error_size) != 0) {
        journal_free(&state);
        return 1;
    }
    if (journal_phase(journal, &state, "committed",
                      error, error_size) != 0) {
        journal_free(&state);
        return 1;
    }
    transaction_cleanup(journal, &state);
    if (live) {
        (void)printf("@@LIVE_RESET {\"reason\":\"authoritative post-recovery SFS map\"}\n");
        (void)fflush(stdout);
    }
    (void)printf("Recovered verified SFS source; committed %" PRIu64 " KiB.\n",
                 written / 1024U);
    ld_emit_result_event(stdout, "recover", "completed", "");
    journal_free(&state);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        (void)printf("%s %s\n", PROG, LD_VERSION);
        return 0;
    }
    if (argc < 3) {
        usage(stderr);
        return 2;
    }

    const char *mode = argv[1];
    const char *device = argv[2];
    if (strcmp(mode, "identify") == 0 && argc == 3) {
        SfsAnalysis analysis;
        if (analyse(device, &analysis) != 0)
            return 1;
        print_identify(&analysis);
        return 0;
    }
    if (strcmp(mode, "analyse-json") == 0 && argc == 3) {
        SfsAnalysis analysis;
        if (analyse(device, &analysis) != 0)
            return 1;
        print_analysis_json(&analysis);
        return 0;
    }
    if (strcmp(mode, "map") == 0 && argc == 5 &&
        strcmp(argv[3], "--cells") == 0) {
        uint64_t cells = 0U;
        if (parse_cells(argv[4], &cells) != 0) {
            (void)fprintf(stderr, "%s: invalid cell count\n", PROG);
            return 2;
        }
        return print_map(device, cells) == 0 ? 0 : 1;
    }

    const bool growth = strcmp(mode, "growth-defrag") == 0;
    const bool defrag = strcmp(mode, "defrag") == 0;
    const bool recover = strcmp(mode, "recover") == 0;
    if (!growth && !defrag && !recover) {
        usage(stderr);
        return 2;
    }

    const char *confirm = NULL;
    const char *journal = NULL;
    unsigned growth_percent = 10U;
    bool write = false;
    bool live = false;
    for (int index = 3; index < argc; ++index) {
        if (strcmp(argv[index], "--write") == 0) {
            write = true;
        } else if (strcmp(argv[index], "--live-updates") == 0) {
            live = true;
        } else if (strcmp(argv[index], "--confirm") == 0 &&
                   index + 1 < argc) {
            confirm = argv[++index];
        } else if (strcmp(argv[index], "--journal") == 0 &&
                   index + 1 < argc) {
            journal = argv[++index];
        } else if (strcmp(argv[index], "--growth-percent") == 0 &&
                   index + 1 < argc) {
            if (!parse_unsigned(argv[++index], &growth_percent)) {
                (void)fprintf(stderr, "%s: invalid --growth-percent\n", PROG);
                return 2;
            }
        } else if ((strcmp(argv[index], "--workers") == 0 ||
                    strcmp(argv[index], "--ram-buffer") == 0 ||
                    strcmp(argv[index], "--batch-clusters") == 0 ||
                    strcmp(argv[index], "--live-map-cells") == 0) &&
                   index + 1 < argc) {
            ++index;
        } else {
            (void)fprintf(stderr, "%s: unknown or incomplete SFS option: %s\n",
                          PROG, argv[index]);
            return 2;
        }
    }
    if (!write || confirm == NULL || journal == NULL ||
        strcmp(confirm, device) != 0) {
        (void)fprintf(stderr,
                      "%s: SFS mutation requires --write --confirm DEVICE --journal PATH\n",
                      PROG);
        return 2;
    }
    if (growth && growth_percent != 10U) {
        (void)fprintf(stderr,
                      "%s: SFS Growth Defrag requires exactly 10 percent reserve\n",
                      PROG);
        return 2;
    }
    if (ld_path_is_mounted(device)) {
        (void)fprintf(stderr,
                      "%s: SFS target is mounted; raw mutation and recovery require an unmounted filesystem\n",
                      PROG);
        return 1;
    }

    ld_stop_clear();
    ld_stop_install_handlers();
    char error[512] = {0};
    if (recover) {
        const int rc = handle_recovery(device, journal, live,
                                       error, sizeof(error));
        if (rc != 0 && rc != STOPPED)
            (void)fprintf(stderr, "%s: %s\n", PROG,
                          error[0] != '\0' ? error : "SFS recovery failed");
        return rc;
    }

    SfsJournal state;
    memset(&state, 0, sizeof(state));
    infiltratr_copy_string(state.operation, sizeof(state.operation), mode);
    int capture_rc = capture_target(device, &state, error, sizeof(error));
    if (capture_rc == STOPPED) {
        ld_emit_result_event(stdout, mode, "stopped", "Stopped during read-only SFS preflight.");
        journal_free(&state);
        return STOPPED;
    }
    if (capture_rc != 0)
        goto fail;

    state.stage = stage_name(journal);
    if (state.stage == NULL) {
        worker_error(error, sizeof(error), "out of memory creating SFS stage path");
        goto fail;
    }
    if (access(journal, F_OK) == 0 || access(state.stage, F_OK) == 0) {
        worker_error(error, sizeof(error),
                     "existing SFS recovery artifacts must be recovered or removed before starting a new transaction");
        goto fail;
    }

    (void)printf("Starting native C SFS0 %s on %s.\n",
                 growth ? "Growth Defrag" : "Defrag", device);
    uint64_t planned = 0U;
    if (sfs_build_stage(state.device, state.stage, growth, growth_percent,
                        live, &planned, error, sizeof(error)) != 0 ||
        sfs_verify_layout(state.stage, growth, growth_percent,
                          error, sizeof(error)) != 0) {
        unlink_if_exists(state.stage);
        goto fail;
    }
    if (ld_stop_requested()) {
        unlink_if_exists(state.stage);
        ld_emit_result_event(stdout, mode, "stopped", "Stopped before any SFS source writes.");
        journal_free(&state);
        return STOPPED;
    }

    int unchanged_rc = check_source_unchanged(
        state.device, &state, error, sizeof(error));
    if (unchanged_rc == STOPPED) {
        unlink_if_exists(state.stage);
        ld_emit_result_event(stdout, mode, "stopped", "Stopped before any SFS source writes.");
        journal_free(&state);
        return STOPPED;
    }
    if (unchanged_rc != 0) {
        unlink_if_exists(state.stage);
        goto fail;
    }
    int hash_rc = stage_sha256(state.stage, &state, state.stage_sha256,
                               error, sizeof(error));
    if (hash_rc == STOPPED) {
        unlink_if_exists(state.stage);
        ld_emit_result_event(stdout, mode, "stopped", "Stopped before any SFS source writes.");
        journal_free(&state);
        return STOPPED;
    }
    if (hash_rc != 0) {
        unlink_if_exists(state.stage);
        goto fail;
    }

    infiltratr_copy_string(state.phase, sizeof(state.phase), "staged");
    if (journal_save(journal, &state, error, sizeof(error)) != 0) {
        unlink_if_exists(state.stage);
        goto fail;
    }
    if (ld_stop_requested()) {
        transaction_cleanup(journal, &state);
        ld_emit_result_event(stdout, mode, "stopped", "Stopped before any SFS source writes.");
        journal_free(&state);
        return STOPPED;
    }
    if (journal_phase(journal, &state, "committing",
                      error, sizeof(error)) != 0)
        goto fail;

    (void)printf("SFS source commit: replaying %" PRIu64
                 " KiB from the verified full-filesystem stage.\n",
                 planned / 1024U);
    (void)fflush(stdout);
    uint64_t written = 0U;
    const int commit_rc = safe_commit_stage(
        state.stage, state.device, &state, &written, error, sizeof(error));
    if (commit_rc == STOPPED) {
        ld_emit_result_event(stdout, mode, "stopped",
               "Run Recover to resume the verified SFS transaction.");
        journal_free(&state);
        return STOPPED;
    }
    if (commit_rc != 0 ||
        sfs_verify_layout(state.device, growth, growth_percent,
                          error, sizeof(error)) != 0)
        goto fail;

    if (journal_phase(journal, &state, "committed",
                      error, sizeof(error)) != 0)
        goto fail;
    transaction_cleanup(journal, &state);
    if (live) {
        (void)printf("@@LIVE_RESET {\"reason\":\"authoritative post-commit SFS map\"}\n");
        (void)fflush(stdout);
    }
    (void)printf("SFS0 %s completed; committed %" PRIu64 " KiB.\n",
                 growth ? "Growth Defrag" : "Defrag", written / 1024U);
    ld_emit_result_event(stdout, mode, "completed", "");
    journal_free(&state);
    return 0;

fail:
    (void)fprintf(stderr, "%s: %s\n", PROG,
                  error[0] != '\0' ? error : "SFS transaction failed");
    journal_free(&state);
    return 1;
}
