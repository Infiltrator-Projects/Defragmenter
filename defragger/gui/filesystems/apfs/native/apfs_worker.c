// SPDX-License-Identifier: GPL-3.0-or-later
#include "apfs_native.h"
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define PROG "linux-defragger-apfs-worker"

static void usage(FILE *stream)
{
    fprintf(stream,
            "Usage: %s --version | identify DEVICE | analyse-json DEVICE | "
            "map DEVICE --cells COUNT | "
            "defrag|growth-defrag|recover DEVICE --write --confirm DEVICE "
            "--journal PATH [--growth-percent 10] [--live-updates]\n", PROG);
}

static void print_uuid(const uint8_t uuid[16])
{
    for (size_t index = 0U; index < 16U; ++index)
        printf("%02x", (unsigned int)uuid[index]);
}

static uint64_t overlap(const ApfsRange *ranges, size_t count,
                        uint64_t start, uint64_t end)
{
    uint64_t total = 0U;
    for (size_t index = 0U; index < count; ++index) {
        if (ranges[index].end <= start)
            continue;
        if (ranges[index].start >= end)
            break;
        const uint64_t left =
            ranges[index].start > start ? ranges[index].start : start;
        const uint64_t right =
            ranges[index].end < end ? ranges[index].end : end;
        if (right > left)
            total += right - left;
    }
    return total;
}

static void print_analysis(const ApfsAnalysis *analysis)
{
    const double percent = infiltratr_percent_u64(
        analysis->fragmented_files, analysis->regular_files);
    printf("{\"filesystem\":\"apfs\",\"block_size\":%u,\"block_count\":%" PRIu64
           ",\"container_uuid\":\"",
           analysis->block_size, analysis->block_count);
    print_uuid(analysis->container_uuid);
    printf("\",\"xid\":%" PRIu64 ",\"active_nx_block\":%" PRIu64
           ",\"spaceman_block\":%" PRIu64 ",\"volume_oid\":%" PRIu64
           ",\"volume_super_block\":%" PRIu64 ",\"free_blocks\":%" PRIu64
           ",\"used_blocks\":%" PRIu64 ",\"regular_files\":%" PRIu64
           ",\"directories\":%" PRIu64 ",\"fragmented_files\":%" PRIu64
           ",\"fragmented_directories\":0,\"fragmentation_percent\":%.6f}\n",
           analysis->xid, analysis->active_nx_block, analysis->spaceman_block,
           analysis->volume_oid, analysis->volume_super_block,
           analysis->free_blocks, analysis->used_blocks,
           analysis->regular_files, analysis->directories,
           analysis->fragmented_files, percent);
}

static int print_map(const ApfsAnalysis *analysis, uint64_t requested_cells)
{
    uint64_t cells = requested_cells;
    if (cells > analysis->block_count)
        cells = analysis->block_count;
    if (cells == 0U)
        cells = 1U;
    uint64_t free_total = 0U;
    uint64_t used_total = 0U;
    printf("{\"schema\":1,\"backend\":\"read-only-domain\",\"filesystem\":\"apfs\","
           "\"map_accuracy\":\"exact-bounded-spaceman\",\"unit_size\":%u,"
           "\"total_units\":%" PRIu64 ",\"cell_count\":%" PRIu64
           ",\"total_bytes\":%" PRIu64 ",\"cells\":[",
           analysis->block_size, analysis->block_count, cells,
           analysis->block_count * (uint64_t)analysis->block_size);
    for (uint64_t index = 0U; index < cells; ++index) {
        const uint64_t start = index * analysis->block_count / cells;
        uint64_t end = (index + 1U) * analysis->block_count / cells;
        if (end <= start)
            end = start + 1U;
        const uint64_t used = overlap(
            analysis->used_ranges, analysis->used_range_count, start, end);
        const uint64_t fragmented = overlap(
            analysis->fragmented_ranges, analysis->fragmented_range_count,
            start, end);
        const uint64_t free_count = (end - start) - used;
        used_total += used;
        free_total += free_count;
        if (index != 0U)
            putchar(',');
        printf("{\"start\":%" PRIu64 ",\"end\":%" PRIu64
               ",\"free\":%" PRIu64 ",\"used\":%" PRIu64
               ",\"unknown\":0,\"bad\":0,\"fragmented\":%" PRIu64
               ",\"directory\":0,\"outside\":0}",
               start, end - 1U, free_count, used, fragmented);
    }
    const double percent = infiltratr_percent_u64(
        analysis->fragmented_files, analysis->regular_files);
    printf("],\"free_bytes\":%" PRIu64 ",\"used_bytes\":%" PRIu64
           ",\"unknown_bytes\":0,\"details\":{\"container_uuid\":\"",
           free_total * analysis->block_size, used_total * analysis->block_size);
    print_uuid(analysis->container_uuid);
    printf("\",\"xid\":%" PRIu64 ",\"active_nx_block\":%" PRIu64
           ",\"spaceman_block\":%" PRIu64
           ",\"fragmentation_available\":true,"
           "\"fragmentation_basis\":\"native APFS catalog file-extent records\","
           "\"allocation_basis\":\"active-checkpoint spaceman chunk bitmaps\"},"
           "\"regular_files\":%" PRIu64 ",\"directories\":%" PRIu64
           ",\"fragmented_files\":%" PRIu64
           ",\"fragmented_directories\":0,\"fragmentation_percent\":%.6f}\n",
           analysis->xid, analysis->active_nx_block, analysis->spaceman_block,
           analysis->regular_files, analysis->directories,
           analysis->fragmented_files, percent);
    return 0;
}

