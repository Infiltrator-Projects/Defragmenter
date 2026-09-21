// SPDX-License-Identifier: GPL-3.0-or-later
#include "exfat_native.h"

#include "ld_device.h"
#include "ld_io.h"
#include "ld_runtime.h"
#include "ld_protocol.h"
#include "ld_path.h"

#include "infiltratr/core.h"
#include "infiltratr/config.h"
#include "infiltratr/posix.h"
#include "infiltratr/quantity.h"
#include "ld_stop.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define PROGRAM_NAME "linux-defragger-exfat-worker"
#define JOURNAL_MAGIC "LINUX-DEFRAGGER-EXFAT-JOURNAL-2"
#define COPY_CHUNK (4U * 1024U * 1024U)
#define JOURNAL_INTERVAL (64U * 1024U * 1024U)

typedef struct {
    char *device;
    char *target_identity;
    char *stage;
    char operation[24];
    char phase[32];
    char stage_sha256[65];
    uint32_t serial;
    uint64_t physical_bytes;
    uint64_t filesystem_bytes;
    uint64_t commit_offset;
    uint64_t boot_length;
} ExfatJournal;

static void usage(FILE *stream) {
    fprintf(stream,
            "Usage:\n"
            "  %s --version\n"
            "  %s identify DEVICE\n"
            "  %s analyse-json DEVICE\n"
            "  %s defrag|growth-defrag|recover DEVICE --write --confirm DEVICE --journal PATH [--live-updates]\n",
            PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME);
}





