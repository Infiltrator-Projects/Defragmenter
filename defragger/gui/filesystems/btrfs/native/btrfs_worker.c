// SPDX-License-Identifier: GPL-3.0-or-later
#include "btrfs_native.h"
#include "version.h"

#include "infiltratr/core.h"
#include "infiltratr/config.h"
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define PROG "linux-defragger-btrfs-worker"

typedef struct {
    uint64_t start;
    uint64_t end;
} UnitRange;

typedef struct {
    UnitRange *items;
    size_t count;
} UnitRanges;

static void usage(FILE *stream)
{
    (void)fprintf(stream,
                  "Usage: %s --version | identify DEVICE | analyse-json DEVICE | "
                  "map DEVICE --cells COUNT | "
                  "defrag|growth-defrag|recover DEVICE --write --confirm DEVICE "
                  "--journal PATH [--growth-percent 10] [--live-updates]\n", PROG);
}

static int unit_range_compare(const void *left, const void *right)
{
    const UnitRange *a = left;
    const UnitRange *b = right;
    if (a->start < b->start) return -1;
    if (a->start > b->start) return 1;
    if (a->end < b->end) return -1;
    if (a->end > b->end) return 1;
    return 0;
}

static int make_unit_ranges(const BtrfsRange *ranges, size_t count, uint32_t unit_size,
                            uint64_t limit, UnitRanges *output)
{
    output->items = NULL;
    output->count = 0U;
    if (count == 0U)
        return 0;
    UnitRange *items = calloc(count, sizeof(*items));
    if (items == NULL)
        return -1;
    size_t used = 0U;
    for (size_t i = 0U; i < count; ++i) {
        uint64_t start = ranges[i].start / unit_size;
        uint64_t end = ranges[i].end / unit_size;
        if (ranges[i].end % unit_size != 0U)
            end++;
        if (start > limit) start = limit;
        if (end > limit) end = limit;
        if (end > start)
            items[used++] = (UnitRange){start, end};
    }
    if (used > 1U)
        qsort(items, used, sizeof(*items), unit_range_compare);
    size_t merged = 0U;
    for (size_t i = 0U; i < used; ++i) {
        if (merged != 0U && items[i].start <= items[merged - 1U].end) {
            if (items[i].end > items[merged - 1U].end)
                items[merged - 1U].end = items[i].end;
        } else {
            items[merged++] = items[i];
        }
    }
    output->items = items;
    output->count = merged;
    return 0;
}

static uint64_t overlap_ranges(const UnitRanges *ranges, uint64_t start, uint64_t end)
{
    uint64_t total = 0U;
    for (size_t i = 0U; i < ranges->count; ++i) {
        if (ranges->items[i].end <= start)
            continue;
        if (ranges->items[i].start >= end)
            break;
        const uint64_t left = ranges->items[i].start > start ? ranges->items[i].start : start;
        const uint64_t right = ranges->items[i].end < end ? ranges->items[i].end : end;
        if (right > left)
            total += right - left;
    }
    return total;
}

static int parse_cells(const char *text, uint64_t *cells)
{
    uint64_t value = 0U;
    if (!infiltratr_parse_u64_range(text, 10U, 1U, UINT64_MAX, &value))
        return -1;
    *cells = value;
    return 0;
}

static void print_identify(void)
{
    (void)puts("{\"filesystem\":\"btrfs\"}");
}