#define APFS_JOURNAL_MAGIC "LINUX-DEFRAGGER-APFS-JOURNAL-1"
#define APFS_STAGE_SUFFIX ".apfs-stage"
#define APFS_TXN_IO_BYTES (1024U * 1024U)
#define APFS_TXN_STOPPED 130

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
    uint64_t xid;
    uint32_t block_size;
} APFSJournal;

static void txn_error(char *error, size_t error_size, const char *format, ...)
{
    if (error == NULL || error_size == 0U) return;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(error, error_size, format, args);
    va_end(args);
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
    if (path == NULL || *path == '\0') return;
    const int failure = infiltratr_unlink_durable(path, true);
    if (failure != 0)
        (void)fprintf(stderr, "%s: warning: cannot durably remove %s: %s\n",
                      PROG, path, strerror(failure));
}

static void journal_free(APFSJournal *state)
{
    free(state->device);
    free(state->target_identity);
    free(state->stage);
    memset(state, 0, sizeof(*state));
}

static bool journal_write_stream(FILE *file, const void *user_data)
{
    const APFSJournal *state = user_data;
    (void)fprintf(file, "%s\n", APFS_JOURNAL_MAGIC);
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
    (void)fprintf(file, "xid=%" PRIu64 "\n", state->xid);
    (void)fprintf(file, "block_size=%u\n", state->block_size);
    return !ferror(file);
}

static int journal_save(const char *path, const APFSJournal *state,
                        char *error, size_t error_size)
{
    if (!safe_value(state->device) || !safe_value(state->target_identity) ||
        !safe_value(state->stage)) {
        txn_error(error, error_size,
                  "APFS transaction paths contain unsupported journal characters");
        return -1;
    }
    char *parent = ld_path_parent_directory(path);
    if (parent == NULL || ld_path_ensure_trusted_directory_tree(parent) != 0) {
        txn_error(error, error_size,
                  "cannot create APFS journal directory: %s", strerror(errno));
        free(parent);
        return -1;
    }
    free(parent);
    const int failure = infiltratr_atomic_file_write(
        path, INFILTRATR_ATOMIC_FILE_PRIVATE, journal_write_stream, state);
    if (failure != 0) {
        txn_error(error, error_size,
                  "cannot publish APFS recovery journal: %s", strerror(failure));
        return -1;
    }
    return 0;
}

static int parse_u64_value(const char *text, uint64_t *value)
{
    return infiltratr_parse_u64(text, 10U, value) ? 0 : -1;
}