static int ensure_directory_tree(const char *path, char **error) {
    if (ld_path_ensure_trusted_directory_tree(path) != 0) {
        exfat_set_error(error, "cannot create exFAT journal directory %s: %s", path, strerror(errno));
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


static void journal_free(ExfatJournal *state) {
    if (state == NULL) return;
    free(state->device); free(state->target_identity); free(state->stage); memset(state, 0, sizeof(*state));
}

static bool safe_value(const char *value) {
    return value != NULL && strchr(value, '\n') == NULL && strchr(value, '\r') == NULL && strchr(value, '=') == NULL;
}

static bool journal_write_stream(FILE *file, const void *user_data) {
    const ExfatJournal *state = user_data;
    fprintf(file, "%s\n", JOURNAL_MAGIC);
    fprintf(file, "device=%s\n", state->device);
    fprintf(file, "target_identity=%s\n", state->target_identity);
    fprintf(file, "stage=%s\n", state->stage);
    fprintf(file, "operation=%s\n", state->operation);
    fprintf(file, "phase=%s\n", state->phase);
    fprintf(file, "stage_sha256=%s\n", state->stage_sha256);
    fprintf(file, "serial=%u\n", state->serial);
    fprintf(file, "physical_bytes=%" PRIu64 "\n", state->physical_bytes);
    fprintf(file, "filesystem_bytes=%" PRIu64 "\n", state->filesystem_bytes);
    fprintf(file, "commit_offset=%" PRIu64 "\n", state->commit_offset);
    fprintf(file, "boot_length=%" PRIu64 "\n", state->boot_length);
    return !ferror(file);
}

static int journal_save(const char *path, const ExfatJournal *state, char **error) {
    if (!safe_value(state->device) || !safe_value(state->target_identity) ||
        !safe_value(state->stage)) {
        exfat_set_error(error, "exFAT transaction paths contain unsupported journal characters");
        return -1;
    }
    char *parent = ld_path_parent_directory(path);
    if (ensure_directory_tree(parent, error) != 0) {
        free(parent);
        return -1;
    }
    free(parent);
    const int failure = infiltratr_atomic_file_write(
        path, INFILTRATR_ATOMIC_FILE_PRIVATE, journal_write_stream, state);
    if (failure != 0) {
        exfat_set_error(error, "cannot publish exFAT journal: %s", strerror(failure));
        return -1;
    }
    return 0;
}

static int parse_u64(const char *text, uint64_t *value) {
    return infiltratr_parse_u64(text, 10U, value) ? 0 : -1;
}

static int journal_load(const char *path, ExfatJournal *state, char **error) {
    memset(state, 0, sizeof(*state)); FILE *file = fopen(path, "r");
    if (file == NULL) { exfat_set_error(error, "cannot open exFAT recovery journal: %s", strerror(errno)); return -1; }
    char *line = NULL; size_t capacity = 0;
    if (getline(&line, &capacity, file) < 0) goto invalid;
    infiltratr_trim_line_end(line); if (strcmp(line, JOURNAL_MAGIC) != 0) goto invalid;
    while (getline(&line, &capacity, file) >= 0) {
        char *key = NULL;
        char *equals = NULL;
        if (infiltratr_config_parse_line(line, &key, &equals) !=
            INFILTRATR_CONFIG_LINE_ENTRY)
            goto invalid;
        if (strcmp(key, "device") == 0) { free(state->device); state->device = ld_xstrdup(equals); }
        else if (strcmp(key, "target_identity") == 0) { free(state->target_identity); state->target_identity = ld_xstrdup(equals); }
        else if (strcmp(key, "stage") == 0) { free(state->stage); state->stage = ld_xstrdup(equals); }
        else if (strcmp(key, "operation") == 0) infiltratr_copy_string(state->operation, sizeof(state->operation), equals);
        else if (strcmp(key, "phase") == 0) infiltratr_copy_string(state->phase, sizeof(state->phase), equals);
        else if (strcmp(key, "stage_sha256") == 0) infiltratr_copy_string(state->stage_sha256, sizeof(state->stage_sha256), equals);
        else if (strcmp(key, "serial") == 0) { uint64_t value; if (!infiltratr_parse_u64_range(equals, 10U, 0U, UINT32_MAX, &value)) goto invalid; state->serial = (uint32_t)value; }
        else if (strcmp(key, "physical_bytes") == 0 && parse_u64(equals, &state->physical_bytes) != 0) goto invalid;
        else if (strcmp(key, "filesystem_bytes") == 0 && parse_u64(equals, &state->filesystem_bytes) != 0) goto invalid;
        else if (strcmp(key, "commit_offset") == 0 && parse_u64(equals, &state->commit_offset) != 0) goto invalid;
        else if (strcmp(key, "boot_length") == 0 && parse_u64(equals, &state->boot_length) != 0) goto invalid;
    }
    free(line); fclose(file);
    if (state->device == NULL || state->target_identity == NULL || state->stage == NULL || state->operation[0] == '\0' ||
        state->phase[0] == '\0' || state->physical_bytes == 0 || state->filesystem_bytes == 0 || state->boot_length == 0) goto invalid_state;
    return 0;
invalid:
    free(line); fclose(file);
invalid_state:
    journal_free(state); exfat_set_error(error, "exFAT recovery journal is malformed or incomplete"); return -1;
}

static int journal_phase(const char *path, ExfatJournal *state, const char *phase, char **error) {
    infiltratr_copy_string(state->phase, sizeof(state->phase), phase); return journal_save(path, state, error);
}

static void transaction_cleanup(const char *journal, const ExfatJournal *state) {
    if (state != NULL) unlink_if_exists(state->stage);
    unlink_if_exists(journal);
}

static int hash_file(const char *path, uint64_t length, char output[65], char **error) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) { exfat_set_error(error, "cannot open exFAT stage for hashing: %s", strerror(errno)); return -1; }
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (context == NULL || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) { close(fd); EVP_MD_CTX_free(context); exfat_set_error(error, "initialising stage SHA-256 failed"); return -1; }
    uint8_t *buffer = ld_xmalloc(COPY_CHUNK); uint64_t offset = 0;
    while (offset < length) {
        size_t chunk = (size_t)((length - offset) < COPY_CHUNK ? (length - offset) : COPY_CHUNK);
        ssize_t got = ld_pread_full(fd, buffer, chunk, offset);
        if (got < 0 || (size_t)got != chunk || EVP_DigestUpdate(context, buffer, chunk) != 1) {
            free(buffer); close(fd); EVP_MD_CTX_free(context); exfat_set_error(error, "hashing exFAT stage failed"); return -1;
        }
        offset += chunk;
    }
    free(buffer); close(fd); uint8_t digest[32]; unsigned int digest_length = 0;
    if (EVP_DigestFinal_ex(context, digest, &digest_length) != 1 || digest_length != 32U) { EVP_MD_CTX_free(context); exfat_set_error(error, "finalising exFAT stage hash failed"); return -1; }
    EVP_MD_CTX_free(context); static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32U; ++i) { output[i * 2U] = digits[digest[i] >> 4]; output[i * 2U + 1U] = digits[digest[i] & 15U]; }
    output[64] = '\0'; return 0;
}

static bool canonical_layout(const ExfatCatalogue *catalogue, const ExfatPlan *plan) {
    if (catalogue->bitmap_length != plan->bitmap_length || memcmp(catalogue->bitmap, plan->expected_bitmap, plan->bitmap_length) != 0) return false;
    for (size_t i = 0; i < catalogue->objects.count; ++i) {
        const ExfatObject *object = &catalogue->objects.items[i];
        for (size_t j = 0; j < object->clusters.count; ++j)
            if (object->clusters.items[j] != object->target_start + (uint32_t)j) return false;
        if (plan->reserve_percent != 0U && object->regular_file) {
            uint32_t start = object->target_start + (uint32_t)object->clusters.count;
            for (uint32_t r = 0; r < object->reserve_clusters; ++r) if (exfat_allocated(catalogue, start + r)) return false;
        }
    }
    return true;
}

