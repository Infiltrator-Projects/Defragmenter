// SPDX-License-Identifier: GPL-3.0-or-later
#include "ufs_native.h"
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
#include <linux/fs.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define PROG "linux-defragger-ufs-worker"
#define UFS_SUMMARY_UNIT_SIZE 512U

static void usage(FILE *stream)
{
    (void)fprintf(stream,
                  "Usage: %s --version | identify DEVICE | analyse-json DEVICE | "
                  "map DEVICE --cells COUNT | "
                  "defrag|growth-defrag|recover DEVICE --write --confirm DEVICE "
                  "--journal PATH [--growth-percent 10] [--live-updates]\n",
                  PROG);
}

static int device_size_bytes(const char *path, uint64_t *size_bytes)
{
    struct stat status;
    if (stat(path, &status) != 0)
        return -1;
    if (S_ISREG(status.st_mode)) {
        if (status.st_size < 0) {
            errno = EINVAL;
            return -1;
        }
        *size_bytes = (uint64_t)status.st_size;
        return 0;
    }
    if (!S_ISBLK(status.st_mode)) {
        errno = EINVAL;
        return -1;
    }

    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    uint64_t bytes = 0U;
    const int result = ioctl(fd, BLKGETSIZE64, &bytes);
    const int saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;
    if (result != 0)
        return -1;
    *size_bytes = bytes;
    return 0;
}

static int parse_cells(const char *text, uint64_t *cells)
{
    return infiltratr_parse_u64_range(text, 10U, 1U, UINT64_MAX, cells)
        ? 0 : -1;
}

static bool exact_ufs_ready(const LdUfsSummary *summary)
{
    return summary->allocation_totals_known &&
           summary->cylinder_geometry_known;
}

static void print_summary_json(const LdUfsSummary *summary, int detailed)
{
    (void)printf("{\"filesystem\":\"ufs\",\"variant\":\"%s\","
                 "\"version\":%u,\"byte_order\":\"%s\"",
                 ufs_variant_name(summary), ufs_version(summary),
                 ufs_byte_order_name(summary));
    if (detailed != 0) {
        (void)printf(",\"superblock_offset\":%llu,\"magic_offset\":%llu",
                     (unsigned long long)summary->superblock_offset,
                     (unsigned long long)summary->magic_offset);
        if (summary->allocation_totals_known) {
            (void)printf(",\"allocation_totals\":\"recorded-superblock\","
                         "\"block_size\":%u,\"fragment_size\":%u,"
                         "\"filesystem_bytes\":%llu,\"free_bytes\":%llu,"
                         "\"used_bytes\":%llu,\"exact_allocation_available\":%s",
                         summary->block_size, summary->fragment_size,
                         (unsigned long long)(summary->filesystem_fragments *
                                              summary->fragment_size),
                         (unsigned long long)ufs_recorded_free_bytes(summary),
                         (unsigned long long)ufs_recorded_used_bytes(summary),
                         exact_ufs_ready(summary) ? "true" : "false");
        }
    }
    (void)puts("}");
}