static void print_analysis_json(const BtrfsAnalysis *analysis)
{
    const double percent = infiltratr_percent_u64(
        analysis->fragmented_files, analysis->regular_files);
    (void)printf(
        "{\"filesystem\":\"btrfs\",\"sector_size\":%u,\"node_size\":%u,"
        "\"device_id\":%" PRIu64 ",\"filesystem_bytes\":%" PRIu64 ","
        "\"physical_bytes\":%" PRIu64 ",\"logical_bytes_used\":%" PRIu64 ","
        "\"chunks\":%zu,\"chunk_tree_blocks\":%zu,\"root_tree_blocks\":%zu,"
        "\"extent_tree_blocks\":%zu,\"regular_files\":%" PRIu64 ","
        "\"directories\":%" PRIu64 ",\"fragmented_files\":%" PRIu64 ","
        "\"fragmented_directories\":%" PRIu64 ",\"fragmentation_percent\":%.6f,"
        "\"filesystem_roots_scanned\":%" PRIu64 ",\"filesystem_tree_blocks\":%" PRIu64 ","
        "\"malformed_items\":%" PRIu64 "}\n",
        analysis->sector_size, analysis->node_size, analysis->device_id,
        analysis->total_bytes, analysis->physical_bytes, analysis->logical_bytes_used,
        analysis->chunk_count, analysis->chunk_tree_blocks, analysis->root_tree_blocks,
        analysis->extent_tree_blocks, analysis->regular_files, analysis->directories,
        analysis->fragmented_files, analysis->fragmented_directories, percent,
        analysis->filesystem_roots_scanned, analysis->filesystem_tree_blocks,
        analysis->malformed_items);
}