static int capacity_preflight(const char *journal_path, const ExfatVolume *volume,
                              const ExfatPlan *plan, char **error) {
    char *parent = ld_path_parent_directory(journal_path); struct statvfs fs;
    if (statvfs(parent, &fs) != 0) { exfat_set_error(error, "cannot inspect exFAT journal filesystem capacity: %s", strerror(errno)); free(parent); return -1; }
    free(parent); uint64_t available = (uint64_t)fs.f_bavail * (uint64_t)fs.f_frsize;
    uint64_t allocated = 0; for (size_t i = 0; i < plan->bitmap_length; ++i) allocated += (uint64_t)__builtin_popcount((unsigned int)plan->expected_bitmap[i]);
    uint64_t required = volume->heap_offset + allocated * volume->cluster_size + 64U * 1024U * 1024U;
    if (available < required) { exfat_set_error(error, "not enough free space beside the journal for the verified exFAT working image"); return -1; }
    return 0;
}

static int compare_region(int left, int right, uint64_t start, uint64_t end, char **error) {
    uint8_t *a = ld_xmalloc(COPY_CHUNK), *b = ld_xmalloc(COPY_CHUNK); uint64_t offset = start;
    while (offset < end) {
        size_t chunk = (size_t)((end - offset) < COPY_CHUNK ? (end - offset) : COPY_CHUNK);
        if (ld_pread_full(left, a, chunk, offset) != (ssize_t)chunk || ld_pread_full(right, b, chunk, offset) != (ssize_t)chunk || memcmp(a, b, chunk) != 0) {
            free(a); free(b); exfat_set_error(error, "persistent exFAT source differs from verified stage at byte %" PRIu64, offset); return -1;
        }
        offset += chunk;
    }
    free(a); free(b); return 0;
}

static uint64_t allocated_cluster_count(const ExfatVolume *volume,
                                        const ExfatCatalogue *catalogue) {
    uint64_t count = 0U;
    for (uint32_t cluster = 2U; cluster < volume->cluster_count + 2U; ++cluster) {
        if (exfat_allocated(catalogue, cluster)) count++;
    }
    return count;
}

static uint64_t committed_body_bytes_before(const ExfatVolume *volume,
                                            const ExfatCatalogue *catalogue,
                                            uint64_t absolute_offset) {
    uint64_t complete = 0U;
    uint64_t fat_start = volume->fat_offset;
    uint64_t fat_end = fat_start + volume->fat_length;
    if (absolute_offset > fat_start) {
        uint64_t upto = absolute_offset < fat_end ? absolute_offset : fat_end;
        if (upto > fat_start) complete += upto - fat_start;
    }
    uint32_t cluster = 2U;
    while (cluster < volume->cluster_count + 2U) {
        if (!exfat_allocated(catalogue, cluster)) { cluster++; continue; }
        uint32_t first = cluster;
        while (cluster < volume->cluster_count + 2U && exfat_allocated(catalogue, cluster)) cluster++;
        uint64_t start = exfat_cluster_offset(volume, first);
        uint64_t end = exfat_cluster_offset(volume, cluster);
        if (absolute_offset <= start) break;
        uint64_t upto = absolute_offset < end ? absolute_offset : end;
        if (upto > start) complete += upto - start;
        if (absolute_offset < end) break;
    }
    return complete;
}

static int commit_one_range(int stage_fd, int target_fd,
                            uint64_t start, uint64_t end, uint64_t resume_offset,
                            const char *journal_path, ExfatJournal *state,
                            uint8_t *buffer, uint64_t total_bytes,
                            uint64_t *completed_bytes, uint64_t *journal_bytes,
                            bool *stop_announced, char **error) {
    if (end <= resume_offset || end <= start) return 0;
    uint64_t offset = start < resume_offset ? resume_offset : start;
    while (offset < end) {
        size_t chunk = (size_t)((end - offset) < COPY_CHUNK ? (end - offset) : COPY_CHUNK);
        if (ld_pread_full(stage_fd, buffer, chunk, offset) != (ssize_t)chunk ||
            ld_pwrite_full(target_fd, buffer, chunk, offset) != (ssize_t)chunk) {
            exfat_set_error(error, "short exFAT allocated-range commit at byte %" PRIu64, offset);
            return -1;
        }
        offset += chunk;
        *completed_bytes += chunk;
        *journal_bytes += chunk;
        state->commit_offset = offset;
        if (*journal_bytes >= JOURNAL_INTERVAL) {
            if (fsync(target_fd) != 0 || journal_save(journal_path, state, error) != 0) return -1;
            *journal_bytes = 0U;
            if (total_bytes != 0U) {
                printf("%.2f percent completed\n",
                       100.0 * (double)(*completed_bytes) / (double)total_bytes);
                fflush(stdout);
            }
        }
        if (ld_stop_requested() && !*stop_announced) {
            puts("Stop requested after exFAT commit began; completing the verified allocated-range commit so the target is never left partially written.");
            fflush(stdout);
            *stop_announced = true;
        }
    }
    return 0;
}