static int journal_load(const char *path, APFSJournal *state,
                        char *error, size_t error_size)
{
    memset(state, 0, sizeof(*state));
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        txn_error(error, error_size,
                  "cannot open APFS recovery journal: %s", strerror(errno));
        return -1;
    }
    char *line = NULL;
    size_t capacity = 0U;
    if (getline(&line, &capacity, file) < 0) goto invalid;
    infiltratr_trim_line_end(line);
    if (strcmp(line, APFS_JOURNAL_MAGIC) != 0) goto invalid;
    while (getline(&line, &capacity, file) >= 0) {
        char *key = NULL;
        char *value = NULL;
        if (infiltratr_config_parse_line(line, &key, &value) !=
            INFILTRATR_CONFIG_LINE_ENTRY) goto invalid;
        if (strcmp(key, "device") == 0) {
            free(state->device); state->device = ld_xstrdup(value);
        } else if (strcmp(key, "target_identity") == 0) {
            free(state->target_identity); state->target_identity = ld_xstrdup(value);
        } else if (strcmp(key, "stage") == 0) {
            free(state->stage); state->stage = ld_xstrdup(value);
        } else if (strcmp(key, "operation") == 0) {
            infiltratr_copy_string(state->operation, sizeof(state->operation), value);
        } else if (strcmp(key, "phase") == 0) {
            infiltratr_copy_string(state->phase, sizeof(state->phase), value);
        } else if (strcmp(key, "volume_token") == 0) {
            infiltratr_copy_string(state->volume_token, sizeof(state->volume_token), value);
        } else if (strcmp(key, "source_sha256") == 0) {
            infiltratr_copy_string(state->source_sha256, sizeof(state->source_sha256), value);
        } else if (strcmp(key, "stage_sha256") == 0) {
            infiltratr_copy_string(state->stage_sha256, sizeof(state->stage_sha256), value);
        } else if (strcmp(key, "physical_bytes") == 0) {
            if (parse_u64_value(value, &state->physical_bytes) != 0) goto invalid;
        } else if (strcmp(key, "filesystem_bytes") == 0) {
            if (parse_u64_value(value, &state->filesystem_bytes) != 0) goto invalid;
        } else if (strcmp(key, "xid") == 0) {
            if (parse_u64_value(value, &state->xid) != 0) goto invalid;
        } else if (strcmp(key, "block_size") == 0) {
            uint64_t parsed = 0U;
            if (parse_u64_value(value, &parsed) != 0 || parsed > UINT32_MAX)
                goto invalid;
            state->block_size = (uint32_t)parsed;
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
        state->block_size == 0U)
        goto invalid_state;
    return 0;
invalid:
    free(line);
    (void)fclose(file);
invalid_state:
    journal_free(state);
    txn_error(error, error_size,
              "APFS recovery journal is malformed or incomplete");
    return -1;
}

static int journal_phase(const char *path, APFSJournal *state,
                         const char *phase, char *error, size_t error_size)
{
    infiltratr_copy_string(state->phase, sizeof(state->phase), phase);
    return journal_save(path, state, error, error_size);
}

static char *stage_name(const char *journal)
{
    return ld_path_append_suffix(journal, APFS_STAGE_SUFFIX);
}

static void transaction_cleanup(const char *journal,
                                const APFSJournal *state)
{
    if (state != NULL) unlink_if_exists(state->stage);
    unlink_if_exists(journal);
}

static char *canonical_path(const char *path, char *error, size_t error_size)
{
    char *resolved = realpath(path, NULL);
    if (resolved == NULL)
        txn_error(error, error_size,
                  "cannot resolve APFS target %s: %s", path, strerror(errno));
    return resolved;
}

static int target_identity(const char *path, char **identity, uint64_t *size,
                           char *error, size_t error_size)
{
    struct stat status;
    if (stat(path, &status) != 0) {
        txn_error(error, error_size,
                  "cannot stat APFS target: %s", strerror(errno));
        return -1;
    }
    if (!S_ISREG(status.st_mode) && !S_ISBLK(status.st_mode)) {
        txn_error(error, error_size,
                  "APFS target is not a block device or regular image");
        return -1;
    }
    char text[160];
    if (S_ISBLK(status.st_mode)) {
        (void)snprintf(text, sizeof(text), "block:%u:%u",
                       major(status.st_rdev), minor(status.st_rdev));
        LdDevice target = ld_device_open(path, false);
        *size = target.size_bytes;
        ld_device_close(&target);
    } else {
        (void)snprintf(text, sizeof(text), "file:%llu:%llu",
                       (unsigned long long)status.st_dev,
                       (unsigned long long)status.st_ino);
        *size = (uint64_t)status.st_size;
    }
    if (*size == 0U) {
        txn_error(error, error_size, "cannot determine APFS target size");
        return -1;
    }
    *identity = ld_xstrdup(text);
    return 0;
}