static int print_map_json(const BtrfsAnalysis *analysis, uint64_t requested_cells)
{
    const uint64_t unit = analysis->sector_size;
    uint64_t filesystem_units = analysis->total_bytes / unit;
    if (analysis->total_bytes % unit != 0U)
        filesystem_units++;
    uint64_t physical = analysis->physical_bytes > analysis->total_bytes ?
                        analysis->physical_bytes : analysis->total_bytes;
    uint64_t total_units = physical / unit;
    if (physical % unit != 0U)
        total_units++;
    if (total_units == 0U)
        total_units = 1U;
    if (filesystem_units > total_units)
        filesystem_units = total_units;
    uint64_t cells = requested_cells;
    if (cells > total_units)
        cells = total_units;
    if (cells == 0U)
        cells = 1U;

    UnitRanges used = {0};
    UnitRanges fragmented = {0};
    if (make_unit_ranges(analysis->used_ranges, analysis->used_range_count,
                         analysis->sector_size, filesystem_units, &used) != 0 ||
        make_unit_ranges(analysis->fragmented_ranges, analysis->fragmented_range_count,
                         analysis->sector_size, filesystem_units, &fragmented) != 0) {
        free(used.items);
        free(fragmented.items);
        return -1;
    }

    uint64_t free_total = 0U;
    uint64_t used_total = 0U;
    uint64_t unknown_total = 0U;
    uint64_t outside_total = 0U;
    uint64_t fragmented_total = 0U;
    for (size_t i = 0U; i < fragmented.count; ++i)
        fragmented_total += fragmented.items[i].end - fragmented.items[i].start;

    (void)printf(
        "{\"schema\":1,\"backend\":\"read-only-domain\",\"filesystem\":\"btrfs\","
        "\"map_accuracy\":\"exact-single-device\",\"unit_size\":%u,"
        "\"total_units\":%" PRIu64 ",\"cell_count\":%" PRIu64 ","
        "\"total_bytes\":%" PRIu64 ",",
        analysis->sector_size, total_units, cells, total_units * unit);

    /* Totals are emitted after cell accounting, so buffer the cells in memory as
       compact count records rather than building a second allocation bitmap. */
    typedef struct {
        uint64_t start, end, free_count, used_count, unknown_count, outside_count, fragmented_count;
    } Cell;
    Cell *cell_values = calloc((size_t)cells, sizeof(*cell_values));
    if (cell_values == NULL) {
        free(used.items);
        free(fragmented.items);
        return -1;
    }
    for (uint64_t index = 0U; index < cells; ++index) {
        const uint64_t start = index * total_units / cells;
        uint64_t end = (index + 1U) * total_units / cells;
        if (end <= start)
            end = start + 1U;
        const uint64_t fs_end = end < filesystem_units ? end : filesystem_units;
        const uint64_t fs_start = start < filesystem_units ? start : filesystem_units;
        const uint64_t filesystem_count = fs_end > fs_start ? fs_end - fs_start : 0U;
        const uint64_t used_count = overlap_ranges(&used, start, fs_end);
        const uint64_t free_count = filesystem_count >= used_count ? filesystem_count - used_count : 0U;
        const uint64_t outside_start = start > filesystem_units ? start : filesystem_units;
        const uint64_t outside_count = end > outside_start ? end - outside_start : 0U;
        const uint64_t accounted = free_count + used_count + outside_count;
        const uint64_t length = end - start;
        const uint64_t unknown_count = length > accounted ? length - accounted : 0U;
        uint64_t fragmented_count = overlap_ranges(&fragmented, start, end);
        if (fragmented_count > used_count)
            fragmented_count = used_count;
        cell_values[index] = (Cell){
            start, end - 1U, free_count, used_count, unknown_count,
            outside_count, fragmented_count,
        };
        free_total += free_count;
        used_total += used_count;
        unknown_total += unknown_count;
        outside_total += outside_count;
    }

    (void)printf(
        "\"free_bytes\":%" PRIu64 ",\"used_bytes\":%" PRIu64 ","
        "\"unknown_bytes\":%" PRIu64 ",\"cells\":[",
        free_total * unit, used_total * unit, unknown_total * unit);
    for (uint64_t index = 0U; index < cells; ++index) {
        const Cell *cell = &cell_values[index];
        if (index != 0U)
            (void)putchar(',');
        (void)printf(
            "{\"start\":%" PRIu64 ",\"end\":%" PRIu64 ",\"free\":%" PRIu64 ","
            "\"used\":%" PRIu64 ",\"unknown\":%" PRIu64 ",\"bad\":0,"
            "\"fragmented\":%" PRIu64 ",\"directory\":0,\"outside\":%" PRIu64 "}",
            cell->start, cell->end, cell->free_count, cell->used_count,
            cell->unknown_count, cell->fragmented_count, cell->outside_count);
    }
    const double percent = infiltratr_percent_u64(
        analysis->fragmented_files, analysis->regular_files);
    (void)printf(
        "],\"details\":{\"sector_size\":%u,\"node_size\":%u,\"device_id\":%" PRIu64 ","
        "\"chunks\":%zu,\"logical_bytes_used\":%" PRIu64 ",\"chunk_tree_blocks\":%zu,"
        "\"root_tree_blocks\":%zu,\"extent_tree_blocks\":%zu,"
        "\"physical_units\":%" PRIu64 ",\"filesystem_units\":%" PRIu64 ","
        "\"outside_filesystem_units\":%" PRIu64 ",\"fragmentation_available\":true,"
        "\"fragmentation_basis\":\"Btrfs inode and FILE_EXTENT_ITEM records across live filesystem roots\","
        "\"directory_fragmentation_note\":\"Btrfs directory records share filesystem-tree blocks and do not form private block chains\","
        "\"filesystem_roots_scanned\":%" PRIu64 ",\"filesystem_tree_blocks\":%" PRIu64 ","
        "\"malformed_items\":%" PRIu64 ",\"fragmented_sectors_mapped\":%" PRIu64 "},"
        "\"filesystem_units\":%" PRIu64 ",\"filesystem_bytes\":%" PRIu64 ","
        "\"outside_bytes\":%" PRIu64 ",\"regular_files\":%" PRIu64 ","
        "\"directories\":%" PRIu64 ",\"fragmented_files\":%" PRIu64 ","
        "\"fragmented_directories\":%" PRIu64 ",\"fragmentation_percent\":%.6f}\n",
        analysis->sector_size, analysis->node_size, analysis->device_id,
        analysis->chunk_count, analysis->logical_bytes_used, analysis->chunk_tree_blocks,
        analysis->root_tree_blocks, analysis->extent_tree_blocks, total_units,
        filesystem_units, outside_total, analysis->filesystem_roots_scanned,
        analysis->filesystem_tree_blocks, analysis->malformed_items, fragmented_total,
        filesystem_units, filesystem_units * unit, outside_total * unit,
        analysis->regular_files, analysis->directories, analysis->fragmented_files,
        analysis->fragmented_directories, percent);

    free(cell_values);
    free(used.items);
    free(fragmented.items);
    return 0;
}