static int verify_commit_ranges(int target_fd, int stage_fd,
                                const ExfatVolume *volume,
                                const ExfatCatalogue *catalogue,
                                char **error) {
    if (compare_region(target_fd, stage_fd, volume->fat_offset,
                       volume->fat_offset + volume->fat_length, error) != 0) return -1;
    uint32_t cluster = 2U;
    while (cluster < volume->cluster_count + 2U) {
        if (!exfat_allocated(catalogue, cluster)) { cluster++; continue; }
        uint32_t first = cluster;
        while (cluster < volume->cluster_count + 2U && exfat_allocated(catalogue, cluster)) cluster++;
        uint64_t start = exfat_cluster_offset(volume, first);
        uint64_t end = exfat_cluster_offset(volume, cluster);
        if (compare_region(target_fd, stage_fd, start, end, error) != 0) return -1;
    }
    return 0;
}

static int commit_stage(const char *device, const char *journal_path, ExfatJournal *state,
                        bool recovery, char **error) {
    ExfatVolume target;
    if (exfat_open_volume(device, true, recovery, &target, error) != 0) return -1;
    if (!ld_fd_matches_identity(target.fd, state->target_identity,
                                state->physical_bytes) ||
        target.serial != state->serial ||
        target.volume_bytes != state->filesystem_bytes) {
        exfat_close_volume(&target);
        exfat_set_error(error,
                        "exFAT writable descriptor does not match the journaled target");
        return -1;
    }

    ExfatVolume stage_volume;
    ExfatCatalogue stage_catalogue;
    if (exfat_scan(state->stage, false, &stage_volume, &stage_catalogue, error) != 0) {
        exfat_close_volume(&target); return -1;
    }
    if (stage_volume.serial != state->serial ||
        stage_volume.volume_bytes != state->filesystem_bytes ||
        stage_volume.cluster_size != target.cluster_size ||
        stage_volume.cluster_count != target.cluster_count ||
        stage_volume.fat_offset != target.fat_offset ||
        stage_volume.fat_length != target.fat_length ||
        stage_volume.heap_offset != target.heap_offset) {
        exfat_catalogue_free(&stage_catalogue); exfat_close_volume(&stage_volume);
        exfat_close_volume(&target); exfat_set_error(error, "verified exFAT stage geometry changed before commit"); return -1;
    }

    uint8_t *dirty_boot = NULL; size_t dirty_length = 0;
    if (exfat_set_stage_boot_dirty(state->stage, true, &dirty_boot, &dirty_length, error) != 0) {
        exfat_catalogue_free(&stage_catalogue); exfat_close_volume(&stage_volume); exfat_close_volume(&target); return -1;
    }
    if (dirty_length != state->boot_length ||
        ld_pwrite_full(target.fd, dirty_boot, dirty_length, 0) != (ssize_t)dirty_length ||
        fsync(target.fd) != 0) {
        free(dirty_boot); exfat_catalogue_free(&stage_catalogue); exfat_close_volume(&stage_volume);
        exfat_close_volume(&target); exfat_set_error(error, "cannot publish dirty exFAT transaction boot region"); return -1;
    }
    free(dirty_boot);

    uint64_t allocated_clusters = allocated_cluster_count(&stage_volume, &stage_catalogue);
    uint64_t total_bytes = stage_volume.fat_length + allocated_clusters * (uint64_t)stage_volume.cluster_size;
    uint64_t resume_offset = state->commit_offset < state->boot_length ? state->boot_length : state->commit_offset;
    uint64_t completed_bytes = committed_body_bytes_before(&stage_volume, &stage_catalogue, resume_offset);
    uint64_t journal_bytes = 0U;
    uint8_t *buffer = ld_xmalloc(COPY_CHUNK);
    bool stop_announced = false;
    int result = 0;

    printf("exFAT source commit: writing %" PRIu64
           " MB of verified allocated/metadata ranges instead of rewriting the full %" PRIu64
           " MB filesystem.\n",
           total_bytes / (1024U * 1024U), state->filesystem_bytes / (1024U * 1024U));
    fflush(stdout);

    if (commit_one_range(stage_volume.fd, target.fd,
                         stage_volume.fat_offset, stage_volume.fat_offset + stage_volume.fat_length,
                         resume_offset, journal_path, state, buffer, total_bytes,
                         &completed_bytes, &journal_bytes, &stop_announced, error) != 0) result = -1;

    uint32_t cluster = 2U;
    while (result == 0 && cluster < stage_volume.cluster_count + 2U) {
        if (!exfat_allocated(&stage_catalogue, cluster)) { cluster++; continue; }
        uint32_t first = cluster;
        while (cluster < stage_volume.cluster_count + 2U && exfat_allocated(&stage_catalogue, cluster)) cluster++;
        uint64_t start = exfat_cluster_offset(&stage_volume, first);
        uint64_t end = exfat_cluster_offset(&stage_volume, cluster);
        if (commit_one_range(stage_volume.fd, target.fd, start, end, resume_offset,
                             journal_path, state, buffer, total_bytes,
                             &completed_bytes, &journal_bytes, &stop_announced, error) != 0) result = -1;
    }

    if (result == 0 && fsync(target.fd) != 0) {
        exfat_set_error(error, "cannot sync completed exFAT allocated-range commit"); result = -1;
    }
    if (result == 0 && verify_commit_ranges(target.fd, stage_volume.fd,
                                             &stage_volume, &stage_catalogue, error) != 0) result = -1;

    if (result == 0) {
        uint8_t *clean_boot = ld_xmalloc((size_t)state->boot_length);
        if (ld_pread_full(stage_volume.fd, clean_boot, (size_t)state->boot_length, 0) != (ssize_t)state->boot_length ||
            ld_pwrite_full(target.fd, clean_boot, (size_t)state->boot_length, 0) != (ssize_t)state->boot_length ||
            fsync(target.fd) != 0) {
            exfat_set_error(error, "cannot publish clean exFAT boot region"); result = -1;
        }
        free(clean_boot);
    }
    if (result == 0) {
        state->commit_offset = state->filesystem_bytes;
        if (journal_save(journal_path, state, error) != 0) result = -1;
        else { puts("100.00 percent completed"); fflush(stdout); }
    }

    free(buffer);
    exfat_catalogue_free(&stage_catalogue); exfat_close_volume(&stage_volume); exfat_close_volume(&target);
    return result;
}