static void print_exact_analysis_json(const LdUfsAnalysis *analysis)
{
    const LdUfsSummary *summary = &analysis->summary;
    (void)printf(
        "{\"filesystem\":\"ufs\",\"variant\":\"%s\",\"version\":%u,"
        "\"byte_order\":\"%s\",\"superblock_offset\":%llu,"
        "\"magic_offset\":%llu,\"map_accuracy\":\"exact\","
        "\"block_size\":%u,\"fragment_size\":%u,\"cylinder_groups\":%u,"
        "\"filesystem_bytes\":%llu,\"physical_bytes\":%llu,"
        "\"free_bytes\":%llu,\"used_bytes\":%llu,"
        "\"regular_files\":%llu,\"directories\":%llu,"
        "\"fragmented_files\":%llu,\"fragmented_directories\":%llu,"
        "\"sparse_files\":%llu,\"indirect_files\":%llu}\n",
        ufs_variant_name(summary), ufs_version(summary),
        ufs_byte_order_name(summary),
        (unsigned long long)summary->superblock_offset,
        (unsigned long long)summary->magic_offset,
        summary->block_size, summary->fragment_size,
        summary->cylinder_groups,
        (unsigned long long)analysis->filesystem_bytes,
        (unsigned long long)analysis->physical_bytes,
        (unsigned long long)(analysis->free_fragments_exact *
                             summary->fragment_size),
        (unsigned long long)(analysis->used_fragments_exact *
                             summary->fragment_size),
        (unsigned long long)analysis->regular_files,
        (unsigned long long)analysis->directories,
        (unsigned long long)analysis->fragmented_files,
        (unsigned long long)analysis->fragmented_directories,
        (unsigned long long)analysis->sparse_files,
        (unsigned long long)analysis->indirect_files);
}

static void print_summary_map(const LdUfsSummary *summary, uint64_t size_bytes,
                              uint64_t requested_cells)
{
    uint64_t total_units = size_bytes / UFS_SUMMARY_UNIT_SIZE;
    if (size_bytes % UFS_SUMMARY_UNIT_SIZE != 0U)
        total_units++;
    if (total_units == 0U)
        total_units = 1U;

    uint64_t cell_count = requested_cells;
    if (cell_count > total_units)
        cell_count = total_units;
    if (cell_count == 0U)
        cell_count = 1U;

    const uint64_t rounded_bytes = total_units * UFS_SUMMARY_UNIT_SIZE;
    const uint64_t free_bytes = ufs_recorded_free_bytes(summary);
    const uint64_t used_bytes = ufs_recorded_used_bytes(summary);

    (void)printf("{\"schema\":1,\"backend\":\"read-only-domain\","
                 "\"filesystem\":\"ufs\",\"map_accuracy\":\"summary\","
                 "\"unit_size\":%u,\"total_units\":%llu,\"cell_count\":%llu,"
                 "\"total_bytes\":%llu,\"free_bytes\":%llu,\"used_bytes\":%llu,"
                 "\"unknown_bytes\":%llu,\"cells\":[",
                 UFS_SUMMARY_UNIT_SIZE,
                 (unsigned long long)total_units,
                 (unsigned long long)cell_count,
                 (unsigned long long)rounded_bytes,
                 (unsigned long long)free_bytes,
                 (unsigned long long)used_bytes,
                 (unsigned long long)rounded_bytes);

    for (uint64_t index = 0U; index < cell_count; ++index) {
        const uint64_t start = (index * total_units) / cell_count;
        const uint64_t end_exclusive = ((index + 1U) * total_units) / cell_count;
        const uint64_t length = end_exclusive - start;
        if (index != 0U)
            (void)putchar(',');
        (void)printf("{\"start\":%llu,\"end\":%llu,\"free\":0,\"used\":0,"
                     "\"unknown\":%llu,\"bad\":0,\"fragmented\":0,"
                     "\"directory\":0}",
                     (unsigned long long)start,
                     (unsigned long long)(end_exclusive - 1U),
                     (unsigned long long)length);
    }

    (void)printf("],\"details\":{\"variant\":\"%s\","
                 "\"version\":%u,\"byte_order\":\"%s\","
                 "\"allocation_totals\":\"%s\","
                 "\"note\":\"UFS1 is identified read-only; physical cylinder-group allocation mapping is currently implemented for validated UFS2 only\"}}\n",
                 ufs_variant_name(summary), ufs_version(summary),
                 ufs_byte_order_name(summary),
                 summary->allocation_totals_known ? "recorded-superblock" : "unknown");
}