#define BTRFS_JOURNAL_MAGIC "LINUX-DEFRAGGER-BTRFS-JOURNAL-1"
#define BTRFS_STAGE_SUFFIX ".btrfs-stage"
#define BTRFS_TXN_IO_BYTES (1024U * 1024U)
#define BTRFS_TXN_STOPPED 130

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
    uint64_t generation;
    uint32_t sector_size;
} BtrfsJournal;

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

static void journal_free(BtrfsJournal *state)
{
    free(state->device);
    free(state->target_identity);
    free(state->stage);
    memset(state, 0, sizeof(*state));
}

static bool journal_write_stream(FILE *file, const void *user_data)
{
    const BtrfsJournal *state = user_data;
    (void)fprintf(file, "%s\n", BTRFS_JOURNAL_MAGIC);
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
    (void)fprintf(file, "generation=%" PRIu64 "\n", state->generation);
    (void)fprintf(file, "sector_size=%u\n", state->sector_size);
    return !ferror(file);
}

static int journal_save(const char *path, const BtrfsJournal *state,
                        char *error, size_t error_size)
{
    if (!safe_value(state->device) || !safe_value(state->target_identity) ||
        !safe_value(state->stage)) {
        txn_error(error, error_size,
                  "Btrfs transaction paths contain unsupported journal characters");
        return -1;
    }
    char *parent = ld_path_parent_directory(path);
    if (parent == NULL || ld_path_ensure_trusted_directory_tree(parent) != 0) {
        txn_error(error, error_size,
                  "cannot create Btrfs journal directory: %s", strerror(errno));
        free(parent);
        return -1;
    }
    free(parent);
    const int failure = infiltratr_atomic_file_write(
        path, INFILTRATR_ATOMIC_FILE_PRIVATE, journal_write_stream, state);
    if (failure != 0) {
        txn_error(error, error_size,
                  "cannot publish Btrfs recovery journal: %s", strerror(failure));
        return -1;
    }
    return 0;
}

static int parse_u64_value(const char *text, uint64_t *value)
{
    return infiltratr_parse_u64(text, 10U, value) ? 0 : -1;
}

static int journal_load(const char *path, BtrfsJournal *state,
                        char *error, size_t error_size)
{
    memset(state, 0, sizeof(*state));
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        txn_error(error, error_size,
                  "cannot open Btrfs recovery journal: %s", strerror(errno));
        return -1;
    }
    char *line = NULL;
    size_t capacity = 0U;
    if (getline(&line, &capacity, file) < 0) goto invalid;
    infiltratr_trim_line_end(line);
    if (strcmp(line, BTRFS_JOURNAL_MAGIC) != 0) goto invalid;
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
        } else if (strcmp(key, "generation") == 0) {
            if (parse_u64_value(value, &state->generation) != 0) goto invalid;
        } else if (strcmp(key, "sector_size") == 0) {
            uint64_t parsed = 0U;
            if (parse_u64_value(value, &parsed) != 0 || parsed > UINT32_MAX)
                goto invalid;
            state->sector_size = (uint32_t)parsed;
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
        state->sector_size == 0U)
        goto invalid_state;
    return 0;
invalid:
    free(line);
    (void)fclose(file);
invalid_state:
    journal_free(state);
    txn_error(error, error_size,
              "Btrfs recovery journal is malformed or incomplete");
    return -1;
}

static int journal_phase(const char *path, BtrfsJournal *state,
                         const char *phase, char *error, size_t error_size)
{
    infiltratr_copy_string(state->phase, sizeof(state->phase), phase);
    return journal_save(path, state, error, error_size);
}

static char *stage_name(const char *journal)
{
    return ld_path_append_suffix(journal, BTRFS_STAGE_SUFFIX);
}

static void transaction_cleanup(const char *journal,
                                const BtrfsJournal *state)
{
    if (state != NULL) unlink_if_exists(state->stage);
    unlink_if_exists(journal);
}