static void emit_bitmap_ranges(const ExfatCatalogue *catalogue, uint32_t clusters, bool free_state) {
    putchar('['); bool first = true; uint32_t i = 0;
    while (i < clusters) {
        bool state = exfat_allocated(catalogue, i + 2U); if (free_state ? state : !state) { i++; continue; }
        uint32_t start = i; while (i < clusters) { bool s = exfat_allocated(catalogue, i + 2U); if (free_state ? s : !s) break; i++; }
        if (!first) putchar(',');
        printf("[%u,%u]", start, i);
        first = false;
    }
    putchar(']');
}

static void emit_object_ranges(const ExfatCatalogue *catalogue, bool directories, bool fragmented) {
    putchar('['); bool first = true;
    for (size_t i = 0; i < catalogue->objects.count; ++i) {
        const ExfatObject *object = &catalogue->objects.items[i];
        bool relevant = directories ? object->directory : object->regular_file;
        if (!relevant || (fragmented && exfat_fragments(&object->clusters) <= 1U)) continue;
        size_t position = 0;
        while (position < object->clusters.count) {
            uint32_t start = object->clusters.items[position] - 2U, end = start + 1U; position++;
            while (position < object->clusters.count && object->clusters.items[position] - 2U == end) { end++; position++; }
            if (!first) putchar(',');
            printf("[%u,%u]", start, end);
            first = false;
        }
    }
    putchar(']');
}

static void emit_live_reset(const ExfatVolume *volume, const ExfatCatalogue *catalogue) {
    printf("@@LIVE_RESET {\"unit_size\":%u,\"filesystem_units\":%u,\"used_ranges\":[",
           volume->cluster_size, volume->cluster_count);
    bool first = true;
    uint32_t cluster = 2U;
    while (cluster < volume->cluster_count + 2U) {
        if (!exfat_allocated(catalogue, cluster)) { cluster++; continue; }
        uint32_t start = cluster;
        while (cluster < volume->cluster_count + 2U && exfat_allocated(catalogue, cluster)) cluster++;
        if (!first) putchar(',');
        printf("[%" PRIu64 ",%" PRIu64 "]",
               (uint64_t)(start - 2U) * volume->cluster_size,
               (uint64_t)(cluster - start) * volume->cluster_size);
        first = false;
    }
    fputs("]}\n", stdout);
    fflush(stdout);
}

static int analyse_json(const char *device, char **error) {
    ExfatVolume volume; ExfatCatalogue catalogue;
    if (exfat_scan(device, true, &volume, &catalogue, error) != 0) return -1;
    double percent = infiltratr_percent_u64(
        catalogue.fragmented_files, catalogue.regular_files);
    printf("{\"filesystem\":\"exfat\",\"cluster_size\":%u,\"total_clusters\":%u,\"serial\":\"%08x\",", volume.cluster_size, volume.cluster_count, volume.serial);
    printf("\"regular_files\":%" PRIu64 ",\"directories\":%" PRIu64 ",\"fragmented_files\":%" PRIu64 ",\"fragmented_directories\":%" PRIu64 ",\"fragmentation_percent\":%.6f,\"growth_10_satisfied\":%s,\"free_ranges\":",
           catalogue.regular_files, catalogue.directories, catalogue.fragmented_files, catalogue.fragmented_directories, percent, catalogue.growth_10_satisfied ? "true" : "false");
    emit_bitmap_ranges(&catalogue, volume.cluster_count, true); fputs(",\"fragmented_ranges\":", stdout); emit_object_ranges(&catalogue, false, true);
    fputs(",\"directory_ranges\":", stdout); emit_object_ranges(&catalogue, true, false); fputs("}\n", stdout);
    exfat_catalogue_free(&catalogue); exfat_close_volume(&volume); return 0;
}