static int digest_final(EVP_MD_CTX *context, char output[65],
                        char *error, size_t error_size)
{
    unsigned char digest[32];
    unsigned int length = 0U;
    if (EVP_DigestFinal_ex(context, digest, &length) != 1 ||
        length != sizeof(digest)) {
        txn_error(error, error_size, "finalising APFS SHA-256 failed");
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
        txn_error(error, error_size,
                  "cannot open APFS bytes for SHA-256: %s", strerror(errno));
        return -1;
    }
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    uint8_t *buffer = malloc(APFS_TXN_IO_BYTES);
    if (context == NULL || buffer == NULL ||
        EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(context); free(buffer); (void)close(fd);
        txn_error(error, error_size, "cannot initialise APFS SHA-256");
        return -1;
    }
    int rc = 0;
    for (uint64_t offset = 0U; offset < bytes;) {
        if (stoppable && ld_stop_requested()) {
            rc = APFS_TXN_STOPPED;
            break;
        }
        const uint64_t remaining = bytes - offset;
        const size_t count = remaining > APFS_TXN_IO_BYTES
                           ? APFS_TXN_IO_BYTES : (size_t)remaining;
        if (ld_pread_full(fd, buffer, count, offset) != (ssize_t)count ||
            EVP_DigestUpdate(context, buffer, count) != 1) {
            txn_error(error, error_size, "hashing APFS bytes failed");
            rc = -1;
            break;
        }
        offset += count;
    }
    if (rc == 0) rc = digest_final(context, output, error, error_size);
    EVP_MD_CTX_free(context); free(buffer); (void)close(fd);
    return rc;
}

static int primary_super_token(const char *path, char output[65],
                               char *error, size_t error_size)
{
    return hash_prefix(path, 4096U, false,
                       output, error, error_size);
}

static int capture_target(const char *device, APFSJournal *state,
                          char *error, size_t error_size)
{
    state->device = canonical_path(device, error, error_size);
    if (state->device == NULL) return -1;
    if (target_identity(state->device, &state->target_identity,
                        &state->physical_bytes, error, error_size) != 0)
        return -1;
    APFSAnalysis analysis;
    if (apfs_analyse(state->device, &analysis, error, error_size) != 0)
        return -1;
    if (analysis.block_count >
        UINT64_MAX / analysis.block_size) {
        apfs_analysis_free(&analysis);
        txn_error(error, error_size,
                  "APFS filesystem byte count overflows");
        return -1;
    }
    state->filesystem_bytes =
        analysis.block_count * (uint64_t)analysis.block_size;
    state->xid = analysis.xid;
    state->block_size = analysis.block_size;
    apfs_analysis_free(&analysis);
    if (state->filesystem_bytes > state->physical_bytes ||
        primary_super_token(state->device, state->volume_token,
                            error, error_size) != 0)
        return -1;
    return hash_prefix(state->device, state->filesystem_bytes, true,
                       state->source_sha256, error, error_size);
}

static int check_target_identity(const char *device, const APFSJournal *state,
                                 char *error, size_t error_size)
{
    char *canonical = canonical_path(device, error, error_size);
    if (canonical == NULL) return -1;
    char *identity = NULL;
    uint64_t size = 0U;
    int rc = target_identity(canonical, &identity, &size, error, error_size);
    if (rc == 0 &&
        (strcmp(canonical, state->device) != 0 ||
         strcmp(identity, state->target_identity) != 0 ||
         size != state->physical_bytes)) {
        txn_error(error, error_size,
                  "APFS target path, identity or capacity changed before commit");
        rc = -1;
    }
    free(identity); free(canonical);
    return rc;
}

static int check_source_unchanged(const char *device,
                                  const APFSJournal *state,
                                  char *error, size_t error_size)
{
    if (check_target_identity(device, state, error, error_size) != 0)
        return -1;
    char token[65];
    char digest[65];
    int rc = primary_super_token(state->device, token, error, error_size);
    if (rc == 0)
        rc = hash_prefix(state->device, state->filesystem_bytes, true,
                         digest, error, error_size);
    if (rc != 0) return rc;
    if (strcmp(token, state->volume_token) != 0 ||
        strcmp(digest, state->source_sha256) != 0) {
        txn_error(error, error_size,
                  "APFS source changed after preflight; refusing source writes");
        return -1;
    }
    return 0;
}