static int digest_final(EVP_MD_CTX *context, char output[65],
                        char *error, size_t error_size)
{
    unsigned char digest[32];
    unsigned int length = 0U;
    if (EVP_DigestFinal_ex(context, digest, &length) != 1 ||
        length != sizeof(digest)) {
        txn_error(error, error_size, "finalising Btrfs SHA-256 failed");
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
                  "cannot open Btrfs bytes for SHA-256: %s", strerror(errno));
        return -1;
    }
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    uint8_t *buffer = malloc(BTRFS_TXN_IO_BYTES);
    if (context == NULL || buffer == NULL ||
        EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(context); free(buffer); (void)close(fd);
        txn_error(error, error_size, "cannot initialise Btrfs SHA-256");
        return -1;
    }
    int rc = 0;
    for (uint64_t offset = 0U; offset < bytes;) {
        if (stoppable && ld_stop_requested()) {
            rc = BTRFS_TXN_STOPPED;
            break;
        }
        const uint64_t remaining = bytes - offset;
        const size_t count = remaining > BTRFS_TXN_IO_BYTES
                           ? BTRFS_TXN_IO_BYTES : (size_t)remaining;
        if (ld_pread_full(fd, buffer, count, offset) != (ssize_t)count ||
            EVP_DigestUpdate(context, buffer, count) != 1) {
            txn_error(error, error_size, "hashing Btrfs bytes failed");
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
    return hash_prefix(path, 64U * 1024U + 4096U, false,
                       output, error, error_size);
}

static int capture_target(const char *device, BtrfsJournal *state,
                          char *error, size_t error_size)
{
    if (ld_device_capture_binding(device, &state->device,
                                  &state->target_identity,
                                  &state->physical_bytes) != 0) {
        txn_error(error, error_size,
                  "cannot bind Btrfs target: %s", strerror(errno));
        return -1;
    }
    BtrfsAnalysis analysis;
    if (btrfs_analyse(state->device, &analysis, error, error_size) != 0)
        return -1;
    state->filesystem_bytes = analysis.total_bytes;
    state->generation = analysis.generation;
    state->sector_size = analysis.sector_size;
    btrfs_analysis_free(&analysis);
    if (state->filesystem_bytes > state->physical_bytes ||
        primary_super_token(state->device, state->volume_token,
                            error, error_size) != 0)
        return -1;
    return hash_prefix(state->device, state->filesystem_bytes, true,
                       state->source_sha256, error, error_size);
}

static int check_target_identity(const char *device, const BtrfsJournal *state,
                                 char *error, size_t error_size)
{
    char *canonical = NULL;
    char *identity = NULL;
    uint64_t size = 0U;
    int rc = ld_device_capture_binding(
        device, &canonical, &identity, &size);
    if (rc != 0) {
        txn_error(error, error_size,
                  "cannot rebind Btrfs target: %s", strerror(errno));
    }
    if (rc == 0 &&
        (strcmp(canonical, state->device) != 0 ||
         strcmp(identity, state->target_identity) != 0 ||
         size != state->physical_bytes)) {
        txn_error(error, error_size,
                  "Btrfs target path, identity or capacity changed before commit");
        rc = -1;
    }
    free(identity); free(canonical);
    return rc;
}

static int check_source_unchanged(const char *device,
                                  const BtrfsJournal *state,
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
                  "Btrfs source changed after preflight; refusing source writes");
        return -1;
    }
    return 0;
}