static int print_exact_map(const char *path, uint64_t requested_cells)
{
    LdUfsAnalysis analysis;
    char error[256] = {0};
    if (ufs_analyse_allocation(path, &analysis, NULL, 0U,
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
            (size_t)cell_count, sizeof(LdUfsMapCell), &map_bytes)) {
        (void)fprintf(stderr, "%s: map cell count is too large\n", PROG);
        return 1;
    }
    (void)map_bytes;
    LdUfsMapCell *cells = calloc((size_t)cell_count, sizeof(*cells));
    if (cells == NULL) {
        (void)fprintf(stderr, "%s: out of memory allocating map cells\n", PROG);
        return 1;
    }
    if (ufs_analyse_allocation(path, &analysis, cells, cell_count,
                               error, sizeof(error)) != 0) {
        free(cells);
        (void)fprintf(stderr, "%s: %s\n", PROG, error);
        return 1;
    }
    const LdUfsSummary *summary = &analysis.summary;
    const uint64_t outside = analysis.total_units > summary->filesystem_fragments
        ? analysis.total_units - summary->filesystem_fragments : 0U;
    (void)printf(
        "{\"schema\":1,\"backend\":\"read-only-domain\","
        "\"filesystem\":\"ufs\",\"map_accuracy\":\"exact\","
        "\"unit_size\":%u,\"total_units\":%llu,\"cell_count\":%llu,"
        "\"total_bytes\":%llu,\"filesystem_bytes\":%llu,"
        "\"free_bytes\":%llu,\"used_bytes\":%llu,\"unknown_bytes\":%llu,"
        "\"regular_files\":%llu,\"directories\":%llu,"
        "\"fragmented_files\":%llu,\"fragmented_directories\":%llu,"
        "\"cells\":[",
        summary->fragment_size,
        (unsigned long long)analysis.total_units,
        (unsigned long long)cell_count,
        (unsigned long long)(analysis.total_units * summary->fragment_size),
        (unsigned long long)analysis.filesystem_bytes,
        (unsigned long long)(analysis.free_fragments_exact *
                             summary->fragment_size),
        (unsigned long long)(analysis.used_fragments_exact *
                             summary->fragment_size),
        (unsigned long long)(outside * summary->fragment_size),
        (unsigned long long)analysis.regular_files,
        (unsigned long long)analysis.directories,
        (unsigned long long)analysis.fragmented_files,
        (unsigned long long)analysis.fragmented_directories);
    for (uint64_t index = 0U; index < cell_count; ++index) {
        if (index != 0U) (void)putchar(',');
        (void)printf(
            "{\"start\":%llu,\"end\":%llu,\"free\":%llu,\"used\":%llu,"
            "\"unknown\":%llu,\"bad\":0,\"fragmented\":%llu,\"directory\":%llu}",
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
        "\"byte_order\":\"%s\",\"cylinder_groups\":%u,"
        "\"allocation_basis\":\"validated UFS1/UFS2 cylinder-group fragment bitmaps\","
        "\"fragmentation_basis\":\"allocated inode direct/single/double/triple-indirect block trees\"}}\n",
        ufs_variant_name(summary), ufs_version(summary),
        ufs_byte_order_name(summary), summary->cylinder_groups);
    free(cells);
    return 0;
}
#define UFS_JOURNAL_MAGIC "LINUX-DEFRAGGER-UFS-JOURNAL-1"
#define UFS_STAGE_SUFFIX ".ufs-stage"
#define UFS_TXN_IO_BYTES (1024U * 1024U)
#define UFS_TXN_STOPPED 130

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
    uint64_t superblock_offset;
    uint32_t fragment_size;
} UFSJournal;

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

static void journal_free(UFSJournal *state)
{
    free(state->device);
    free(state->target_identity);
    free(state->stage);
    memset(state, 0, sizeof(*state));
}

static bool journal_write_stream(FILE *file, const void *user_data)
{
    const UFSJournal *state = user_data;
    (void)fprintf(file, "%s\n", UFS_JOURNAL_MAGIC);
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
    (void)fprintf(file, "superblock_offset=%" PRIu64 "\n", state->superblock_offset);
    (void)fprintf(file, "fragment_size=%u\n", state->fragment_size);
    return !ferror(file);
}

