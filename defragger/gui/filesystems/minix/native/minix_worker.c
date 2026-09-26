// SPDX-License-Identifier: GPL-3.0-or-later
#include "minix_native.h"
#include "version.h"

#include "infiltratr/arithmetic.h"
#include "infiltratr/config.h"
#include "infiltratr/core.h"
#include "infiltratr/posix.h"
#include "ld_device.h"
#include "ld_io.h"
#include "ld_path.h"
#include "ld_protocol.h"
#include "ld_runtime.h"
#include "ld_stop.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define PROG "linux-defragger-minix-worker"
#define JOURNAL_MAGIC "LINUX-DEFRAGGER-MINIX-JOURNAL-1"
#define HASH_CHUNK (1024U * 1024U)
#define COMMIT_CHUNK (4U * 1024U * 1024U)
#define STOPPED 130

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
    uint32_t block_size;
    uint32_t zone_count;
    uint32_t first_data_zone;
    uint32_t magic;
    uint32_t version;
} MinixJournal;

static void usage(FILE *stream)
{
    (void)fprintf(
        stream,
        "Usage: %s --version | identify DEVICE | analyse-json DEVICE | "
        "map DEVICE --cells COUNT | "
        "defrag|growth-defrag|recover DEVICE --write --confirm DEVICE "
        "--journal PATH [--growth-percent 10] [--live-updates]\n",
        PROG);
}