static int build_and_commit(const char *device, const char *operation, const char *journal_path,
                            size_t ram_bytes, size_t batch_clusters,
                            bool live_updates, char **error) {
    if (ld_path_is_mounted(device)) { exfat_set_error(error, "exFAT target is mounted; raw mutation requires an unmounted filesystem"); return 1; }
    if (access(journal_path, F_OK) == 0) { exfat_set_error(error, "an unfinished exFAT journal exists; run Recover first"); return 1; }
    bool growth = strcmp(operation, "growth-defrag") == 0;
    ExfatRelayoutStats relayout_stats;
    int relayout = exfat_relayout_in_place(device, journal_path,
                                           growth ? 10U : 0U,
                                           ram_bytes, batch_clusters,
                                           live_updates, &relayout_stats, error);
    if (relayout == EXFAT_RELAYOUT_COMPLETED) {
        ld_emit_result_event(stdout, operation, "completed", "");
        return 0;
    }
    if (relayout == EXFAT_RELAYOUT_NOT_NEEDED) {
        ld_emit_result_event(stdout, operation, "not-needed", "");
        return 0;
    }
    if (relayout == EXFAT_RELAYOUT_STOPPED) {
        ld_emit_result_event(stdout, operation, "stopped", "");
        return 130;
    }
    if (relayout == EXFAT_RELAYOUT_FAILED) return 1;
    fprintf(stderr,
            "exFAT terminal-workspace fast path is unavailable for this layout; using the verified shadow-image compatibility path.\n");
    char *real = NULL, *identity = NULL; uint64_t physical = 0;
    if (ld_device_capture_binding(device, &real, &identity, &physical) != 0) {
        exfat_set_error(error, "cannot bind exFAT target: %s", strerror(errno));
        free(real); free(identity); return 1;
    }
    ExfatVolume source; ExfatCatalogue catalogue; ExfatPlan plan; ExfatJournal state; memset(&state, 0, sizeof(state));
    if (exfat_scan(device, false, &source, &catalogue, error) != 0) { free(real); free(identity); return 1; }
    if (exfat_build_plan(&source, &catalogue, growth ? 10U : 0U, &plan, error) != 0) { exfat_catalogue_free(&catalogue); exfat_close_volume(&source); free(real); free(identity); return 1; }
    if (canonical_layout(&catalogue, &plan)) {
        puts(growth ? "Not needed; canonical exFAT layout with exact 10% growth reserves verified." : "Not needed; canonical packed exFAT layout verified.");
        ld_emit_result_event(stdout, operation, "not-needed", ""); exfat_plan_free(&plan); exfat_catalogue_free(&catalogue); exfat_close_volume(&source); free(real); free(identity); return 0;
    }
    state.device = ld_xstrdup(real); state.target_identity = ld_xstrdup(identity); state.stage = ld_path_append_suffix(journal_path, ".exfat-stage.img");
    snprintf(state.operation, sizeof(state.operation), "%s", operation); snprintf(state.phase, sizeof(state.phase), "prepared");
    state.serial = source.serial; state.physical_bytes = physical; state.filesystem_bytes = source.volume_bytes; state.boot_length = (uint64_t)source.bytes_per_sector * 24U;
    if (capacity_preflight(journal_path, &source, &plan, error) != 0 || journal_save(journal_path, &state, error) != 0) goto precommit_fail;
    printf("Raw userspace native-C exFAT engine %s\n", LD_VERSION); fflush(stdout);
    if (ld_stop_requested()) goto stopped;
    if (journal_phase(journal_path, &state, "building-stage", error) != 0 || exfat_build_stage(device, state.stage, &source, &catalogue, &plan, live_updates, error) != 0) goto precommit_fail;
    if (exfat_verify_stage(state.stage, &catalogue, &plan, false, error) != 0) goto precommit_fail;
    if (hash_file(state.stage, state.filesystem_bytes, state.stage_sha256, error) != 0) goto precommit_fail;
    if (journal_phase(journal_path, &state, "ready", error) != 0) goto precommit_fail;
    exfat_plan_free(&plan); exfat_catalogue_free(&catalogue); exfat_close_volume(&source);
    if (ld_stop_requested()) goto stopped_after_close;
    char *now_path = NULL, *now_identity = NULL; uint64_t now_size = 0; ExfatVolume now; ExfatCatalogue now_catalogue;
    if (ld_device_capture_binding(device, &now_path, &now_identity, &now_size) != 0 ||
        strcmp(now_path, state.device) != 0 || strcmp(now_identity, state.target_identity) != 0 || now_size != state.physical_bytes ||
        exfat_scan(device, false, &now, &now_catalogue, error) != 0 || now.serial != state.serial) {
        free(now_path); free(now_identity); if (error != NULL && *error == NULL) exfat_set_error(error, "exFAT target changed before commit"); goto precommit_fail_closed;
    }
    free(now_path); free(now_identity); exfat_catalogue_free(&now_catalogue); exfat_close_volume(&now);
    state.commit_offset = state.boot_length;
    if (journal_phase(journal_path, &state, "committing", error) != 0) goto precommit_fail_closed;
    puts("The internally verified native-C exFAT working image is complete. Starting the persistent source commit."); fflush(stdout);
    if (commit_stage(device, journal_path, &state, false, error) != 0) goto commit_fail;
    if (journal_phase(journal_path, &state, "verifying-source", error) != 0) goto commit_fail;
    ExfatVolume final_volume; ExfatCatalogue final_catalogue;
    if (exfat_scan(device, false, &final_volume, &final_catalogue, error) != 0) goto commit_fail;
    if (live_updates) emit_live_reset(&final_volume, &final_catalogue);
    exfat_catalogue_free(&final_catalogue); exfat_close_volume(&final_volume);
    transaction_cleanup(journal_path, &state);
    printf("exFAT %s completed with serial and full volume capacity preserved.\n", growth ? "Growth Defrag" : "Defragment");
    ld_emit_result_event(stdout, operation, ld_stop_requested() ? "stopped" : "completed", "");
    journal_free(&state); free(real); free(identity); return ld_stop_requested() ? 130 : 0;
stopped:
    exfat_plan_free(&plan); exfat_catalogue_free(&catalogue); exfat_close_volume(&source);
stopped_after_close:
    transaction_cleanup(journal_path, &state); puts("Stop requested before source commit; the original exFAT filesystem is unchanged."); ld_emit_result_event(stdout, operation, "stopped", "");
    journal_free(&state); free(real); free(identity); return 130;
precommit_fail:
    exfat_plan_free(&plan); exfat_catalogue_free(&catalogue); exfat_close_volume(&source);
precommit_fail_closed:
    transaction_cleanup(journal_path, &state);
commit_fail:
    journal_free(&state); free(real); free(identity); return 1;
}