static int journal_save(const char *path, const UFSJournal *state,
                        char *error, size_t error_size)
{
    if (!safe_value(state->device) || !safe_value(state->target_identity) ||
        !safe_value(state->stage)) {
        txn_error(error, error_size,
                  "UFS transaction paths contain unsupported journal characters");
        return -1;
    }
    char *parent = ld_path_parent_directory(path);
    if (parent == NULL || ld_path_ensure_trusted_directory_tree(parent) != 0) {
        txn_error(error, error_size,
                  "cannot create UFS journal directory: %s", strerror(errno));
        free(parent);
        return -1;
    }
    free(parent);
    const int failure = infiltratr_atomic_file_write(
        path, INFILTRATR_ATOMIC_FILE_PRIVATE, journal_write_stream, state);
    if (failure != 0) {
        txn_error(error, error_size,
                  "cannot publish UFS recovery journal: %s", strerror(failure));
        return -1;
    }
    return 0;
}

static int parse_u64_value(const char *text, uint64_t *value)
{
    return infiltratr_parse_u64(text, 10U, value) ? 0 : -1;
}

static int journal_load(const char *path, UFSJournal *state,
                        char *error, size_t error_size)
{
    memset(state, 0, sizeof(*state));
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        txn_error(error, error_size,
                  "cannot open UFS recovery journal: %s", strerror(errno));
        return -1;
    }
    char *line = NULL;
    size_t capacity = 0U;
    if (getline(&line, &capacity, file) < 0) goto invalid;
    infiltratr_trim_line_end(line);
    if (strcmp(line, UFS_JOURNAL_MAGIC) != 0) goto invalid;
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
        } else if (strcmp(key, "superblock_offset") == 0) {
            if (parse_u64_value(value, &state->superblock_offset) != 0) goto invalid;
        } else if (strcmp(key, "fragment_size") == 0) {
            uint64_t parsed = 0U;
            if (parse_u64_value(value, &parsed) != 0 || parsed > UINT32_MAX)
                goto invalid;
            state->fragment_size = (uint32_t)parsed;
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
        state->fragment_size == 0U)
        goto invalid_state;
    return 0;
invalid:
    free(line);
    (void)fclose(file);
invalid_state:
    journal_free(state);
    txn_error(error, error_size,
              "UFS recovery journal is malformed or incomplete");
    return -1;
}

static int journal_phase(const char *path, UFSJournal *state,
                         const char *phase, char *error, size_t error_size)
{
    infiltratr_copy_string(state->phase, sizeof(state->phase), phase);
    return journal_save(path, state, error, error_size);
}

static char *stage_name(const char *journal)
{
    return ld_path_append_suffix(journal, UFS_STAGE_SUFFIX);
}

static void transaction_cleanup(const char *journal,
                                const UFSJournal *state)
{
    if (state != NULL) unlink_if_exists(state->stage);
    unlink_if_exists(journal);
}

static char *canonical_path(const char *path, char *error, size_t error_size)
{
    char *resolved = realpath(path, NULL);
    if (resolved == NULL)
        txn_error(error, error_size,
                  "cannot resolve UFS target %s: %s", path, strerror(errno));
    return resolved;
}