static int safe_commit_stage(const char *stage_path, const char *target_path,
                             const BtrfsJournal *state, uint64_t *written,
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
                  "cannot open Btrfs stage or target for commit: %s",
                  strerror(errno));
        return -1;
    }
    if (flock(target, LOCK_EX | LOCK_NB) != 0) {
        txn_error(error, error_size,
                  "cannot lock Btrfs target for commit: %s", strerror(errno));
        (void)close(stage); (void)close(target);
        return -1;
    }
    uint8_t *buffer = malloc(BTRFS_TXN_IO_BYTES);
    if (buffer == NULL) {
        txn_error(error, error_size, "out of memory committing Btrfs stage");
        (void)flock(target, LOCK_UN); (void)close(stage); (void)close(target);
        return -1;
    }
    int rc = 0;
    uint64_t total = 0U;
    for (uint64_t offset = 0U; offset < state->filesystem_bytes;) {
        if (ld_stop_requested()) {
            rc = ld_sync_fd(target) == 0 ? BTRFS_TXN_STOPPED : -1;
            if (rc < 0)
                txn_error(error, error_size,
                          "cannot sync Btrfs target at Stop boundary: %s",
                          strerror(errno));
            break;
        }
        const uint64_t remaining = state->filesystem_bytes - offset;
        const size_t count = remaining > BTRFS_TXN_IO_BYTES
                           ? BTRFS_TXN_IO_BYTES : (size_t)remaining;
        if (ld_pread_full(stage, buffer, count, offset) != (ssize_t)count ||
            ld_pwrite_full(target, buffer, count, offset) != (ssize_t)count) {
            txn_error(error, error_size,
                      "short I/O committing Btrfs bytes at offset %" PRIu64,
                      offset);
            rc = -1;
            break;
        }
        offset += count; total += count;
    }
    if (rc == 0 && ld_sync_fd(target) != 0) {
        txn_error(error, error_size,
                  "cannot sync Btrfs target: %s", strerror(errno));
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
    BtrfsJournal state;
    if (journal_load(journal, &state, error, error_size) != 0)
        return 1;
    if (!ld_path_is_derived_from(state.stage, journal, BTRFS_STAGE_SUFFIX) ||
        !valid_operation(state.operation) || !valid_phase(state.phase)) {
        txn_error(error, error_size,
                  "Btrfs recovery journal has an invalid stage binding, operation or phase");
        journal_free(&state);
        return 1;
    }
    const int identity_rc = strcmp(state.phase, "staged") == 0
        ? check_source_unchanged(device, &state, error, error_size)
        : check_target_identity(device, &state, error, error_size);
    if (identity_rc != 0) {
        journal_free(&state);
        return identity_rc == BTRFS_TXN_STOPPED ? BTRFS_TXN_STOPPED : 1;
    }
    char digest[65];
    const int hash_rc = hash_prefix(state.stage, state.filesystem_bytes, true,
                                    digest, error, error_size);
    if (hash_rc != 0 || strcmp(digest, state.stage_sha256) != 0) {
        if (hash_rc == 0)
            txn_error(error, error_size,
                      "Btrfs recovery stage SHA-256 does not match the journal");
        journal_free(&state);
        return hash_rc == BTRFS_TXN_STOPPED ? BTRFS_TXN_STOPPED : 1;
    }
    const bool growth = strcmp(state.operation, "growth-defrag") == 0;
    if (btrfs_verify_layout(state.stage, growth, 10U,
                            error, error_size) != 0) {
        journal_free(&state);
        return 1;
    }
    if (strcmp(state.phase, "committed") == 0) {
        if (btrfs_verify_layout(device, growth, 10U,
                                error, error_size) != 0) {
            journal_free(&state);
            return 1;
        }
        transaction_cleanup(journal, &state);
        ld_emit_result_event(stdout, "recover", "completed",
                             "Verified an already committed Btrfs transaction.");
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
    if (commit_rc == BTRFS_TXN_STOPPED) {
        ld_emit_result_event(stdout, "recover", "stopped",
                             "Recovery stopped at a durable boundary and can be resumed.");
        journal_free(&state);
        return BTRFS_TXN_STOPPED;
    }
    if (commit_rc != 0 ||
        btrfs_verify_layout(state.device, growth, 10U,
                            error, error_size) != 0 ||
        journal_phase(journal, &state, "committed",
                      error, error_size) != 0) {
        journal_free(&state);
        return 1;
    }
    transaction_cleanup(journal, &state);
    if (live) {
        (void)printf(
            "@@LIVE_RESET {\"reason\":\"authoritative post-recovery Btrfs map\"}\n");
        (void)fflush(stdout);
    }
    (void)printf("Recovered verified Btrfs source; committed %" PRIu64 " KiB.\n",
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
        if (!btrfs_probe(argv[2])) return 1;
        print_identify(); return 0;
    }
    if (argc == 5 && strcmp(argv[1], "map") == 0 &&
        strcmp(argv[3], "--cells") == 0) {
        uint64_t cells = 0U;
        if (parse_cells(argv[4], &cells) != 0) {
            (void)fprintf(stderr, "%s: invalid cell count\n", PROG);
            return 2;
        }
        BtrfsAnalysis analysis;
        char error[512] = {0};
        if (btrfs_analyse(argv[2], &analysis, error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "%s: %s\n", PROG,
                          error[0] != '\0' ? error : "Btrfs analysis failed");
            return 1;
        }
        const int result = print_map_json(&analysis, cells);
        btrfs_analysis_free(&analysis);
        return result == 0 ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "analyse-json") == 0) {
        BtrfsAnalysis analysis;
        char error[512] = {0};
        if (btrfs_analyse(argv[2], &analysis, error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "%s: %s\n", PROG,
                          error[0] != '\0' ? error : "Btrfs analysis failed");
            return 1;
        }
        print_analysis_json(&analysis);
        btrfs_analysis_free(&analysis);
        return 0;
    }
    if (argc < 3) { usage(stderr); return 2; }

    const char *mode = argv[1];
    const char *device = argv[2];
    const bool growth = strcmp(mode, "growth-defrag") == 0;
    const bool defrag = strcmp(mode, "defrag") == 0;
    const bool recover = strcmp(mode, "recover") == 0;
    if (!growth && !defrag && !recover) { usage(stderr); return 2; }

    const char *confirm = NULL;
    const char *journal = NULL;
    unsigned growth_percent = 10U;
    bool write = false;
    bool live = false;
    for (int index = 3; index < argc; ++index) {
        if (strcmp(argv[index], "--write") == 0) write = true;
        else if (strcmp(argv[index], "--live-updates") == 0) live = true;
        else if (strcmp(argv[index], "--confirm") == 0 && index + 1 < argc)
            confirm = argv[++index];
        else if (strcmp(argv[index], "--journal") == 0 && index + 1 < argc)
            journal = argv[++index];
        else if (strcmp(argv[index], "--growth-percent") == 0 &&
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
            (void)fprintf(stderr, "%s: unknown or incomplete Btrfs option: %s\n",
                          PROG, argv[index]);
            return 2;
        }
    }
    if (!write || confirm == NULL || journal == NULL ||
        strcmp(confirm, device) != 0) {
        (void)fprintf(stderr,
                      "%s: Btrfs mutation requires --write --confirm DEVICE --journal PATH\n",
                      PROG);
        return 2;
    }
    if (growth && growth_percent != 10U) {
        (void)fprintf(stderr,
                      "%s: Btrfs Growth Defrag requires exactly 10 percent reserve\n",
                      PROG);
        return 2;
    }
    if (ld_path_is_mounted(device)) {
        (void)fprintf(stderr,
                      "%s: Btrfs target is mounted; raw mutation and recovery require an unmounted filesystem\n",
                      PROG);
        return 1;
    }

    ld_stop_clear();
    ld_stop_install_handlers();
    ld_stop_report_ready();
    char error[512] = {0};
    if (recover) {
        const int rc = recover_transaction(device, journal, live,
                                           error, sizeof(error));
        if (rc != 0 && rc != BTRFS_TXN_STOPPED)
            (void)fprintf(stderr, "%s: %s\n", PROG,
                          error[0] != '\0' ? error : "Btrfs recovery failed");
        return rc;
    }

    BtrfsJournal state;
    memset(&state, 0, sizeof(state));
    infiltratr_copy_string(state.operation, sizeof(state.operation), mode);
    const int capture_rc =
        capture_target(device, &state, error, sizeof(error));
    if (capture_rc != 0) {
        if (capture_rc == BTRFS_TXN_STOPPED)
            ld_emit_result_event(stdout, mode, "stopped",
                                 "Stopped during read-only Btrfs preflight.");
        goto fail;
    }
    state.stage = stage_name(journal);
    if (state.stage == NULL) {
        txn_error(error, sizeof(error),
                  "out of memory creating Btrfs stage path");
        goto fail;
    }
    if (access(journal, F_OK) == 0 || access(state.stage, F_OK) == 0) {
        txn_error(error, sizeof(error),
                  "existing Btrfs recovery artifacts must be recovered or removed before starting");
        goto fail;
    }

    (void)printf("Starting native C Btrfs %s on %s.\n",
                 growth ? "Growth Defrag" : "Defrag", device);
    uint64_t planned = 0U;
    const int stage_rc =
        btrfs_build_stage(state.device, state.stage, growth,
                          growth_percent, live, &planned,
                          error, sizeof(error));
    if (stage_rc != 0 ||
        btrfs_verify_layout(state.stage, growth, growth_percent,
                            error, sizeof(error)) != 0) {
        unlink_if_exists(state.stage);
        if (stage_rc == BTRFS_TXN_STOPPED) {
            ld_emit_result_event(stdout, mode, "stopped",
                                 "Stopped before any Btrfs source writes.");
            journal_free(&state);
            return BTRFS_TXN_STOPPED;
        }
        goto fail;
    }
    const int unchanged =
        check_source_unchanged(state.device, &state, error, sizeof(error));
    if (unchanged != 0) {
        unlink_if_exists(state.stage);
        if (unchanged == BTRFS_TXN_STOPPED) {
            ld_emit_result_event(stdout, mode, "stopped",
                                 "Stopped before any Btrfs source writes.");
            journal_free(&state);
            return BTRFS_TXN_STOPPED;
        }
        goto fail;
    }
    const int hash_rc =
        hash_prefix(state.stage, state.filesystem_bytes, true,
                    state.stage_sha256, error, sizeof(error));
    if (hash_rc != 0) {
        unlink_if_exists(state.stage);
        if (hash_rc == BTRFS_TXN_STOPPED) {
            ld_emit_result_event(stdout, mode, "stopped",
                                 "Stopped before any Btrfs source writes.");
            journal_free(&state);
            return BTRFS_TXN_STOPPED;
        }
        goto fail;
    }
    infiltratr_copy_string(state.phase, sizeof(state.phase), "staged");
    if (journal_save(journal, &state, error, sizeof(error)) != 0)
        goto fail;
    if (journal_phase(journal, &state, "committing",
                      error, sizeof(error)) != 0)
        goto fail;

    (void)printf("Btrfs source commit: replaying %" PRIu64
                 " KiB from the verified transaction stage.\n",
                 planned / 1024U);
    (void)fflush(stdout);
    uint64_t written = 0U;
    const int commit_rc =
        safe_commit_stage(state.stage, state.device, &state,
                          &written, error, sizeof(error));
    if (commit_rc == BTRFS_TXN_STOPPED) {
        ld_emit_result_event(stdout, mode, "stopped",
                             "Run Recover to resume the verified Btrfs transaction.");
        journal_free(&state);
        return BTRFS_TXN_STOPPED;
    }
    if (commit_rc != 0 ||
        btrfs_verify_layout(state.device, growth, growth_percent,
                            error, sizeof(error)) != 0)
        goto fail;
    if (journal_phase(journal, &state, "committed",
                      error, sizeof(error)) != 0)
        goto fail;
    transaction_cleanup(journal, &state);
    if (live) {
        (void)printf(
            "@@LIVE_RESET {\"reason\":\"authoritative post-commit Btrfs map\"}\n");
        (void)fflush(stdout);
    }
    (void)printf("Btrfs %s completed; committed %" PRIu64 " KiB.\n",
                 growth ? "Growth Defrag" : "Defrag", written / 1024U);
    ld_emit_result_event(stdout, mode, "completed", "");
    journal_free(&state);
    return 0;

fail:
    (void)fprintf(stderr, "%s: %s\n", PROG,
                  error[0] != '\0' ? error : "Btrfs transaction failed");
    journal_free(&state);
    return 1;
}