static size_t parse_ram_bytes(const char *text) {
    if (strcmp(text, "auto") == 0) return ld_default_ram_limit();
    uint64_t bytes = 0U;
    if (!infiltratr_parse_binary_quantity_u64(text, &bytes) ||
        bytes == 0U || bytes > SIZE_MAX)
        return 0U;
    return (size_t)bytes;
}

static size_t parse_batch_clusters(const char *text) {
    uint64_t value = 0U;
    if (!infiltratr_parse_u64_range(text, 10U, 1U, (uint64_t)SIZE_MAX, &value))
        return 0U;
    return (size_t)value;
}

static int recover_transaction(const char *device, const char *journal_path,
                               size_t ram_bytes, size_t batch_clusters,
                               bool live_updates, char **error) {
    bool handled = false;
    int modern = exfat_relayout_recover(device, journal_path, ram_bytes,
                                        batch_clusters, live_updates,
                                        &handled, error);
    if (handled) return modern;
    ExfatJournal state; if (journal_load(journal_path, &state, error) != 0) return 1;
    if (!ld_path_is_derived_from(state.stage, journal_path, ".exfat-stage.img")) {
        exfat_set_error(error,
            "exFAT recovery stage is not derived from the selected journal path");
        journal_free(&state);
        return 1;
    }
    int result = 1; char *real = NULL, *identity = NULL; uint64_t size = 0;
    if (ld_device_capture_binding(device, &real, &identity, &size) != 0) {
        exfat_set_error(error, "cannot bind exFAT recovery target: %s", strerror(errno));
        goto done;
    }
    if (strcmp(real, state.device) != 0 || strcmp(identity, state.target_identity) != 0 || size != state.physical_bytes) { exfat_set_error(error, "recovery journal belongs to a different exFAT target"); goto done; }
    if (strcmp(state.phase, "committing") != 0 && strcmp(state.phase, "verifying-source") != 0) {
        transaction_cleanup(journal_path, &state); puts("Discarded an incomplete exFAT working image; the source was unchanged."); result = 0; goto done;
    }
    struct stat stage_status; if (stat(state.stage, &stage_status) != 0 || !S_ISREG(stage_status.st_mode) || (uint64_t)stage_status.st_size < state.filesystem_bytes) { exfat_set_error(error, "persistent exFAT working image is missing or truncated"); goto done; }
    char digest[65]; if (hash_file(state.stage, state.filesystem_bytes, digest, error) != 0 || strcmp(digest, state.stage_sha256) != 0) { if (error != NULL && *error == NULL) exfat_set_error(error, "persistent exFAT working image checksum changed"); goto done; }
    if (strcmp(state.phase, "committing") == 0) {
        printf("Resuming the exFAT source commit at byte %" PRIu64 ".\n", state.commit_offset); fflush(stdout);
        if (commit_stage(device, journal_path, &state, true, error) != 0) goto done;
        if (journal_phase(journal_path, &state, "verifying-source", error) != 0) goto done;
    }
    ExfatVolume final_volume; ExfatCatalogue final_catalogue;
    if (exfat_scan(device, false, &final_volume, &final_catalogue, error) != 0) goto done;
    exfat_catalogue_free(&final_catalogue); exfat_close_volume(&final_volume); transaction_cleanup(journal_path, &state);
    puts("exFAT recovery completed successfully."); ld_emit_result_event(stdout, "recover", "completed", ""); result = 0;
done:
    free(real); free(identity); journal_free(&state); return result;
}