static int target_identity(const char *path, char **identity, uint64_t *size,
                           char *error, size_t error_size)
{
    struct stat status;
    if (stat(path, &status) != 0) {
        txn_error(error, error_size,
                  "cannot stat UFS target: %s", strerror(errno));
        return -1;
    }
    if (!S_ISREG(status.st_mode) && !S_ISBLK(status.st_mode)) {
        txn_error(error, error_size,
                  "UFS target is not a block device or regular image");
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
        txn_error(error, error_size, "cannot determine UFS target size");
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
        txn_error(error, error_size, "finalising UFS SHA-256 failed");
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
                  "cannot open UFS bytes for SHA-256: %s", strerror(errno));
        return -1;
    }
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    uint8_t *buffer = malloc(UFS_TXN_IO_BYTES);
    if (context == NULL || buffer == NULL ||
        EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(context); free(buffer); (void)close(fd);
        txn_error(error, error_size, "cannot initialise UFS SHA-256");
        return -1;
    }
    int rc = 0;
    for (uint64_t offset = 0U; offset < bytes;) {
        if (stoppable && ld_stop_requested()) {
            rc = UFS_TXN_STOPPED;
            break;
        }
        const uint64_t remaining = bytes - offset;
        const size_t count = remaining > UFS_TXN_IO_BYTES
                           ? UFS_TXN_IO_BYTES : (size_t)remaining;
        if (ld_pread_full(fd, buffer, count, offset) != (ssize_t)count ||
            EVP_DigestUpdate(context, buffer, count) != 1) {
            txn_error(error, error_size, "hashing UFS bytes failed");
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
    LdUfsSummary summary;
    if (ufs_read_summary(path, &summary, error, error_size) != 0)
        return -1;
    if (summary.superblock_offset > UINT64_MAX - 1376U) {
        txn_error(error, error_size, "UFS superblock token range overflows");
        return -1;
    }
    return hash_prefix(path, summary.superblock_offset + 1376U, false,
                       output, error, error_size);
}

static int capture_target(const char *device, UFSJournal *state,
                          char *error, size_t error_size)
{
    state->device = canonical_path(device, error, error_size);
    if (state->device == NULL) return -1;
    if (target_identity(state->device, &state->target_identity,
                        &state->physical_bytes, error, error_size) != 0)
        return -1;
    LdUfsAnalysis analysis;
    if (ufs_analyse_allocation(state->device, &analysis, NULL, 0U,
                               error, error_size) != 0)
        return -1;
    if (!infiltratr_u64_multiply_checked(
            analysis.summary.filesystem_fragments,
            analysis.summary.fragment_size,
            &state->filesystem_bytes)) {
        txn_error(error, error_size, "UFS filesystem byte count overflows");
        return -1;
    }
    state->superblock_offset = analysis.summary.superblock_offset;
    state->fragment_size = analysis.summary.fragment_size;
    if (state->filesystem_bytes > state->physical_bytes ||
        primary_super_token(state->device, state->volume_token,
                            error, error_size) != 0)
        return -1;
    return hash_prefix(state->device, state->filesystem_bytes, true,
                       state->source_sha256, error, error_size);
}

static int check_target_identity(const char *device, const UFSJournal *state,
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
                  "UFS target path, identity or capacity changed before commit");
        rc = -1;
    }
    free(identity); free(canonical);
    return rc;
}

static int check_source_unchanged(const char *device,
                                  const UFSJournal *state,
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
                  "UFS source changed after preflight; refusing source writes");
        return -1;
    }
    return 0;
}