static int parse_cells(const char *text, uint64_t *cells)
{
    return infiltratr_parse_u64_range(text, 10U, 1U, UINT64_MAX, cells)
        ? 0 : -1;
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

static void worker_error(char *error, size_t error_size,
                         const char *format, ...)
{
    if (error == NULL || error_size == 0U)
        return;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(error, error_size, format, args);
    va_end(args);
}

static void unlink_if_exists(const char *path)
{
    if (path == NULL || *path == '\0')
        return;
    const int failure = infiltratr_unlink_durable(path, true);
    if (failure != 0)
        (void)fprintf(stderr,
                      "%s: warning: cannot durably remove %s: %s\n",
                      PROG, path, strerror(failure));
}

static void journal_free(MinixJournal *state)
{
    if (state == NULL)
        return;
    free(state->device);
    free(state->target_identity);
    free(state->stage);
    memset(state, 0, sizeof(*state));
}

static bool journal_write_stream(FILE *file, const void *user_data)
{
    const MinixJournal *state = user_data;
    (void)fprintf(file, "%s\n", JOURNAL_MAGIC);
    (void)fprintf(file, "device=%s\n", state->device);
    (void)fprintf(file, "target_identity=%s\n", state->target_identity);
    (void)fprintf(file, "stage=%s\n", state->stage);
    (void)fprintf(file, "operation=%s\n", state->operation);
    (void)fprintf(file, "phase=%s\n", state->phase);
    (void)fprintf(file, "volume_token=%s\n", state->volume_token);
    (void)fprintf(file, "source_sha256=%s\n", state->source_sha256);
    (void)fprintf(file, "stage_sha256=%s\n", state->stage_sha256);
    (void)fprintf(file, "physical_bytes=%" PRIu64 "\n",
                  state->physical_bytes);
    (void)fprintf(file, "filesystem_bytes=%" PRIu64 "\n",
                  state->filesystem_bytes);
    (void)fprintf(file, "block_size=%u\n", state->block_size);
    (void)fprintf(file, "zone_count=%u\n", state->zone_count);
    (void)fprintf(file, "first_data_zone=%u\n", state->first_data_zone);
    (void)fprintf(file, "magic=%u\n", state->magic);
    (void)fprintf(file, "version=%u\n", state->version);
    return !ferror(file);
}

static int journal_save(const char *path, const MinixJournal *state,
                        char *error, size_t error_size)
{
    if (!safe_value(state->device) ||
        !safe_value(state->target_identity) ||
        !safe_value(state->stage)) {
        worker_error(error, error_size,
                     "Minix transaction paths contain unsupported journal characters");
        return -1;
    }
    char *parent = ld_path_parent_directory(path);
    if (parent == NULL ||
        ld_path_ensure_trusted_directory_tree(parent) != 0) {
        worker_error(error, error_size,
                     "cannot create Minix journal directory: %s",
                     strerror(errno));
        free(parent);
        return -1;
    }
    free(parent);

    const int failure = infiltratr_atomic_file_write(
        path, INFILTRATR_ATOMIC_FILE_PRIVATE,
        journal_write_stream, state);
    if (failure != 0) {
        worker_error(error, error_size,
                     "cannot publish Minix recovery journal: %s",
                     strerror(failure));
        return -1;
    }
    return 0;
}

static int parse_u64(const char *text, uint64_t *value)
{
    return infiltratr_parse_u64(text, 10U, value) ? 0 : -1;
}

static int journal_load(const char *path, MinixJournal *state,
                        char *error, size_t error_size)
{
    memset(state, 0, sizeof(*state));
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        worker_error(error, error_size,
                     "cannot open Minix recovery journal: %s",
                     strerror(errno));
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
        char *value = NULL;
        if (infiltratr_config_parse_line(line, &key, &value) !=
            INFILTRATR_CONFIG_LINE_ENTRY)
            goto invalid;
        if (strcmp(key, "device") == 0) {
            free(state->device);
            state->device = ld_xstrdup(value);
        } else if (strcmp(key, "target_identity") == 0) {
            free(state->target_identity);
            state->target_identity = ld_xstrdup(value);
        } else if (strcmp(key, "stage") == 0) {
            free(state->stage);
            state->stage = ld_xstrdup(value);
        } else if (strcmp(key, "operation") == 0) {
            infiltratr_copy_string(state->operation,
                                    sizeof(state->operation), value);
        } else if (strcmp(key, "phase") == 0) {
            infiltratr_copy_string(state->phase,
                                    sizeof(state->phase), value);
        } else if (strcmp(key, "volume_token") == 0) {
            infiltratr_copy_string(state->volume_token,
                                    sizeof(state->volume_token), value);
        } else if (strcmp(key, "source_sha256") == 0) {
            infiltratr_copy_string(state->source_sha256,
                                    sizeof(state->source_sha256), value);
        } else if (strcmp(key, "stage_sha256") == 0) {
            infiltratr_copy_string(state->stage_sha256,
                                    sizeof(state->stage_sha256), value);
        } else if (strcmp(key, "physical_bytes") == 0) {
            if (parse_u64(value, &state->physical_bytes) != 0)
                goto invalid;
        } else if (strcmp(key, "filesystem_bytes") == 0) {
            if (parse_u64(value, &state->filesystem_bytes) != 0)
                goto invalid;
        } else if (strcmp(key, "block_size") == 0) {
            uint64_t parsed = 0U;
            if (parse_u64(value, &parsed) != 0 || parsed > UINT32_MAX)
                goto invalid;
            state->block_size = (uint32_t)parsed;
        } else if (strcmp(key, "zone_count") == 0) {
            uint64_t parsed = 0U;
            if (parse_u64(value, &parsed) != 0 || parsed > UINT32_MAX)
                goto invalid;
            state->zone_count = (uint32_t)parsed;
        } else if (strcmp(key, "first_data_zone") == 0) {
            uint64_t parsed = 0U;
            if (parse_u64(value, &parsed) != 0 || parsed > UINT32_MAX)
                goto invalid;
            state->first_data_zone = (uint32_t)parsed;
        } else if (strcmp(key, "magic") == 0) {
            uint64_t parsed = 0U;
            if (parse_u64(value, &parsed) != 0 || parsed > UINT32_MAX)
                goto invalid;
            state->magic = (uint32_t)parsed;
        } else if (strcmp(key, "version") == 0) {
            uint64_t parsed = 0U;
            if (parse_u64(value, &parsed) != 0 || parsed > UINT32_MAX)
                goto invalid;
            state->version = (uint32_t)parsed;
        }
    }
    free(line);
    (void)fclose(file);

    if (state->device == NULL ||
        state->target_identity == NULL ||
        state->stage == NULL ||
        state->operation[0] == '\0' ||
        state->phase[0] == '\0' ||
        strlen(state->volume_token) != 64U ||
        strlen(state->source_sha256) != 64U ||
        strlen(state->stage_sha256) != 64U ||
        state->physical_bytes == 0U ||
        state->filesystem_bytes == 0U ||
        state->block_size == 0U ||
        state->zone_count == 0U ||
        state->first_data_zone == 0U ||
        state->magic == 0U ||
        state->version == 0U)
        goto invalid_state;
    return 0;

invalid:
    free(line);
    (void)fclose(file);
invalid_state:
    journal_free(state);
    worker_error(error, error_size,
                 "Minix recovery journal is malformed or incomplete");
    return -1;
}

static int journal_phase(const char *path, MinixJournal *state,
                         const char *phase,
                         char *error, size_t error_size)
{
    infiltratr_copy_string(state->phase, sizeof(state->phase), phase);
    return journal_save(path, state, error, error_size);
}

static void transaction_cleanup(const char *journal,
                                const MinixJournal *state)
{
    if (state != NULL)
        unlink_if_exists(state->stage);
    unlink_if_exists(journal);
}