int main(int argc, char **argv) {
    ld_runtime_set_program_name(PROGRAM_NAME);
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) { usage(stdout); return 0; }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) { puts(LD_VERSION); return 0; }
    if (argc < 3) { usage(stderr); return 2; }
    const char *operation = argv[1], *device = argv[2];
    if (strcmp(operation, "identify") == 0) {
        ExfatVolume volume; char *error = NULL; if (exfat_open_volume(device, false, true, &volume, &error) != 0) { free(error); return 1; }
        exfat_close_volume(&volume); puts("{\"filesystem\":\"exfat\"}"); return 0;
    }
    if (strcmp(operation, "analyse-json") == 0) {
        char *error = NULL; int status = analyse_json(device, &error); if (status != 0 && error != NULL) fprintf(stderr, "%s\n", error); free(error); return status == 0 ? 0 : 1;
    }
    if (strcmp(operation, "defrag") != 0 && strcmp(operation, "growth-defrag") != 0 && strcmp(operation, "recover") != 0) { usage(stderr); return 2; }
    const char *confirm = NULL, *journal = NULL; bool write = false, live_updates = false; int growth_percent = 10;
    size_t ram_bytes = ld_default_ram_limit();
    size_t batch_clusters = 0U;
    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--write") == 0) write = true;
        else if (strcmp(argv[i], "--live-updates") == 0) live_updates = true;
        else if (strcmp(argv[i], "--confirm") == 0 && i + 1 < argc) confirm = argv[++i];
        else if (strcmp(argv[i], "--journal") == 0 && i + 1 < argc) journal = argv[++i];
        else if (strcmp(argv[i], "--growth-percent") == 0 && i + 1 < argc) {
            uint64_t parsed = 0U;
            if (parse_u64(argv[++i], &parsed) != 0 || parsed > 100U) {
                fprintf(stderr, "%s: --growth-percent requires an integer from 0 to 100\n", PROGRAM_NAME);
                return 2;
            }
            growth_percent = (int)parsed;
        }
        else if (strcmp(argv[i], "--live-map-cells") == 0 && i + 1 < argc) {
            uint64_t parsed = 0U;
            if (parse_u64(argv[++i], &parsed) != 0) {
                fprintf(stderr, "%s: --live-map-cells requires a non-negative integer\n", PROGRAM_NAME);
                return 2;
            }
            live_updates = parsed > 0U;
        }
        else if (strcmp(argv[i], "--ram-buffer") == 0 && i + 1 < argc) { ram_bytes = parse_ram_bytes(argv[++i]); if (ram_bytes == 0U) { fprintf(stderr, "%s: invalid RAM buffer size\n", PROGRAM_NAME); return 2; } }
        else if (strcmp(argv[i], "--batch-clusters") == 0 && i + 1 < argc) { batch_clusters = parse_batch_clusters(argv[++i]); if (batch_clusters == 0U) { fprintf(stderr, "%s: invalid batch cluster count\n", PROGRAM_NAME); return 2; } }
        else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) { i++; }
        else { fprintf(stderr, "%s: unknown or incomplete option: %s\n", PROGRAM_NAME, argv[i]); return 2; }
    }
    if (!write || confirm == NULL || journal == NULL || strcmp(confirm, device) != 0) {
        fprintf(stderr, "%s: raw exFAT mutation requires --write --confirm DEVICE --journal PATH\n", PROGRAM_NAME); return 2;
    }
    if (strcmp(operation, "growth-defrag") == 0 && growth_percent != 10) {
        fprintf(stderr, "%s: Growth Defrag requires exactly 10%%\n", PROGRAM_NAME); return 2;
    }
    if (ld_path_is_mounted(device)) {
        fprintf(stderr,
            "%s: target is mounted; raw mutation and recovery require an unmounted filesystem\n",
            PROGRAM_NAME);
        return 1;
    }
    ld_stop_install_handlers(); char *error = NULL; int result = strcmp(operation, "recover") == 0
        ? recover_transaction(device, journal, ram_bytes, batch_clusters, live_updates, &error)
        : build_and_commit(device, operation, journal, ram_bytes, batch_clusters, live_updates, &error);
    if (result != 0 && result != 130) {
        const char *message = error == NULL ? "native exFAT operation failed" : error; fprintf(stderr, "%s\n", message); ld_emit_result_event(stdout, operation, "failed", message);
    }
    free(error); return result;
}