static int safe_commit_stage(const char *stage_path, const char *target_path,
                             const UFSJournal *state, uint64_t *written,
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
                  "cannot open UFS stage or target for commit: %s",
                  strerror(errno));
        return -1;
    }
    if (flock(target, LOCK_EX | LOCK_NB) != 0) {
        txn_error(error, error_size,
                  "cannot lock UFS target for commit: %s", strerror(errno));
        (void)close(stage); (void)close(target);
        return -1;
    }
    uint8_t *buffer = malloc(UFS_TXN_IO_BYTES);
    if (buffer == NULL) {
        txn_error(error, error_size, "out of memory committing UFS stage");
        (void)flock(target, LOCK_UN); (void)close(stage); (void)close(target);
        return -1;
    }
    int rc = 0;
    uint64_t total = 0U;
    for (uint64_t offset = 0U; offset < state->filesystem_bytes;) {
        if (ld_stop_requested()) {
            rc = fsync(target) == 0 ? UFS_TXN_STOPPED : -1;
            if (rc < 0)
                txn_error(error, error_size,
                          "cannot sync UFS target at Stop boundary: %s",
                          strerror(errno));
            break;
        }
        const uint64_t remaining = state->filesystem_bytes - offset;
        const size_t count = remaining > UFS_TXN_IO_BYTES
                           ? UFS_TXN_IO_BYTES : (size_t)remaining;
        if (ld_pread_full(stage, buffer, count, offset) != (ssize_t)count ||
            ld_pwrite_full(target, buffer, count, offset) != (ssize_t)count) {
            txn_error(error, error_size,
                      "short I/O committing UFS bytes at offset %" PRIu64,
                      offset);
            rc = -1;
            break;
        }
        offset += count; total += count;
    }
    if (rc == 0 && fsync(target) != 0) {
        txn_error(error, error_size,
                  "cannot sync UFS target: %s", strerror(errno));
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
    UFSJournal state;
    if (journal_load(journal, &state, error, error_size) != 0)
        return 1;
    if (!ld_path_is_derived_from(state.stage, journal, UFS_STAGE_SUFFIX) ||
        !valid_operation(state.operation) || !valid_phase(state.phase)) {
        txn_error(error, error_size,
                  "UFS recovery journal has an invalid stage binding, operation or phase");
        journal_free(&state);
        return 1;
    }
    const int identity_rc = strcmp(state.phase, "staged") == 0
        ? check_source_unchanged(device, &state, error, error_size)
        : check_target_identity(device, &state, error, error_size);
    if (identity_rc != 0) {
        journal_free(&state);
        return identity_rc == UFS_TXN_STOPPED ? UFS_TXN_STOPPED : 1;
    }
    char digest[65];
    const int hash_rc = hash_prefix(state.stage, state.filesystem_bytes, true,
                                    digest, error, error_size);
    if (hash_rc != 0 || strcmp(digest, state.stage_sha256) != 0) {
        if (hash_rc == 0)
            txn_error(error, error_size,
                      "UFS recovery stage SHA-256 does not match the journal");
        journal_free(&state);
        return hash_rc == UFS_TXN_STOPPED ? UFS_TXN_STOPPED : 1;
    }
    const bool growth = strcmp(state.operation, "growth-defrag") == 0;
    if (ufs_verify_layout(state.stage, growth, 10U,
                            error, error_size) != 0) {
        journal_free(&state);
        return 1;
    }
    if (strcmp(state.phase, "committed") == 0) {
        if (ufs_verify_layout(device, growth, 10U,
                                error, error_size) != 0) {
            journal_free(&state);
            return 1;
        }
        transaction_cleanup(journal, &state);
        ld_emit_result_event(stdout, "recover", "completed",
                             "Verified an already committed UFS transaction.");
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
    if (commit_rc == UFS_TXN_STOPPED) {
        ld_emit_result_event(stdout, "recover", "stopped",
                             "Recovery stopped at a durable boundary and can be resumed.");
        journal_free(&state);
        return UFS_TXN_STOPPED;
    }
    if (commit_rc != 0 ||
        ufs_verify_layout(state.device, growth, 10U,
                            error, error_size) != 0 ||
        journal_phase(journal, &state, "committed",
                      error, error_size) != 0) {
        journal_free(&state);
        return 1;
    }
    transaction_cleanup(journal, &state);
    if (live) {
        (void)printf(
            "@@LIVE_RESET {\"reason\":\"authoritative post-recovery UFS map\"}\n");
        (void)fflush(stdout);
    }
    (void)printf("Recovered verified UFS source; committed %" PRIu64 " KiB.\n",
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
    if (argc == 5 && strcmp(argv[1], "map") == 0 &&
        strcmp(argv[3], "--cells") == 0) {
        uint64_t requested_cells = 0U;
        if (parse_cells(argv[4], &requested_cells) != 0) {
            (void)fprintf(stderr, "%s: invalid cell count\n", PROG);
            return 2;
        }
        LdUfsSummary summary;
        char error[512] = {0};
        if (ufs_read_summary(argv[2], &summary, error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "%s: %s\n", PROG, error);
            return 1;
        }
        if (exact_ufs_ready(&summary))
            return print_exact_map(argv[2], requested_cells);
        uint64_t size_bytes = 0U;
        if (device_size_bytes(argv[2], &size_bytes) != 0) {
            (void)fprintf(stderr, "%s: cannot determine device size: %s\n",
                          PROG, strerror(errno));
            return 1;
        }
        print_summary_map(&summary, size_bytes, requested_cells);
        return 0;
    }
    if (argc == 3 &&
        (strcmp(argv[1], "identify") == 0 ||
         strcmp(argv[1], "analyse-json") == 0)) {
        LdUfsSummary summary;
        char error[512] = {0};
        if (ufs_read_summary(argv[2], &summary, error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "%s: %s\n", PROG, error);
            return 1;
        }
        if (strcmp(argv[1], "identify") == 0) {
            print_summary_json(&summary, 0);
            return 0;
        }
        if (exact_ufs_ready(&summary)) {
            LdUfsAnalysis analysis;
            if (ufs_analyse_allocation(argv[2], &analysis, NULL, 0U,
                                       error, sizeof(error)) != 0) {
                (void)fprintf(stderr, "%s: %s\n", PROG, error);
                return 1;
            }
            print_exact_analysis_json(&analysis);
        } else {
            print_summary_json(&summary, 1);
        }
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
            (void)fprintf(stderr, "%s: unknown or incomplete UFS option: %s\n",
                          PROG, argv[index]);
            return 2;
        }
    }
    if (!write || confirm == NULL || journal == NULL ||
        strcmp(confirm, device) != 0) {
        (void)fprintf(stderr,
                      "%s: UFS mutation requires --write --confirm DEVICE --journal PATH\n",
                      PROG);
        return 2;
    }
    if (growth && growth_percent != 10U) {
        (void)fprintf(stderr,
                      "%s: UFS Growth Defrag requires exactly 10 percent reserve\n",
                      PROG);
        return 2;
    }
    if (ld_path_is_mounted(device)) {
        (void)fprintf(stderr,
                      "%s: UFS target is mounted; raw mutation and recovery require an unmounted filesystem\n",
                      PROG);
        return 1;
    }

    ld_stop_clear();
    ld_stop_install_handlers();
    char error[512] = {0};
    if (recover) {
        const int result = recover_transaction(
            device, journal, live, error, sizeof(error));
        if (result != 0 && result != UFS_TXN_STOPPED)
            (void)fprintf(stderr, "%s: %s\n", PROG,
                          error[0] != '\0' ? error : "UFS recovery failed");
        return result;
    }

    if (ufs_verify_layout(device, growth, growth_percent,
                          error, sizeof(error)) == 0) {
        ld_emit_result_event(
            stdout, mode, "not-needed",
            growth
                ? "Canonical UFS layout with exact 10% post-file reserve already verified."
                : "Canonical packed UFS layout already verified.");
        return 0;
    }
    error[0] = '\0';

    UFSJournal state;
    memset(&state, 0, sizeof(state));
    infiltratr_copy_string(state.operation, sizeof(state.operation), mode);
    const int capture_result =
        capture_target(device, &state, error, sizeof(error));
    if (capture_result != 0) {
        if (capture_result == UFS_TXN_STOPPED)
            ld_emit_result_event(stdout, mode, "stopped",
                                 "Stopped during read-only UFS preflight.");
        goto fail;
    }
    state.stage = stage_name(journal);
    if (state.stage == NULL) {
        txn_error(error, sizeof(error), "out of memory creating UFS stage path");
        goto fail;
    }
    if (access(journal, F_OK) == 0 || access(state.stage, F_OK) == 0) {
        txn_error(error, sizeof(error),
                  "existing UFS recovery artifacts must be recovered or removed before starting");
        goto fail;
    }
    char *parent = ld_path_parent_directory(journal);
    if (parent == NULL || ld_path_ensure_trusted_directory_tree(parent) != 0) {
        txn_error(error, sizeof(error),
                  "cannot create UFS recovery directory: %s", strerror(errno));
        free(parent);
        goto fail;
    }
    free(parent);

    (void)printf("Starting native C UFS %s on %s.\n",
                 growth ? "Growth Defrag" : "Defrag", device);
    uint64_t planned = 0U;
    const int stage_result =
        ufs_build_stage(state.device, state.stage, growth, growth_percent,
                        live, &planned, error, sizeof(error));
    if (stage_result != 0 ||
        ufs_verify_layout(state.stage, growth, growth_percent,
                          error, sizeof(error)) != 0) {
        unlink_if_exists(state.stage);
        if (stage_result == UFS_TXN_STOPPED) {
            ld_emit_result_event(stdout, mode, "stopped",
                                 "Stopped before any UFS source writes.");
            journal_free(&state);
            return UFS_TXN_STOPPED;
        }
        goto fail;
    }
    const int unchanged =
        check_source_unchanged(state.device, &state, error, sizeof(error));
    if (unchanged != 0) {
        unlink_if_exists(state.stage);
        if (unchanged == UFS_TXN_STOPPED) {
            ld_emit_result_event(stdout, mode, "stopped",
                                 "Stopped before any UFS source writes.");
            journal_free(&state);
            return UFS_TXN_STOPPED;
        }
        goto fail;
    }
    const int hash_result =
        hash_prefix(state.stage, state.filesystem_bytes, true,
                    state.stage_sha256, error, sizeof(error));
    if (hash_result != 0) {
        unlink_if_exists(state.stage);
        if (hash_result == UFS_TXN_STOPPED) {
            ld_emit_result_event(stdout, mode, "stopped",
                                 "Stopped before any UFS source writes.");
            journal_free(&state);
            return UFS_TXN_STOPPED;
        }
        goto fail;
    }
    infiltratr_copy_string(state.phase, sizeof(state.phase), "staged");
    if (journal_save(journal, &state, error, sizeof(error)) != 0)
        goto fail;
    if (journal_phase(journal, &state, "committing",
                      error, sizeof(error)) != 0)
        goto fail;

    (void)printf("UFS source commit: replaying %" PRIu64
                 " KiB from the verified transaction stage.\n",
                 planned / 1024U);
    (void)fflush(stdout);
    uint64_t written = 0U;
    const int commit_result =
        safe_commit_stage(state.stage, state.device, &state, &written,
                          error, sizeof(error));
    if (commit_result == UFS_TXN_STOPPED) {
        ld_emit_result_event(stdout, mode, "stopped",
                             "Run Recover to resume the verified UFS transaction.");
        journal_free(&state);
        return UFS_TXN_STOPPED;
    }
    if (commit_result != 0 ||
        ufs_verify_layout(state.device, growth, growth_percent,
                          error, sizeof(error)) != 0)
        goto fail;
    if (journal_phase(journal, &state, "committed",
                      error, sizeof(error)) != 0)
        goto fail;
    transaction_cleanup(journal, &state);
    if (live) {
        (void)printf(
            "@@LIVE_RESET {\"reason\":\"authoritative post-commit UFS map\"}\n");
        (void)fflush(stdout);
    }
    (void)printf("UFS %s completed; committed %" PRIu64 " KiB.\n",
                 growth ? "Growth Defrag" : "Defrag", written / 1024U);
    ld_emit_result_event(stdout, mode, "completed", "");
    journal_free(&state);
    return 0;

fail:
    (void)fprintf(stderr, "%s: %s\n", PROG,
                  error[0] != '\0' ? error : "UFS transaction failed");
    journal_free(&state);
    return 1;
}