static int safe_commit_stage(const char *stage_path, const char *target_path,
                             const APFSJournal *state, uint64_t *written,
                             char *error, size_t error_size)
{
    int stage = open(stage_path, O_RDONLY | O_CLOEXEC);
    int target = ld_device_open_verified_fd(target_path, true,
                                            state->target_identity,
                                            state->physical_bytes);
    if (stage < 0 || target < 0) {
        if (stage >= 0) (void)close(stage);
        if (target >= 0) (void)close(target);
        txn_error(error, error_size,
                  "cannot open APFS stage or target for commit: %s",
                  strerror(errno));
        return -1;
    }
    if (flock(target, LOCK_EX | LOCK_NB) != 0) {
        txn_error(error, error_size,
                  "cannot lock APFS target for commit: %s", strerror(errno));
        (void)close(stage); (void)close(target);
        return -1;
    }
    uint8_t *buffer = malloc(APFS_TXN_IO_BYTES);
    if (buffer == NULL) {
        txn_error(error, error_size, "out of memory committing APFS stage");
        (void)flock(target, LOCK_UN); (void)close(stage); (void)close(target);
        return -1;
    }
    int rc = 0;
    uint64_t total = 0U;
    for (uint64_t offset = 0U; offset < state->filesystem_bytes;) {
        if (ld_stop_requested()) {
            rc = fsync(target) == 0 ? APFS_TXN_STOPPED : -1;
            if (rc < 0)
                txn_error(error, error_size,
                          "cannot sync APFS target at Stop boundary: %s",
                          strerror(errno));
            break;
        }
        const uint64_t remaining = state->filesystem_bytes - offset;
        const size_t count = remaining > APFS_TXN_IO_BYTES
                           ? APFS_TXN_IO_BYTES : (size_t)remaining;
        if (ld_pread_full(stage, buffer, count, offset) != (ssize_t)count ||
            ld_pwrite_full(target, buffer, count, offset) != (ssize_t)count) {
            txn_error(error, error_size,
                      "short I/O committing APFS bytes at offset %" PRIu64,
                      offset);
            rc = -1;
            break;
        }
        offset += count; total += count;
    }
    if (rc == 0 && fsync(target) != 0) {
        txn_error(error, error_size,
                  "cannot sync APFS target: %s", strerror(errno));
        rc = -1;
    }
    free(buffer); (void)flock(target, LOCK_UN);
    (void)close(stage); (void)close(target);
    if (written != NULL) *written = total;
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

static int recover_transaction(const char *device, const char *journal,
                               bool live, char *error, size_t error_size)
{
    APFSJournal state;
    if (journal_load(journal, &state, error, error_size) != 0)
        return 1;
    if (!ld_path_is_derived_from(state.stage, journal, APFS_STAGE_SUFFIX) ||
        !valid_operation(state.operation) || !valid_phase(state.phase)) {
        txn_error(error, error_size,
                  "APFS recovery journal has an invalid stage binding, operation or phase");
        journal_free(&state);
        return 1;
    }
    const int identity_rc = strcmp(state.phase, "staged") == 0
        ? check_source_unchanged(device, &state, error, error_size)
        : check_target_identity(device, &state, error, error_size);
    if (identity_rc != 0) {
        journal_free(&state);
        return identity_rc == APFS_TXN_STOPPED ? APFS_TXN_STOPPED : 1;
    }
    char digest[65];
    const int hash_rc = hash_prefix(state.stage, state.filesystem_bytes, true,
                                    digest, error, error_size);
    if (hash_rc != 0 || strcmp(digest, state.stage_sha256) != 0) {
        if (hash_rc == 0)
            txn_error(error, error_size,
                      "APFS recovery stage SHA-256 does not match the journal");
        journal_free(&state);
        return hash_rc == APFS_TXN_STOPPED ? APFS_TXN_STOPPED : 1;
    }
    const bool growth = strcmp(state.operation, "growth-defrag") == 0;
    if (apfs_verify_layout(state.stage, growth, 10U,
                            error, error_size) != 0) {
        journal_free(&state);
        return 1;
    }
    if (strcmp(state.phase, "committed") == 0) {
        if (apfs_verify_layout(device, growth, 10U,
                                error, error_size) != 0) {
            journal_free(&state);
            return 1;
        }
        transaction_cleanup(journal, &state);
        ld_emit_result_event(stdout, "recover", "completed",
                             "Verified an already committed APFS transaction.");
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
    if (commit_rc == APFS_TXN_STOPPED) {
        ld_emit_result_event(stdout, "recover", "stopped",
                             "Recovery stopped at a durable boundary and can be resumed.");
        journal_free(&state);
        return APFS_TXN_STOPPED;
    }
    if (commit_rc != 0 ||
        apfs_verify_layout(state.device, growth, 10U,
                            error, error_size) != 0 ||
        journal_phase(journal, &state, "committed",
                      error, error_size) != 0) {
        journal_free(&state);
        return 1;
    }
    transaction_cleanup(journal, &state);
    if (live) {
        (void)printf(
            "@@LIVE_RESET {\"reason\":\"authoritative post-recovery APFS map\"}\n");
        (void)fflush(stdout);
    }
    (void)printf("Recovered verified APFS source; committed %" PRIu64 " KiB.\n",
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
    if (argc == 3 && strcmp(argv[1], "identify") == 0) {
        APFSSummary summary;
        char error[256] = {0};
        if (apfs_read_summary(argv[2], &summary,
                              error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "%s: %s\n", PROG, error);
            return 1;
        }
        (void)printf(
            "{\"filesystem\":\"apfs\",\"block_size\":%u,"
            "\"block_count\":%" PRIu64 "}\n",
            summary.block_size, summary.block_count);
        return 0;
    }
    if (argc == 5 && strcmp(argv[1], "map") == 0 &&
        strcmp(argv[3], "--cells") == 0) {
        uint64_t cells = 0U;
        if (!infiltratr_parse_u64_range(
                argv[4], 10U, 1U, UINT64_MAX, &cells)) {
            (void)fprintf(stderr, "%s: invalid cell count\n", PROG);
            return 2;
        }
        APFSAnalysis analysis;
        char error[512] = {0};
        if (apfs_analyse(argv[2], &analysis,
                         error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "%s: %s\n", PROG,
                          error[0] != '\0' ? error
                                           : "APFS analysis failed");
            return 1;
        }
        const int rc = print_map(&analysis, cells);
        apfs_analysis_free(&analysis);
        return rc;
    }
    if (argc == 3 && strcmp(argv[1], "analyse-json") == 0) {
        APFSAnalysis analysis;
        char error[512] = {0};
        if (apfs_analyse(argv[2], &analysis,
                         error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "%s: %s\n", PROG,
                          error[0] != '\0' ? error
                                           : "APFS analysis failed");
            return 1;
        }
        print_analysis(&analysis);
        apfs_analysis_free(&analysis);
        return 0;
    }
    if (argc < 3) {
        usage(stderr);
        return 2;
    }

    const char *mode = argv[1];
    const char *device = argv[2];
    const bool growth =
        strcmp(mode, "growth-defrag") == 0;
    const bool recover =
        strcmp(mode, "recover") == 0;
    if (strcmp(mode, "defrag") != 0 &&
        !growth && !recover) {
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
                          "%s: unknown or incomplete APFS option: %s\n",
                          PROG, argv[index]);
            return 2;
        }
    }
    if (!write || confirm == NULL || journal == NULL ||
        strcmp(confirm, device) != 0) {
        (void)fprintf(
            stderr,
            "%s: APFS mutation requires --write --confirm DEVICE --journal PATH\n",
            PROG);
        return 2;
    }
    if (growth && growth_percent != 10U) {
        (void)fprintf(
            stderr,
            "%s: APFS Growth Defrag requires exactly 10 percent reserve\n",
            PROG);
        return 2;
    }
    if (ld_path_is_mounted(device)) {
        (void)fprintf(
            stderr,
            "%s: APFS target is mounted; raw mutation and recovery require an unmounted filesystem\n",
            PROG);
        return 1;
    }

    ld_stop_clear();
    ld_stop_install_handlers();
    char error[512] = {0};
    if (recover) {
        const int rc =
            recover_transaction(device, journal, live,
                                error, sizeof(error));
        if (rc != 0 && rc != APFS_TXN_STOPPED)
            (void)fprintf(stderr, "%s: %s\n", PROG,
                          error[0] != '\0'
                              ? error : "APFS recovery failed");
        return rc;
    }

    if (apfs_verify_layout(device, growth,
                           growth_percent,
                           error, sizeof(error)) == 0) {
        ld_emit_result_event(
            stdout, mode, "not-needed",
            growth
                ? "Canonical APFS layout with exact 10% post-file reserve already verified."
                : "Canonical APFS packed layout already verified.");
        return 0;
    }
    error[0] = '\0';

    APFSJournal state;
    memset(&state, 0, sizeof(state));
    infiltratr_copy_string(state.operation,
                            sizeof(state.operation), mode);
    const int capture_rc =
        capture_target(device, &state,
                       error, sizeof(error));
    if (capture_rc != 0) {
        if (capture_rc == APFS_TXN_STOPPED)
            ld_emit_result_event(
                stdout, mode, "stopped",
                "Stopped during read-only APFS preflight.");
        goto fail;
    }
    state.stage = stage_name(journal);
    if (state.stage == NULL) {
        txn_error(error, sizeof(error),
                  "out of memory creating APFS stage path");
        goto fail;
    }
    if (access(journal, F_OK) == 0 ||
        access(state.stage, F_OK) == 0) {
        txn_error(
            error, sizeof(error),
            "existing APFS recovery artifacts must be recovered or removed before starting");
        goto fail;
    }

    (void)printf("Starting native C APFS %s on %s.\n",
                 growth ? "Growth Defrag" : "Defrag",
                 device);
    uint64_t planned = 0U;
    const int stage_rc =
        apfs_build_stage(
            state.device, state.stage, growth,
            growth_percent, live, &planned,
            error, sizeof(error));
    if (stage_rc != 0 ||
        apfs_verify_layout(
            state.stage, growth, growth_percent,
            error, sizeof(error)) != 0) {
        unlink_if_exists(state.stage);
        if (stage_rc == APFS_TXN_STOPPED) {
            ld_emit_result_event(
                stdout, mode, "stopped",
                "Stopped before any APFS source writes.");
            journal_free(&state);
            return APFS_TXN_STOPPED;
        }
        goto fail;
    }

    const int unchanged =
        check_source_unchanged(
            state.device, &state,
            error, sizeof(error));
    if (unchanged != 0) {
        unlink_if_exists(state.stage);
        if (unchanged == APFS_TXN_STOPPED) {
            ld_emit_result_event(
                stdout, mode, "stopped",
                "Stopped before any APFS source writes.");
            journal_free(&state);
            return APFS_TXN_STOPPED;
        }
        goto fail;
    }
    const int hash_rc =
        hash_prefix(state.stage,
                    state.filesystem_bytes, true,
                    state.stage_sha256,
                    error, sizeof(error));
    if (hash_rc != 0) {
        unlink_if_exists(state.stage);
        if (hash_rc == APFS_TXN_STOPPED) {
            ld_emit_result_event(
                stdout, mode, "stopped",
                "Stopped before any APFS source writes.");
            journal_free(&state);
            return APFS_TXN_STOPPED;
        }
        goto fail;
    }

    infiltratr_copy_string(
        state.phase, sizeof(state.phase), "staged");
    if (journal_save(journal, &state,
                     error, sizeof(error)) != 0)
        goto fail;
    if (journal_phase(journal, &state, "committing",
                      error, sizeof(error)) != 0)
        goto fail;

    (void)printf(
        "APFS source commit: replaying %" PRIu64
        " KiB from the verified transaction stage.\n",
        planned / 1024U);
    (void)fflush(stdout);

    uint64_t written = 0U;
    const int commit_rc =
        safe_commit_stage(
            state.stage, state.device, &state,
            &written, error, sizeof(error));
    if (commit_rc == APFS_TXN_STOPPED) {
        ld_emit_result_event(
            stdout, mode, "stopped",
            "Run Recover to resume the verified APFS transaction.");
        journal_free(&state);
        return APFS_TXN_STOPPED;
    }
    if (commit_rc != 0 ||
        apfs_verify_layout(
            state.device, growth, growth_percent,
            error, sizeof(error)) != 0)
        goto fail;
    if (journal_phase(journal, &state, "committed",
                      error, sizeof(error)) != 0)
        goto fail;

    transaction_cleanup(journal, &state);
    if (live) {
        (void)printf(
            "@@LIVE_RESET {\"reason\":\"authoritative post-commit APFS map\"}\n");
        (void)fflush(stdout);
    }
    (void)printf(
        "APFS %s completed; committed %" PRIu64 " KiB.\n",
        growth ? "Growth Defrag" : "Defrag",
        written / 1024U);
    ld_emit_result_event(stdout, mode, "completed", "");
    journal_free(&state);
    return 0;

fail:
    (void)fprintf(stderr, "%s: %s\n", PROG,
                  error[0] != '\0'
                      ? error : "APFS transaction failed");
    journal_free(&state);
    return 1;
}