static int digest_final_hex(EVP_MD_CTX *context, char output[65],
                            char *error, size_t error_size)
{
    unsigned char digest[32];
    unsigned int length = 0U;
    if (EVP_DigestFinal_ex(context, digest, &length) != 1 ||
        length != sizeof(digest)) {
        worker_error(error, error_size,
                     "finalising Minix SHA-256 failed");
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

static int hash_region(const char *path, uint64_t offset, uint64_t bytes,
                       bool stoppable, char output[65],
                       char *error, size_t error_size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        worker_error(error, error_size,
                     "cannot open Minix data for SHA-256: %s",
                     strerror(errno));
        return -1;
    }
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    uint8_t *buffer = malloc(HASH_CHUNK);
    if (context == NULL || buffer == NULL ||
        EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
        free(buffer);
        EVP_MD_CTX_free(context);
        (void)close(fd);
        worker_error(error, error_size,
                     "initialising Minix SHA-256 failed");
        return -1;
    }

    int result = 0;
    for (uint64_t done = 0U; done < bytes;) {
        if (stoppable && ld_stop_requested()) {
            result = STOPPED;
            break;
        }
        const uint64_t remaining = bytes - done;
        const size_t take =
            remaining > HASH_CHUNK ? HASH_CHUNK : (size_t)remaining;
        if (ld_pread_full(fd, buffer, take, offset + done) !=
                (ssize_t)take ||
            EVP_DigestUpdate(context, buffer, take) != 1) {
            worker_error(error, error_size,
                         "hashing Minix data failed");
            result = -1;
            break;
        }
        done += take;
    }
    if (result == 0)
        result = digest_final_hex(context, output, error, error_size);
    free(buffer);
    EVP_MD_CTX_free(context);
    (void)close(fd);
    return result;
}

static int volume_token(const char *path, char output[65],
                        char *error, size_t error_size)
{
    return hash_region(path, 1024U, 64U, false,
                       output, error, error_size);
}

static int capture_target(const char *device, MinixJournal *state,
                          char *error, size_t error_size)
{
    if (ld_device_capture_binding(device, &state->device,
                                  &state->target_identity,
                                  &state->physical_bytes) != 0) {
        worker_error(error, error_size,
                     "cannot bind Minix target: %s", strerror(errno));
        return -1;
    }

    MinixAnalysis analysis;
    if (minix_analyse(state->device, &analysis, NULL, 0U,
                      error, error_size) != 0)
        return -1;
    state->filesystem_bytes = analysis.filesystem_bytes;
    state->block_size = analysis.summary.block_size;
    state->zone_count = analysis.summary.zone_count;
    state->first_data_zone = analysis.summary.first_data_zone;
    state->magic = analysis.summary.magic;
    state->version = analysis.summary.version;
    if (state->filesystem_bytes > state->physical_bytes) {
        worker_error(error, error_size,
                     "Minix filesystem exceeds target capacity");
        return -1;
    }
    if (volume_token(state->device, state->volume_token,
                     error, error_size) != 0)
        return -1;
    return hash_region(state->device, 0U, state->filesystem_bytes,
                       true, state->source_sha256,
                       error, error_size);
}

static int check_target_identity(const char *device,
                                 const MinixJournal *state,
                                 char *error, size_t error_size)
{
    char *canonical = NULL;
    char *identity = NULL;
    uint64_t physical_bytes = 0U;
    int result = ld_device_capture_binding(
        device, &canonical, &identity, &physical_bytes);
    if (result != 0) {
        worker_error(error, error_size,
                     "cannot rebind Minix target: %s", strerror(errno));
    }
    if (result == 0 &&
        (strcmp(canonical, state->device) != 0 ||
         strcmp(identity, state->target_identity) != 0 ||
         physical_bytes != state->physical_bytes)) {
        worker_error(error, error_size,
                     "Minix target path, identity or capacity changed before commit");
        result = -1;
    }

    MinixSummary summary;
    if (result == 0 &&
        minix_read_summary(canonical, &summary,
                           error, error_size) != 0)
        result = -1;
    if (result == 0 &&
        (summary.magic != state->magic ||
         summary.version != state->version ||
         summary.block_size != state->block_size ||
         summary.zone_count != state->zone_count ||
         summary.first_data_zone != state->first_data_zone ||
         (uint64_t)summary.zone_count * summary.zone_size !=
             state->filesystem_bytes)) {
        worker_error(error, error_size,
                     "Minix filesystem geometry changed before source commit");
        result = -1;
    }

    char token[65];
    if (result == 0 &&
        (volume_token(canonical, token, error, error_size) != 0 ||
         strcmp(token, state->volume_token) != 0)) {
        if (error != NULL && error[0] == '\0')
            worker_error(error, error_size,
                         "Minix superblock identity changed before source commit");
        result = -1;
    }
    free(identity);
    free(canonical);
    return result;
}

static int check_source_unchanged(const char *device,
                                  const MinixJournal *state,
                                  char *error, size_t error_size)
{
    if (check_target_identity(device, state,
                              error, error_size) != 0)
        return -1;
    char digest[65];
    const int result =
        hash_region(state->device, 0U, state->filesystem_bytes,
                    true, digest, error, error_size);
    if (result != 0)
        return result;
    if (strcmp(digest, state->source_sha256) != 0) {
        worker_error(error, error_size,
                     "Minix source changed after preflight; refusing source writes");
        return -1;
    }
    return 0;
}

static int stage_sha256(const char *stage,
                        const MinixJournal *state,
                        char output[65],
                        char *error, size_t error_size)
{
    MinixAnalysis analysis;
    if (minix_analyse(stage, &analysis, NULL, 0U,
                      error, error_size) != 0)
        return -1;
    if (analysis.filesystem_bytes != state->filesystem_bytes ||
        analysis.summary.block_size != state->block_size ||
        analysis.summary.zone_count != state->zone_count ||
        analysis.summary.first_data_zone != state->first_data_zone ||
        analysis.summary.magic != state->magic ||
        analysis.summary.version != state->version) {
        worker_error(error, error_size,
                     "verified Minix stage geometry differs from source");
        return -1;
    }
    return hash_region(stage, 0U, state->filesystem_bytes,
                       true, output, error, error_size);
}

static int verify_committed_source(const MinixJournal *state,
                                   bool growth,
                                   char *error, size_t error_size)
{
    if (minix_verify_layout(state->device, growth, 10U,
                            error, error_size) != 0)
        return -1;
    char digest[65];
    const int result =
        hash_region(state->device, 0U, state->filesystem_bytes,
                    true, digest, error, error_size);
    if (result != 0)
        return result;
    if (strcmp(digest, state->stage_sha256) != 0) {
        worker_error(error, error_size,
                     "Minix committed source does not match the verified stage");
        return -1;
    }
    return 0;
}

static int stop_commit(int target,
                       char *error, size_t error_size)
{
    if (ld_sync_fd(target) != 0) {
        worker_error(error, error_size,
                     "cannot sync Minix target at Stop boundary: %s",
                     strerror(errno));
        return -1;
    }
    return STOPPED;
}

static int safe_commit_stage(const MinixJournal *state,
                             uint64_t *written,
                             char *error, size_t error_size)
{
    int stage = open(state->stage, O_RDONLY | O_CLOEXEC);
    int target = ld_device_open_verified_fd(
        state->device, true,
        state->target_identity, state->physical_bytes);
    if (stage < 0 || target < 0) {
        if (stage >= 0)
            (void)close(stage);
        if (target >= 0)
            (void)close(target);
        worker_error(error, error_size,
                     "cannot open Minix stage or source for commit: %s",
                     strerror(errno));
        return -1;
    }
    if (flock(target, LOCK_EX | LOCK_NB) != 0) {
        worker_error(error, error_size,
                     "cannot lock Minix source for commit: %s",
                     strerror(errno));
        (void)close(stage);
        (void)close(target);
        return -1;
    }

    uint8_t *buffer = malloc(COMMIT_CHUNK);
    if (buffer == NULL) {
        worker_error(error, error_size,
                     "out of memory batching Minix source commit");
        (void)flock(target, LOCK_UN);
        (void)close(stage);
        (void)close(target);
        return -1;
    }

    uint64_t total_written = 0U;
    int result = 0;
    for (uint64_t offset = 0U;
         offset < state->filesystem_bytes;) {
        if (ld_stop_requested()) {
            result = stop_commit(target, error, error_size);
            break;
        }
        const uint64_t remaining =
            state->filesystem_bytes - offset;
        const size_t take =
            remaining > COMMIT_CHUNK ? COMMIT_CHUNK :
            (size_t)remaining;
        if (ld_pread_full(stage, buffer, take, offset) !=
                (ssize_t)take ||
            ld_pwrite_full(target, buffer, take, offset) !=
                (ssize_t)take) {
            worker_error(error, error_size,
                         "short I/O committing Minix bytes");
            result = -1;
            break;
        }
        total_written += take;
        offset += take;
    }
    if (result == 0 && ld_stop_requested())
        result = stop_commit(target, error, error_size);
    if (result == 0 && ld_sync_fd(target) != 0) {
        worker_error(error, error_size,
                     "cannot sync Minix source: %s",
                     strerror(errno));
        result = -1;
    }

    free(buffer);
    (void)flock(target, LOCK_UN);
    (void)close(stage);
    (void)close(target);
    if (written != NULL)
        *written = total_written;
    return result;
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
                           bool live,
                           char *error, size_t error_size)
{
    MinixJournal state;
    if (journal_load(journal, &state,
                     error, error_size) != 0)
        return 1;
    if (!ld_path_is_derived_from(state.stage, journal,
                                 ".minix-stage")) {
        worker_error(error, error_size,
                     "Minix recovery stage is not derived from the selected journal path");
        journal_free(&state);
        return 1;
    }
    if (!valid_operation(state.operation) ||
        !valid_phase(state.phase)) {
        worker_error(error, error_size,
                     "Minix recovery journal contains an unsupported operation or phase");
        journal_free(&state);
        return 1;
    }

    const bool growth =
        strcmp(state.operation, "growth-defrag") == 0;
    int identity_result =
        strcmp(state.phase, "staged") == 0
            ? check_source_unchanged(device, &state,
                                     error, error_size)
            : check_target_identity(device, &state,
                                    error, error_size);
    if (identity_result == STOPPED) {
        ld_emit_result_event(
            stdout, "recover", "stopped",
            "Stopped before recovery source writes; artifacts remain intact.");
        journal_free(&state);
        return STOPPED;
    }
    if (identity_result != 0) {
        journal_free(&state);
        return 1;
    }

    char digest[65];
    int digest_result =
        stage_sha256(state.stage, &state, digest,
                     error, error_size);
    if (digest_result == STOPPED) {
        ld_emit_result_event(
            stdout, "recover", "stopped",
            "Stopped before recovery source writes; artifacts remain intact.");
        journal_free(&state);
        return STOPPED;
    }
    if (digest_result != 0 ||
        strcmp(digest, state.stage_sha256) != 0) {
        if (digest_result == 0)
            worker_error(
                error, error_size,
                "Minix recovery stage SHA-256 does not match the journal");
        journal_free(&state);
        return 1;
    }
    if (minix_verify_layout(state.stage, growth, 10U,
                            error, error_size) != 0) {
        journal_free(&state);
        return 1;
    }

    if (strcmp(state.phase, "committed") == 0) {
        if (verify_committed_source(&state, growth,
                                    error, error_size) != 0) {
            journal_free(&state);
            return 1;
        }
        transaction_cleanup(journal, &state);
        ld_emit_result_event(
            stdout, "recover", "completed",
            "Verified an already committed Minix transaction.");
        journal_free(&state);
        return 0;
    }

    if (journal_phase(journal, &state, "committing",
                      error, error_size) != 0) {
        journal_free(&state);
        return 1;
    }
    uint64_t written = 0U;
    const int commit_result =
        safe_commit_stage(&state, &written,
                          error, error_size);
    if (commit_result == STOPPED) {
        ld_emit_result_event(
            stdout, "recover", "stopped",
            "Recovery stopped at a durable boundary and can be resumed.");
        journal_free(&state);
        return STOPPED;
    }
    if (commit_result != 0 ||
        verify_committed_source(&state, growth,
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
        (void)printf(
            "@@LIVE_RESET {\"reason\":\"authoritative post-recovery Minix map\"}\n");
        (void)fflush(stdout);
    }
    (void)printf(
        "Recovered verified Minix source; committed %" PRIu64 " KiB.\n",
        written / 1024U);
    ld_emit_result_event(stdout, "recover", "completed", "");
    journal_free(&state);
    return 0;
}

static void print_identity(const MinixSummary *summary)
{
    (void)printf("{\"filesystem\":\"minix\",\"variant\":\"%s\","
                 "\"byte_order\":\"%s\",\"version\":%u}\n",
                 minix_variant_name(summary),
                 minix_byte_order_name(summary),
                 summary->version);
}

static void print_analysis(const MinixAnalysis *analysis)
{
    const MinixSummary *summary = &analysis->summary;
    (void)printf(
        "{\"filesystem\":\"minix\",\"variant\":\"%s\",\"byte_order\":\"%s\","
        "\"version\":%u,\"magic\":%u,\"inode_count\":%u,\"zone_count\":%u,"
        "\"imap_blocks\":%u,\"zmap_blocks\":%u,\"first_data_zone\":%u,"
        "\"log_zone_size\":%u,\"block_size\":%u,\"zone_size\":%llu,"
        "\"max_size\":%u,\"filesystem_bytes\":%llu,\"physical_bytes\":%llu,"
        "\"free_bytes\":%llu,\"used_bytes\":%llu,\"regular_files\":%llu,"
        "\"directories\":%llu,\"fragmented_files\":%llu,"
        "\"fragmented_directories\":%llu}\n",
        minix_variant_name(summary), minix_byte_order_name(summary),
        summary->version, (unsigned int)summary->magic,
        (unsigned int)summary->inode_count,
        (unsigned int)summary->zone_count,
        (unsigned int)summary->imap_blocks,
        (unsigned int)summary->zmap_blocks,
        (unsigned int)summary->first_data_zone,
        (unsigned int)summary->log_zone_size,
        (unsigned int)summary->block_size,
        (unsigned long long)summary->zone_size,
        (unsigned int)summary->max_size,
        (unsigned long long)analysis->filesystem_bytes,
        (unsigned long long)analysis->physical_bytes,
        (unsigned long long)
            (analysis->free_zones * summary->zone_size),
        (unsigned long long)
            (analysis->used_zones * summary->zone_size),
        (unsigned long long)analysis->regular_files,
        (unsigned long long)analysis->directories,
        (unsigned long long)analysis->fragmented_files,
        (unsigned long long)analysis->fragmented_directories);
}

static int print_map(const char *path, uint64_t requested_cells)
{
    MinixAnalysis analysis;
    char error[256];
    if (minix_analyse(path, &analysis, NULL, 0U,
                      error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s: %s\n", PROG, error);
        return 1;
    }
    uint64_t cell_count = requested_cells;
    if (cell_count > analysis.total_units)
        cell_count = analysis.total_units;
    if (cell_count == 0U)
        cell_count = 1U;

    size_t map_bytes = 0U;
    if (cell_count > SIZE_MAX ||
        !infiltratr_size_multiply_checked(
            (size_t)cell_count, sizeof(MinixMapCell),
            &map_bytes)) {
        (void)fprintf(stderr,
                      "%s: map cell count is too large\n", PROG);
        return 1;
    }
    (void)map_bytes;
    MinixMapCell *cells =
        calloc((size_t)cell_count, sizeof(*cells));
    if (cells == NULL) {
        (void)fprintf(stderr,
                      "%s: out of memory allocating map cells\n",
                      PROG);
        return 1;
    }
    if (minix_analyse(path, &analysis, cells, cell_count,
                      error, sizeof(error)) != 0) {
        free(cells);
        (void)fprintf(stderr, "%s: %s\n", PROG, error);
        return 1;
    }

    const MinixSummary *summary = &analysis.summary;
    const uint64_t outside_units =
        analysis.total_units > summary->zone_count
            ? analysis.total_units - summary->zone_count : 0U;
    (void)printf(
        "{\"schema\":1,\"backend\":\"read-only-domain\","
        "\"filesystem\":\"minix\",\"map_accuracy\":\"exact\","
        "\"unit_size\":%llu,\"total_units\":%llu,\"cell_count\":%llu,"
        "\"total_bytes\":%llu,\"filesystem_bytes\":%llu,"
        "\"free_bytes\":%llu,\"used_bytes\":%llu,\"unknown_bytes\":%llu,"
        "\"regular_files\":%llu,\"directories\":%llu,"
        "\"fragmented_files\":%llu,\"fragmented_directories\":%llu,"
        "\"cells\":[",
        (unsigned long long)summary->zone_size,
        (unsigned long long)analysis.total_units,
        (unsigned long long)cell_count,
        (unsigned long long)
            (analysis.total_units * summary->zone_size),
        (unsigned long long)analysis.filesystem_bytes,
        (unsigned long long)
            (analysis.free_zones * summary->zone_size),
        (unsigned long long)
            (analysis.used_zones * summary->zone_size),
        (unsigned long long)
            (outside_units * summary->zone_size),
        (unsigned long long)analysis.regular_files,
        (unsigned long long)analysis.directories,
        (unsigned long long)analysis.fragmented_files,
        (unsigned long long)analysis.fragmented_directories);

    for (uint64_t index = 0U; index < cell_count; ++index) {
        if (index != 0U)
            (void)putchar(',');
        (void)printf(
            "{\"start\":%llu,\"end\":%llu,\"free\":%llu,\"used\":%llu,"
            "\"unknown\":%llu,\"bad\":0,\"fragmented\":%llu,"
            "\"directory\":%llu}",
            (unsigned long long)cells[index].start,
            (unsigned long long)cells[index].end,
            (unsigned long long)cells[index].free_count,
            (unsigned long long)cells[index].used_count,
            (unsigned long long)cells[index].outside_count,
            (unsigned long long)cells[index].fragmented_count,
            (unsigned long long)cells[index].directory_count);
    }
    (void)printf(
        "],\"details\":{\"variant\":\"%s\",\"version\":%u,"
        "\"byte_order\":\"%s\","
        "\"allocation_basis\":\"inode and zone bitmaps\","
        "\"fragmentation_basis\":\"direct and indirect inode zone trees\"}}\n",
        minix_variant_name(summary), summary->version,
        minix_byte_order_name(summary));
    free(cells);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        (void)printf("%s %s\n", PROG, LD_VERSION);
        return 0;
    }

    if (argc == 5 && strcmp(argv[1], "map") == 0 &&
        strcmp(argv[3], "--cells") == 0) {
        uint64_t requested_cells = 0U;
        if (parse_cells(argv[4], &requested_cells) != 0) {
            (void)fprintf(stderr,
                          "%s: invalid cell count\n", PROG);
            return 2;
        }
        return print_map(argv[2], requested_cells);
    }

    if (argc < 3) {
        usage(stderr);
        return 2;
    }

    const char *mode = argv[1];
    const char *device = argv[2];

    if (strcmp(mode, "identify") == 0) {
        if (argc != 3) {
            usage(stderr);
            return 2;
        }
        MinixSummary summary;
        char error[256];
        if (minix_read_summary(device, &summary,
                               error, sizeof(error)) != 0) {
            (void)fprintf(stderr,
                          "%s: %s\n", PROG, error);
            return 1;
        }
        print_identity(&summary);
        return 0;
    }

    if (strcmp(mode, "analyse-json") == 0) {
        if (argc != 3) {
            usage(stderr);
            return 2;
        }
        MinixAnalysis analysis;
        char error[256];
        if (minix_analyse(device, &analysis, NULL, 0U,
                          error, sizeof(error)) != 0) {
            (void)fprintf(stderr,
                          "%s: %s\n", PROG, error);
            return 1;
        }
        print_analysis(&analysis);
        return 0;
    }

    const bool growth =
        strcmp(mode, "growth-defrag") == 0;
    const bool defrag =
        strcmp(mode, "defrag") == 0;
    const bool recover =
        strcmp(mode, "recover") == 0;
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
        } else if (strcmp(argv[index],
                          "--live-updates") == 0) {
            live = true;
        } else if (strcmp(argv[index], "--confirm") == 0 &&
                   index + 1 < argc) {
            confirm = argv[++index];
        } else if (strcmp(argv[index], "--journal") == 0 &&
                   index + 1 < argc) {
            journal = argv[++index];
        } else if (strcmp(argv[index],
                          "--growth-percent") == 0 &&
                   index + 1 < argc) {
            if (!parse_unsigned(argv[++index],
                                &growth_percent)) {
                (void)fprintf(stderr,
                              "%s: invalid --growth-percent\n",
                              PROG);
                return 2;
            }
        } else if ((strcmp(argv[index], "--workers") == 0 ||
                    strcmp(argv[index], "--ram-buffer") == 0 ||
                    strcmp(argv[index], "--batch-clusters") == 0 ||
                    strcmp(argv[index], "--live-map-cells") == 0) &&
                   index + 1 < argc) {
            ++index;
        } else {
            (void)fprintf(stderr,
                          "%s: unknown or incomplete option: %s\n",
                          PROG, argv[index]);
            return 2;
        }
    }

    if (!write || journal == NULL || confirm == NULL ||
        strcmp(confirm, device) != 0) {
        (void)fprintf(
            stderr,
            "%s: Minix mutation requires --write --confirm DEVICE --journal PATH\n",
            PROG);
        return 2;
    }
    if (growth && growth_percent != 10U) {
        (void)fprintf(
            stderr,
            "%s: Minix Growth Defrag requires exactly 10 percent reserve\n",
            PROG);
        return 2;
    }
    if (ld_path_is_mounted(device)) {
        (void)fprintf(
            stderr,
            "%s: Minix target is mounted; raw mutation and recovery require an unmounted filesystem\n",
            PROG);
        return 1;
    }

    ld_stop_clear();
    ld_stop_install_handlers();
    ld_stop_report_ready();

    char error[256] = {0};
    if (recover) {
        const int result =
            handle_recovery(device, journal, live,
                            error, sizeof(error));
        if (result != 0 && result != STOPPED)
            (void)fprintf(stderr, "%s: %s\n", PROG,
                          error[0] == '\0'
                              ? "Minix recovery failed" : error);
        return result;
    }

    if (minix_verify_layout(device, growth, growth_percent,
                            error, sizeof(error)) == 0) {
        ld_emit_result_event(
            stdout, mode, "not-needed",
            growth
                ? "Canonical Minix layout with exact 10% post-file reserve already verified."
                : "Canonical packed Minix layout already verified.");
        return 0;
    }
    error[0] = '\0';

    MinixJournal state;
    memset(&state, 0, sizeof(state));
    infiltratr_copy_string(state.operation,
                            sizeof(state.operation), mode);
    int capture_result =
        capture_target(device, &state, error, sizeof(error));
    if (capture_result == STOPPED) {
        ld_emit_result_event(
            stdout, mode, "stopped",
            "Stopped during read-only Minix preflight.");
        journal_free(&state);
        return STOPPED;
    }
    if (capture_result != 0)
        goto fail;

    state.stage =
        ld_path_append_suffix(journal, ".minix-stage");
    if (state.stage == NULL) {
        worker_error(error, sizeof(error),
                     "out of memory creating Minix stage path");
        goto fail;
    }
    if (access(journal, F_OK) == 0 ||
        access(state.stage, F_OK) == 0) {
        worker_error(
            error, sizeof(error),
            "existing Minix recovery artifacts must be recovered or removed before starting a new transaction");
        goto fail;
    }

    char *parent = ld_path_parent_directory(journal);
    if (parent == NULL ||
        ld_path_ensure_trusted_directory_tree(parent) != 0) {
        worker_error(error, sizeof(error),
                     "cannot create Minix recovery directory: %s",
                     strerror(errno));
        free(parent);
        goto fail;
    }
    free(parent);

    (void)printf("Starting native C Minix %s on %s.\n",
                 growth ? "Growth Defrag" : "Defrag",
                 device);
    uint64_t planned = 0U;
    if (minix_build_stage(
            state.device, state.stage, growth, growth_percent,
            &planned, error, sizeof(error)) != 0 ||
        minix_verify_layout(
            state.stage, growth, growth_percent,
            error, sizeof(error)) != 0) {
        unlink_if_exists(state.stage);
        goto fail;
    }
    if (ld_stop_requested()) {
        unlink_if_exists(state.stage);
        ld_emit_result_event(
            stdout, mode, "stopped",
            "Stopped before any Minix source writes.");
        journal_free(&state);
        return STOPPED;
    }

    int source_result =
        check_source_unchanged(device, &state,
                               error, sizeof(error));
    if (source_result == STOPPED) {
        unlink_if_exists(state.stage);
        ld_emit_result_event(
            stdout, mode, "stopped",
            "Stopped before any Minix source writes.");
        journal_free(&state);
        return STOPPED;
    }
    if (source_result != 0) {
        unlink_if_exists(state.stage);
        goto fail;
    }

    int hash_result =
        stage_sha256(state.stage, &state,
                     state.stage_sha256,
                     error, sizeof(error));
    if (hash_result == STOPPED) {
        unlink_if_exists(state.stage);
        ld_emit_result_event(
            stdout, mode, "stopped",
            "Stopped before any Minix source writes.");
        journal_free(&state);
        return STOPPED;
    }
    if (hash_result != 0) {
        unlink_if_exists(state.stage);
        goto fail;
    }

    infiltratr_copy_string(state.phase,
                            sizeof(state.phase), "staged");
    if (journal_save(journal, &state,
                     error, sizeof(error)) != 0) {
        unlink_if_exists(state.stage);
        goto fail;
    }
    if (ld_stop_requested()) {
        transaction_cleanup(journal, &state);
        ld_emit_result_event(
            stdout, mode, "stopped",
            "Stopped before any Minix source writes.");
        journal_free(&state);
        return STOPPED;
    }
    if (journal_phase(journal, &state, "committing",
                      error, sizeof(error)) != 0)
        goto fail;

    (void)printf(
        "Minix source commit: writing %" PRIu64
        " KiB from the verified full-filesystem stage.\n",
        planned / 1024U);
    (void)fflush(stdout);

    uint64_t written = 0U;
    const int commit_result =
        safe_commit_stage(&state, &written,
                          error, sizeof(error));
    if (commit_result == STOPPED) {
        (void)printf(
            "Minix Stop reached a durable source-write boundary; recovery journal and verified stage were retained.\n");
        ld_emit_result_event(
            stdout, mode, "stopped",
            "Run Recover to resume the verified Minix transaction.");
        journal_free(&state);
        return STOPPED;
    }
    if (commit_result != 0 ||
        verify_committed_source(&state, growth,
                                error, sizeof(error)) != 0)
        goto fail;
    if (journal_phase(journal, &state, "committed",
                      error, sizeof(error)) != 0)
        goto fail;

    transaction_cleanup(journal, &state);
    if (live) {
        (void)printf(
            "@@LIVE_RESET {\"reason\":\"authoritative post-commit Minix map\"}\n");
        (void)fflush(stdout);
    }
    (void)printf(
        "Minix %s completed; committed %" PRIu64 " KiB.\n",
        growth ? "Growth Defrag" : "Defrag",
        written / 1024U);
    ld_emit_result_event(stdout, mode, "completed", "");
    journal_free(&state);
    return 0;

fail:
    (void)fprintf(stderr, "%s: %s\n", PROG,
                  error[0] == '\0'
                      ? "Minix transaction failed" : error);
    journal_free(&state);
    return 1;
}
