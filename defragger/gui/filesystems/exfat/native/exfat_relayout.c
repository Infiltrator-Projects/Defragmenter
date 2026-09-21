// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * exFAT canonical in-place relayout engine.
 *
 * The fast path mirrors the FAT12/FAT16/FAT32 design: build one canonical
 * target plan, copy the complete live object set to a durable terminal safety
 * workspace, then place the final layout directly on the source device.  The
 * allocation bitmap and directory entry sets are rebuilt while placing, and
 * the FAT/boot metadata are committed last.  A small manifest is the only
 * external journal; no full-filesystem shadow image is created.
 */

#include "exfat_native.h"

#include "ld_device.h"
#include "ld_io.h"
#include "ld_path.h"
#include "ld_runtime.h"
#include "ld_stop.h"
#include "version.h"
#include "infiltratr/core.h"
#include "infiltratr/config.h"
#include "infiltratr/posix.h"
#include "infiltratr/token.h"

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
#include <sys/sysmacros.h>
#include <unistd.h>

#define EXFAT_RELAYOUT_MAGIC "LINUX-DEFRAGGER-EXFAT-RELAYOUT-1"
#define EXFAT_RELAYOUT_MAX_BUFFER (UINT64_C(64) * 1024 * 1024)

typedef struct {
    ExfatObjectKind kind;
    uint32_t target_start;
    uint32_t staged_start;
    uint32_t clusters;
    uint32_t reserve_clusters;
    uint64_t parent_index;
    uint64_t entry_offset;
    uint32_t entry_count;
    uint64_t system_entry_offset;
    uint64_t data_length;
    uint64_t valid_length;
    bool regular_file;
    bool directory;
} ExfatRelayoutRecord;

typedef struct {
    uint32_t serial;
    char target_identity[160];
    uint64_t device_size;
    uint64_t volume_bytes;
    uint32_t bytes_per_sector;
    uint32_t cluster_size;
    uint32_t cluster_count;
    uint64_t fat_offset;
    uint64_t fat_length;
    uint64_t heap_offset;
    uint32_t workspace_start;
    unsigned reserve_percent;
    ExfatRelayoutRecord *records;
    size_t record_count;
} ExfatRelayoutManifest;

typedef struct {
    size_t index;
    uint32_t target_start;
} PlacementIndex;

static int placement_index_compare(const void *left_ptr, const void *right_ptr) {
    const PlacementIndex *left = left_ptr;
    const PlacementIndex *right = right_ptr;
    if (left->target_start < right->target_start) return -1;
    if (left->target_start > right->target_start) return 1;
    if (left->index < right->index) return -1;
    if (left->index > right->index) return 1;
    return 0;
}

static void manifest_free(ExfatRelayoutManifest *manifest) {
    if (manifest == NULL) return;
    free(manifest->records);
    memset(manifest, 0, sizeof(*manifest));
}

static int ensure_directory_tree(const char *path, char **error) {
    if (ld_path_ensure_trusted_directory_tree(path) != 0) {
        exfat_set_error(error, "cannot create exFAT journal directory %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

static bool write_manifest_stream(FILE *file, const void *user_data) {
    const ExfatRelayoutManifest *manifest = user_data;
    fprintf(file, "%s\n", EXFAT_RELAYOUT_MAGIC);
    fprintf(file, "serial=%u\n", manifest->serial);
    fprintf(file, "target_identity=%s\n", manifest->target_identity);
    fprintf(file, "device_size=%" PRIu64 "\n", manifest->device_size);
    fprintf(file, "volume_bytes=%" PRIu64 "\n", manifest->volume_bytes);
    fprintf(file, "bytes_per_sector=%u\n", manifest->bytes_per_sector);
    fprintf(file, "cluster_size=%u\n", manifest->cluster_size);
    fprintf(file, "cluster_count=%u\n", manifest->cluster_count);
    fprintf(file, "fat_offset=%" PRIu64 "\n", manifest->fat_offset);
    fprintf(file, "fat_length=%" PRIu64 "\n", manifest->fat_length);
    fprintf(file, "heap_offset=%" PRIu64 "\n", manifest->heap_offset);
    fprintf(file, "workspace_start=%u\n", manifest->workspace_start);
    fprintf(file, "reserve_percent=%u\n", manifest->reserve_percent);
    fprintf(file, "object_count=%zu\n", manifest->record_count);
    for (size_t i = 0; i < manifest->record_count; ++i) {
        const ExfatRelayoutRecord *record = &manifest->records[i];
        fprintf(file,
                "record=%u,%u,%u,%u,%u,%" PRIu64 ",%" PRIu64 ",%u,%" PRIu64
                ",%" PRIu64 ",%" PRIu64 ",%u,%u\n",
                (unsigned)record->kind, record->target_start, record->staged_start,
                record->clusters, record->reserve_clusters, record->parent_index,
                record->entry_offset, record->entry_count,
                record->system_entry_offset, record->data_length,
                record->valid_length, record->regular_file ? 1U : 0U,
                record->directory ? 1U : 0U);
    }
    return !ferror(file);
}

static int save_manifest(const char *journal_path,
                         const ExfatRelayoutManifest *manifest,
                         char **error) {
    char *parent = ld_path_parent_directory(journal_path);
    if (ensure_directory_tree(parent, error) != 0) {
        free(parent);
        return -1;
    }
    free(parent);
    const int failure = infiltratr_atomic_file_write(
        journal_path, INFILTRATR_ATOMIC_FILE_PRIVATE,
        write_manifest_stream, manifest);
    if (failure != 0) {
        exfat_set_error(error, "cannot publish exFAT relayout journal: %s",
                        strerror(failure));
        return -1;
    }
    return 0;
}

static int remove_manifest(const char *journal_path, char **error) {
    const int failure = infiltratr_unlink_durable(journal_path, true);
    if (failure != 0) {
        exfat_set_error(error, "cannot durably remove exFAT relayout journal: %s",
                        strerror(failure));
        return -1;
    }
    return 0;
}

static bool parse_u64_text(const char *text, uint64_t *value) {
    return infiltratr_parse_u64(text, 10U, value);
}

static bool parse_record_tuple(const char *text, uint64_t values[13]) {
    const char *cursor = text;
    for (size_t index = 0U; index < 13U; ++index) {
        if (!infiltratr_parse_u64_token(&cursor, 10U, &values[index]))
            return false;
        if (index + 1U < 13U) {
            if (*cursor != ',') return false;
            cursor++;
        }
    }
    return *cursor == '\0';
}

static bool parse_u32_text(const char *text, uint32_t *value) {
    uint64_t parsed = 0;
    if (!parse_u64_text(text, &parsed) || parsed > UINT32_MAX) return false;
    *value = (uint32_t)parsed;
    return true;
}

static int load_manifest(const char *journal_path,
                         ExfatRelayoutManifest *manifest,
                         bool *handled,
                         char **error) {
    memset(manifest, 0, sizeof(*manifest));
    *handled = false;
    FILE *file = fopen(journal_path, "r");
    if (file == NULL) {
        if (errno == ENOENT) return 0;
        exfat_set_error(error, "cannot open exFAT recovery journal: %s",
                        strerror(errno));
        return -1;
    }
    char *line = NULL;
    size_t capacity = 0;
    if (getline(&line, &capacity, file) < 0) {
        free(line);
        fclose(file);
        return 0;
    }
    while (*line != '\0' && (line[strlen(line) - 1U] == '\n' ||
                             line[strlen(line) - 1U] == '\r')) {
        line[strlen(line) - 1U] = '\0';
    }
    if (strcmp(line, EXFAT_RELAYOUT_MAGIC) != 0) {
        free(line);
        fclose(file);
        return 0;
    }
    *handled = true;

    size_t records_seen = 0;
    while (getline(&line, &capacity, file) >= 0) {
        char *key = NULL;
        char *equals = NULL;
        if (infiltratr_config_parse_line(line, &key, &equals) !=
            INFILTRATR_CONFIG_LINE_ENTRY)
            goto malformed;
        if (strcmp(key, "serial") == 0) {
            if (!parse_u32_text(equals, &manifest->serial)) goto malformed;
        } else if (strcmp(key, "target_identity") == 0) {
            if (*equals == '\0' || strlen(equals) >= sizeof(manifest->target_identity) ||
                strchr(equals, '\n') != NULL || strchr(equals, '\r') != NULL)
                goto malformed;
            infiltratr_copy_string(manifest->target_identity,
                                   sizeof(manifest->target_identity), equals);
        } else if (strcmp(key, "device_size") == 0) {
            if (!parse_u64_text(equals, &manifest->device_size) ||
                manifest->device_size == 0U) goto malformed;
        } else if (strcmp(key, "volume_bytes") == 0) {
            if (!parse_u64_text(equals, &manifest->volume_bytes)) goto malformed;
        } else if (strcmp(key, "bytes_per_sector") == 0) {
            if (!parse_u32_text(equals, &manifest->bytes_per_sector)) goto malformed;
        } else if (strcmp(key, "cluster_size") == 0) {
            if (!parse_u32_text(equals, &manifest->cluster_size)) goto malformed;
        } else if (strcmp(key, "cluster_count") == 0) {
            if (!parse_u32_text(equals, &manifest->cluster_count)) goto malformed;
        } else if (strcmp(key, "fat_offset") == 0) {
            if (!parse_u64_text(equals, &manifest->fat_offset)) goto malformed;
        } else if (strcmp(key, "fat_length") == 0) {
            if (!parse_u64_text(equals, &manifest->fat_length)) goto malformed;
        } else if (strcmp(key, "heap_offset") == 0) {
            if (!parse_u64_text(equals, &manifest->heap_offset)) goto malformed;
        } else if (strcmp(key, "workspace_start") == 0) {
            if (!parse_u32_text(equals, &manifest->workspace_start)) goto malformed;
        } else if (strcmp(key, "reserve_percent") == 0) {
            uint32_t percent = 0;
            if (!parse_u32_text(equals, &percent) || percent > 25U) goto malformed;
            manifest->reserve_percent = percent;
        } else if (strcmp(key, "object_count") == 0) {
            uint64_t count = 0;
            if (!parse_u64_text(equals, &count) || count > SIZE_MAX) goto malformed;
            manifest->record_count = (size_t)count;
            manifest->records = ld_xcalloc(manifest->record_count == 0 ? 1U : manifest->record_count,
                                           sizeof(*manifest->records));
        } else if (strcmp(key, "record") == 0) {
            if (manifest->records == NULL || records_seen >= manifest->record_count) goto malformed;
            uint64_t fields[13] = {0};
            if (!parse_record_tuple(equals, fields) ||
                fields[0] > (uint64_t)EXFAT_OBJ_FILE ||
                fields[1] > UINT32_MAX || fields[2] > UINT32_MAX ||
                fields[3] == 0U || fields[3] > UINT32_MAX ||
                fields[4] > UINT32_MAX || fields[7] > UINT32_MAX ||
                fields[11] > UINT32_MAX || fields[12] > UINT32_MAX)
                goto malformed;
            ExfatRelayoutRecord *record = &manifest->records[records_seen++];
            record->kind = (ExfatObjectKind)fields[0];
            record->target_start = (uint32_t)fields[1];
            record->staged_start = (uint32_t)fields[2];
            record->clusters = (uint32_t)fields[3];
            record->reserve_clusters = (uint32_t)fields[4];
            record->parent_index = fields[5];
            record->entry_offset = fields[6];
            record->entry_count = (uint32_t)fields[7];
            record->system_entry_offset = fields[8];
            record->data_length = fields[9];
            record->valid_length = fields[10];
            record->regular_file = fields[11] != 0U;
            record->directory = fields[12] != 0U;
        } else {
            goto malformed;
        }
    }
    free(line);
    fclose(file);
    if (manifest->serial == 0U || manifest->target_identity[0] == '\0' ||
        manifest->device_size == 0U || manifest->volume_bytes == 0U ||
        manifest->bytes_per_sector == 0U || manifest->cluster_size == 0U ||
        manifest->cluster_count == 0U || manifest->fat_length == 0U ||
        manifest->workspace_start < 2U || manifest->records == NULL ||
        records_seen != manifest->record_count) {
        goto malformed_closed;
    }
    return 0;

malformed:
    free(line);
    fclose(file);
malformed_closed:
    manifest_free(manifest);
    exfat_set_error(error, "exFAT relayout recovery journal is malformed or incomplete");
    return -1;
}

static uint32_t fat_value(const ExfatVolume *volume, uint32_t cluster) {
    uint64_t offset = (uint64_t)cluster * 4U;
    if (offset + 4U > volume->fat_length) return EXFAT_BAD_CLUSTER;
    return exfat_u32(volume->fat, (size_t)offset);
}

static bool volume_has_bad_clusters(const ExfatVolume *volume) {
    for (uint32_t cluster = 2U; cluster < volume->cluster_count + 2U; ++cluster) {
        if (fat_value(volume, cluster) == EXFAT_BAD_CLUSTER) return true;
    }
    return false;
}

static bool canonical_layout(const ExfatCatalogue *catalogue,
                             const ExfatPlan *plan) {
    if (catalogue->bitmap_length != plan->bitmap_length ||
        memcmp(catalogue->bitmap, plan->expected_bitmap, plan->bitmap_length) != 0) {
        return false;
    }
    for (size_t i = 0; i < catalogue->objects.count; ++i) {
        const ExfatObject *object = &catalogue->objects.items[i];
        if (object->clusters.count == 0U) continue;
        for (size_t j = 0; j < object->clusters.count; ++j) {
            if (object->clusters.items[j] != object->target_start + (uint32_t)j) return false;
        }
        if (plan->reserve_percent != 0U && object->regular_file) {
            uint32_t start = object->target_start + (uint32_t)object->clusters.count;
            for (uint32_t r = 0; r < object->reserve_clusters; ++r) {
                if (exfat_allocated(catalogue, start + r)) return false;
            }
        }
    }
    return true;
}

static size_t object_cluster_total(const ExfatCatalogue *catalogue) {
    size_t total = 0;
    for (size_t i = 0; i < catalogue->objects.count; ++i) {
        size_t count = catalogue->objects.items[i].clusters.count;
        if (count > SIZE_MAX - total) return SIZE_MAX;
        total += count;
    }
    return total;
}

static bool terminal_workspace(const ExfatVolume *volume,
                               const ExfatCatalogue *catalogue,
                               uint32_t final_cluster,
                               size_t needed,
                               uint32_t *start_out) {
    if (needed == 0U || needed > volume->cluster_count) return false;
    uint32_t max_cluster = volume->cluster_count + 1U;
    size_t run = 0;
    for (uint32_t cluster = max_cluster; cluster >= 2U; --cluster) {
        if (exfat_allocated(catalogue, cluster) ||
            fat_value(volume, cluster) == EXFAT_BAD_CLUSTER) {
            break;
        }
        run++;
        if (run >= needed) break;
        if (cluster == 2U) break;
    }
    if (run < needed) return false;
    uint32_t start = max_cluster - (uint32_t)needed + 1U;
    if (start <= final_cluster) return false;
    *start_out = start;
    return true;
}

static size_t choose_buffer_bytes(uint32_t cluster_size, size_t ram_bytes,
                                  size_t batch_clusters) {
    if (ram_bytes == 0U) ram_bytes = ld_default_ram_limit();
    uint64_t limit = ram_bytes;
    if (limit > EXFAT_RELAYOUT_MAX_BUFFER) limit = EXFAT_RELAYOUT_MAX_BUFFER;
    if (batch_clusters != 0U) {
        uint64_t batch = (uint64_t)batch_clusters * cluster_size;
        if (batch != 0U && batch < limit) limit = batch;
    }
    if (limit < cluster_size) limit = cluster_size;
    limit -= limit % cluster_size;
    if (limit == 0U) limit = cluster_size;
    if (limit > SIZE_MAX) limit = SIZE_MAX - (SIZE_MAX % cluster_size);
    return (size_t)limit;
}

static int copy_range(int fd, uint64_t source, uint64_t destination,
                      uint64_t length, uint8_t *buffer, size_t buffer_bytes,
                      bool honour_stop, bool *stopped, char **error) {
    uint64_t done = 0;
    while (done < length) {
        size_t chunk = (size_t)((length - done) < buffer_bytes
                                    ? (length - done) : buffer_bytes);
        if (ld_pread_full(fd, buffer, chunk, source + done) != (ssize_t)chunk ||
            ld_pwrite_full(fd, buffer, chunk, destination + done) != (ssize_t)chunk) {
            exfat_set_error(error, "short exFAT relayout copy at byte %" PRIu64,
                            source + done);
            return -1;
        }
        done += chunk;
        if (honour_stop && ld_stop_requested()) {
            *stopped = true;
            return 0;
        }
    }
    return 0;
}

static int copy_object_to_run(ExfatVolume *volume, const ExfatObject *object,
                              uint32_t target_start, uint8_t *buffer,
                              size_t buffer_bytes, bool honour_stop,
                              bool *stopped, char **error) {
    size_t position = 0;
    while (position < object->clusters.count) {
        size_t run = 1;
        while (position + run < object->clusters.count &&
               object->clusters.items[position + run] ==
                   object->clusters.items[position] + (uint32_t)run) {
            run++;
        }
        uint64_t source = exfat_cluster_offset(volume, object->clusters.items[position]);
        uint64_t destination = exfat_cluster_offset(volume,
                                                    target_start + (uint32_t)position);
        uint64_t bytes = (uint64_t)run * volume->cluster_size;
        if (copy_range(volume->fd, source, destination, bytes, buffer, buffer_bytes,
                       honour_stop, stopped, error) != 0 || *stopped) {
            return *stopped ? 0 : -1;
        }
        position += run;
    }
    return 0;
}

static int copy_run_to_object(ExfatVolume *volume, uint32_t source_start,
                              const ExfatObject *object, uint8_t *buffer,
                              size_t buffer_bytes, char **error) {
    for (size_t position = 0; position < object->clusters.count;) {
        size_t run = 1;
        while (position + run < object->clusters.count &&
               object->clusters.items[position + run] ==
                   object->clusters.items[position] + (uint32_t)run) {
            run++;
        }
        uint64_t source = exfat_cluster_offset(volume,
                                               source_start + (uint32_t)position);
        uint64_t destination = exfat_cluster_offset(volume,
                                                    object->clusters.items[position]);
        uint64_t bytes = (uint64_t)run * volume->cluster_size;
        bool stopped = false;
        if (copy_range(volume->fd, source, destination, bytes, buffer, buffer_bytes,
                       false, &stopped, error) != 0) {
            return -1;
        }
        position += run;
    }
    return 0;
}

static void bitmap_set(uint8_t *bitmap, uint32_t cluster, bool allocated) {
    uint32_t bit = cluster - 2U;
    uint8_t mask = (uint8_t)(1U << (bit & 7U));
    if (allocated) bitmap[bit >> 3U] |= mask;
    else bitmap[bit >> 3U] &= (uint8_t)~mask;
}

static int build_expected_bitmap(const ExfatRelayoutManifest *manifest,
                                 size_t bitmap_length,
                                 uint8_t **bitmap_out,
                                 char **error) {
    uint8_t *bitmap = calloc(bitmap_length == 0U ? 1U : bitmap_length, 1U);
    if (bitmap == NULL) {
        exfat_set_error(error, "allocating exFAT canonical bitmap failed");
        return -1;
    }
    for (size_t i = 0; i < manifest->record_count; ++i) {
        const ExfatRelayoutRecord *record = &manifest->records[i];
        uint64_t end = (uint64_t)record->target_start + record->clusters;
        if (record->target_start < 2U ||
            end > (uint64_t)manifest->cluster_count + 2U) {
            free(bitmap);
            exfat_set_error(error, "exFAT canonical object target exceeds the heap");
            return -1;
        }
        for (uint32_t j = 0; j < record->clusters; ++j) {
            uint32_t cluster = record->target_start + j;
            uint32_t bit = cluster - 2U;
            if ((uint64_t)bit >= bitmap_length * 8ULL) {
                free(bitmap);
                exfat_set_error(error, "exFAT canonical bitmap target is out of bounds");
                return -1;
            }
            bitmap_set(bitmap, cluster, true);
        }
    }
    *bitmap_out = bitmap;
    return 0;
}

static const ExfatRelayoutRecord *find_kind(const ExfatRelayoutManifest *manifest,
                                             ExfatObjectKind kind) {
    for (size_t i = 0; i < manifest->record_count; ++i) {
        if (manifest->records[i].kind == kind) return &manifest->records[i];
    }
    return NULL;
}

static void patch_entry_set_checksum(uint8_t *payload, size_t offset,
                                     uint32_t count) {
    exfat_put_u16(payload, offset + 2U,
                  exfat_entry_checksum(payload + offset, (size_t)count * 32U));
}

static int patch_directory_payload(const ExfatRelayoutManifest *manifest,
                                   size_t directory_index,
                                   uint8_t *payload, size_t length,
                                   char **error) {
    for (size_t i = 0; i < manifest->record_count; ++i) {
        const ExfatRelayoutRecord *child = &manifest->records[i];
        if (child->parent_index != directory_index ||
            child->kind == EXFAT_OBJ_BITMAP || child->kind == EXFAT_OBJ_UPCASE ||
            child->kind == EXFAT_OBJ_ROOT) {
            continue;
        }
        if (child->entry_count < 2U || child->entry_offset == UINT64_MAX ||
            child->entry_offset + (uint64_t)child->entry_count * 32U > length) {
            exfat_set_error(error, "missing exFAT directory reference during relayout");
            return -1;
        }
        size_t stream = SIZE_MAX;
        for (uint32_t entry = 1U; entry < child->entry_count; ++entry) {
            size_t candidate = (size_t)child->entry_offset + (size_t)entry * 32U;
            if (payload[candidate] == 0xc0U) {
                stream = candidate;
                break;
            }
        }
        if (stream == SIZE_MAX) {
            exfat_set_error(error, "exFAT stream extension disappeared during relayout");
            return -1;
        }
        exfat_put_u32(payload, stream + 20U, child->target_start);
        payload[stream + 1U] |= 0x02U;
        patch_entry_set_checksum(payload, (size_t)child->entry_offset,
                                 child->entry_count);
    }

    if (manifest->records[directory_index].kind == EXFAT_OBJ_ROOT) {
        const ExfatRelayoutRecord *bitmap = find_kind(manifest, EXFAT_OBJ_BITMAP);
        const ExfatRelayoutRecord *upcase = find_kind(manifest, EXFAT_OBJ_UPCASE);
        if (bitmap == NULL || upcase == NULL ||
            bitmap->system_entry_offset == UINT64_MAX ||
            upcase->system_entry_offset == UINT64_MAX ||
            bitmap->system_entry_offset + 32U > length ||
            upcase->system_entry_offset + 32U > length) {
            exfat_set_error(error, "exFAT root system entries are unavailable during relayout");
            return -1;
        }
        exfat_put_u32(payload, (size_t)bitmap->system_entry_offset + 20U,
                      bitmap->target_start);
        exfat_put_u32(payload, (size_t)upcase->system_entry_offset + 20U,
                      upcase->target_start);
    }
    return 0;
}

static int write_buffer_to_run(ExfatVolume *volume, uint32_t target_start,
                               const uint8_t *payload, size_t length,
                               char **error) {
    uint64_t offset = exfat_cluster_offset(volume, target_start);
    if (ld_pwrite_full(volume->fd, payload, length, offset) != (ssize_t)length) {
        exfat_set_error(error, "short exFAT canonical payload write");
        return -1;
    }
    return 0;
}

static int place_record(ExfatVolume *volume,
                        const ExfatRelayoutManifest *manifest,
                        size_t index, uint8_t *copy_buffer,
                        size_t copy_buffer_bytes, bool honour_stop,
                        bool *stopped, char **error) {
    const ExfatRelayoutRecord *record = &manifest->records[index];
    uint64_t allocation64 = (uint64_t)record->clusters * volume->cluster_size;
    if (allocation64 > SIZE_MAX) {
        exfat_set_error(error, "exFAT relayout object exceeds addressable memory");
        return -1;
    }

    if (record->kind != EXFAT_OBJ_BITMAP && !record->directory) {
        return copy_range(volume->fd,
                          exfat_cluster_offset(volume, record->staged_start),
                          exfat_cluster_offset(volume, record->target_start),
                          allocation64, copy_buffer, copy_buffer_bytes,
                          honour_stop, stopped, error);
    }

    size_t allocation = (size_t)allocation64;
    uint8_t *payload = calloc(allocation == 0U ? 1U : allocation, 1U);
    if (payload == NULL) {
        exfat_set_error(error, "allocating exFAT relayout metadata payload failed");
        return -1;
    }
    if (record->kind == EXFAT_OBJ_BITMAP) {
        uint8_t *bitmap = NULL;
        if (build_expected_bitmap(manifest, (size_t)record->data_length,
                                  &bitmap, error) != 0) {
            free(payload);
            return -1;
        }
        if (record->data_length > allocation64) {
            free(bitmap);
            free(payload);
            exfat_set_error(error, "exFAT allocation bitmap exceeds its allocation");
            return -1;
        }
        memcpy(payload, bitmap, (size_t)record->data_length);
        free(bitmap);
    } else {
        if (ld_pread_full(volume->fd, payload, allocation,
                          exfat_cluster_offset(volume, record->staged_start)) !=
            (ssize_t)allocation) {
            free(payload);
            exfat_set_error(error, "short exFAT staged directory read");
            return -1;
        }
        if (patch_directory_payload(manifest, index, payload, allocation, error) != 0) {
            free(payload);
            return -1;
        }
    }
    int result = write_buffer_to_run(volume, record->target_start,
                                     payload, allocation, error);
    free(payload);
    if (result == 0 && honour_stop && ld_stop_requested()) *stopped = true;
    return result;
}

static int build_final_fat(const ExfatVolume *volume,
                           const ExfatRelayoutManifest *manifest,
                           uint8_t **fat_out, char **error) {
    if (volume->fat_length > SIZE_MAX) {
        exfat_set_error(error, "exFAT FAT is too large");
        return -1;
    }
    uint8_t *fat = calloc((size_t)volume->fat_length, 1U);
    if (fat == NULL) {
        exfat_set_error(error, "allocating exFAT canonical FAT failed");
        return -1;
    }
    if (volume->fat_length >= 8U) memcpy(fat, volume->fat, 8U);
    for (size_t i = 0; i < manifest->record_count; ++i) {
        const ExfatRelayoutRecord *record = &manifest->records[i];
        if (record->kind != EXFAT_OBJ_BITMAP && record->kind != EXFAT_OBJ_UPCASE &&
            record->kind != EXFAT_OBJ_ROOT) {
            continue;
        }
        for (uint32_t j = 0; j < record->clusters; ++j) {
            uint32_t cluster = record->target_start + j;
            uint64_t offset = (uint64_t)cluster * 4U;
            if (offset + 4U > volume->fat_length) {
                free(fat);
                exfat_set_error(error, "exFAT FAT target exceeds the FAT region");
                return -1;
            }
            uint32_t value = j + 1U < record->clusters ? cluster + 1U : EXFAT_EOC;
            exfat_put_u32(fat, (size_t)offset, value);
        }
    }
    *fat_out = fat;
    return 0;
}

static void fill_boot_checksum_sector(uint8_t *region, uint32_t bytes_per_sector) {
    uint32_t checksum = exfat_boot_checksum(region, bytes_per_sector);
    uint8_t *sector = region + (size_t)11U * bytes_per_sector;
    for (uint32_t offset = 0; offset + 4U <= bytes_per_sector; offset += 4U) {
        exfat_put_u32(sector, offset, checksum);
    }
}

static int write_dirty_flag_only(ExfatVolume *volume, bool dirty, char **error) {
    uint16_t flags = volume->volume_flags;
    if (dirty) flags |= EXFAT_VOLUME_DIRTY;
    else flags &= (uint16_t)~EXFAT_VOLUME_DIRTY;
    uint8_t bytes[2] = {(uint8_t)flags, (uint8_t)(flags >> 8)};
    uint64_t backup = (uint64_t)12U * volume->bytes_per_sector + 106U;
    if (ld_pwrite_full(volume->fd, bytes, sizeof(bytes), backup) !=
            (ssize_t)sizeof(bytes) ||
        fsync(volume->fd) != 0 ||
        ld_pwrite_full(volume->fd, bytes, sizeof(bytes), 106U) !=
            (ssize_t)sizeof(bytes) ||
        fsync(volume->fd) != 0) {
        exfat_set_error(error, "cannot publish exFAT dirty transaction flag");
        return -1;
    }
    volume->volume_flags = flags;
    return 0;
}

static int write_boot_state(ExfatVolume *volume, uint32_t root_cluster,
                            bool dirty, uint8_t percent_in_use,
                            char **error) {
    size_t region_bytes = (size_t)12U * volume->bytes_per_sector;
    size_t total_bytes = region_bytes * 2U;
    uint8_t *boot = ld_xmalloc(total_bytes);
    memcpy(boot, volume->boot_regions, total_bytes);
    for (size_t copy = 0; copy < 2U; ++copy) {
        uint8_t *base = boot + copy * region_bytes;
        exfat_put_u32(base, 96U, root_cluster);
        uint16_t flags = exfat_u16(base, 106U);
        if (dirty) flags |= EXFAT_VOLUME_DIRTY;
        else flags &= (uint16_t)~EXFAT_VOLUME_DIRTY;
        exfat_put_u16(base, 106U, flags);
        base[112U] = percent_in_use;
        fill_boot_checksum_sector(base, volume->bytes_per_sector);
    }
    if (ld_pwrite_full(volume->fd, boot + region_bytes, region_bytes,
                       region_bytes) != (ssize_t)region_bytes ||
        fsync(volume->fd) != 0 ||
        ld_pwrite_full(volume->fd, boot, region_bytes, 0) != (ssize_t)region_bytes ||
        fsync(volume->fd) != 0) {
        free(boot);
        exfat_set_error(error, "cannot publish exFAT boot-region transaction state");
        return -1;
    }
    free(boot);
    return 0;
}

static int restore_original_boot(ExfatVolume *volume, char **error) {
    size_t region_bytes = (size_t)12U * volume->bytes_per_sector;
    if (ld_pwrite_full(volume->fd, volume->boot_regions + region_bytes,
                       region_bytes, region_bytes) != (ssize_t)region_bytes ||
        fsync(volume->fd) != 0 ||
        ld_pwrite_full(volume->fd, volume->boot_regions,
                       region_bytes, 0) != (ssize_t)region_bytes ||
        fsync(volume->fd) != 0) {
        exfat_set_error(error, "cannot restore the original exFAT boot region");
        return -1;
    }
    return 0;
}

static uint8_t percent_in_use(const ExfatRelayoutManifest *manifest) {
    uint64_t allocated = 0;
    for (size_t i = 0; i < manifest->record_count; ++i) {
        allocated += manifest->records[i].clusters;
    }
    uint64_t percent = manifest->cluster_count == 0U ? 0U
        : allocated * 100U / manifest->cluster_count;
    if (percent > 100U) percent = 100U;
    return (uint8_t)percent;
}

static int commit_final_metadata(ExfatVolume *volume,
                                 const ExfatRelayoutManifest *manifest,
                                 char **error) {
    uint8_t *fat = NULL;
    if (build_final_fat(volume, manifest, &fat, error) != 0) return -1;
    if (ld_pwrite_full(volume->fd, fat, (size_t)volume->fat_length,
                       volume->fat_offset) != (ssize_t)volume->fat_length ||
        fsync(volume->fd) != 0) {
        free(fat);
        exfat_set_error(error, "cannot commit the canonical exFAT FAT");
        return -1;
    }
    free(fat);
    const ExfatRelayoutRecord *root = find_kind(manifest, EXFAT_OBJ_ROOT);
    if (root == NULL) {
        exfat_set_error(error, "canonical exFAT root record is missing");
        return -1;
    }
    return write_boot_state(volume, root->target_start, false,
                            percent_in_use(manifest), error);
}

static void emit_live_reset(const ExfatVolume *volume,
                            const ExfatCatalogue *catalogue) {
    printf("@@LIVE_RESET {\"unit_size\":%u,\"filesystem_units\":%u,\"used_ranges\":[",
           volume->cluster_size, volume->cluster_count);
    bool first = true;
    uint32_t cluster = 2U;
    while (cluster < volume->cluster_count + 2U) {
        if (!exfat_allocated(catalogue, cluster)) {
            cluster++;
            continue;
        }
        uint32_t start = cluster;
        while (cluster < volume->cluster_count + 2U &&
               exfat_allocated(catalogue, cluster)) {
            cluster++;
        }
        if (!first) putchar(',');
        printf("[%" PRIu64 ",%" PRIu64 "]",
               (uint64_t)(start - 2U) * volume->cluster_size,
               (uint64_t)(cluster - start) * volume->cluster_size);
        first = false;
    }
    fputs("]}\n", stdout);
    fflush(stdout);
}

static int verify_final_layout(const char *device, unsigned reserve_percent,
                               bool live_updates, char **error) {
    ExfatVolume volume;
    ExfatCatalogue catalogue;
    ExfatPlan plan;
    if (exfat_scan(device, false, &volume, &catalogue, error) != 0) return -1;
    if (exfat_build_plan(&volume, &catalogue, reserve_percent, &plan, error) != 0) {
        exfat_catalogue_free(&catalogue);
        exfat_close_volume(&volume);
        return -1;
    }
    bool verified = canonical_layout(&catalogue, &plan);
    if (verified && live_updates) emit_live_reset(&volume, &catalogue);
    size_t objects = catalogue.objects.count;
    uint64_t files = catalogue.regular_files;
    uint64_t directories = catalogue.directories;
    exfat_plan_free(&plan);
    exfat_catalogue_free(&catalogue);
    exfat_close_volume(&volume);
    if (!verified) {
        exfat_set_error(error, "final exFAT layout differs from the canonical relayout plan");
        return -1;
    }
    printf("Layout verification:      %zu exFAT objects (%" PRIu64
           " files, %" PRIu64 " directories) contiguous; canonical %u%% policy verified.\n",
           objects, files, directories, reserve_percent);
    return 0;
}

static int populate_manifest(ExfatRelayoutManifest *manifest,
                             const ExfatVolume *volume,
                             const ExfatCatalogue *catalogue,
                             unsigned reserve_percent,
                             uint32_t workspace_start,
                             char **error) {
    memset(manifest, 0, sizeof(*manifest));
    if (ld_fd_format_identity(volume->fd, manifest->target_identity,
                              sizeof(manifest->target_identity)) != 0) {
        exfat_set_error(error, "cannot capture exFAT target identity: %s",
                        strerror(errno));
        return -1;
    }
    manifest->device_size = volume->device_size;
    manifest->serial = volume->serial;
    manifest->volume_bytes = volume->volume_bytes;
    manifest->bytes_per_sector = volume->bytes_per_sector;
    manifest->cluster_size = volume->cluster_size;
    manifest->cluster_count = volume->cluster_count;
    manifest->fat_offset = volume->fat_offset;
    manifest->fat_length = volume->fat_length;
    manifest->heap_offset = volume->heap_offset;
    manifest->workspace_start = workspace_start;
    manifest->reserve_percent = reserve_percent;
    manifest->record_count = catalogue->objects.count;
    manifest->records = ld_xcalloc(manifest->record_count == 0U ? 1U : manifest->record_count,
                                   sizeof(*manifest->records));
    uint32_t staged = workspace_start;
    for (size_t i = 0; i < manifest->record_count; ++i) {
        const ExfatObject *object = &catalogue->objects.items[i];
        ExfatRelayoutRecord *record = &manifest->records[i];
        record->kind = object->kind;
        record->target_start = object->target_start;
        record->staged_start = staged;
        record->clusters = (uint32_t)object->clusters.count;
        record->reserve_clusters = object->reserve_clusters;
        record->parent_index = object->parent_index == SIZE_MAX
                                   ? UINT64_MAX : (uint64_t)object->parent_index;
        record->entry_offset = object->entry_offset;
        record->entry_count = object->entry_count;
        record->system_entry_offset = object->system_entry_offset;
        record->data_length = object->data_length;
        record->valid_length = object->valid_length;
        record->regular_file = object->regular_file;
        record->directory = object->directory;
        staged += record->clusters;
    }
    return 0;
}

static int rollback_original_layout(ExfatVolume *volume,
                                    const ExfatCatalogue *catalogue,
                                    const ExfatRelayoutManifest *manifest,
                                    uint8_t *buffer, size_t buffer_bytes,
                                    char **error) {
    fprintf(stderr,
            "Stop requested during final placement; restoring the original exFAT layout from the durable terminal workspace.\n");
    fflush(stderr);
    for (size_t i = 0; i < catalogue->objects.count; ++i) {
        if (copy_run_to_object(volume, manifest->records[i].staged_start,
                               &catalogue->objects.items[i], buffer,
                               buffer_bytes, error) != 0) {
            return -1;
        }
    }
    if (fsync(volume->fd) != 0 || restore_original_boot(volume, error) != 0) {
        if (error != NULL && *error == NULL) {
            exfat_set_error(error, "cannot sync restored exFAT source layout");
        }
        return -1;
    }
    return 0;
}

static int execute_manifest_layout(ExfatVolume *volume,
                                   const ExfatRelayoutManifest *manifest,
                                   size_t ram_bytes, size_t batch_clusters,
                                   bool honour_stop, bool *stopped,
                                   char **error) {
    size_t buffer_bytes = choose_buffer_bytes(volume->cluster_size,
                                              ram_bytes, batch_clusters);
    uint8_t *buffer = ld_xmalloc(buffer_bytes);
    PlacementIndex *order = ld_xmalloc(manifest->record_count * sizeof(*order));
    for (size_t i = 0; i < manifest->record_count; ++i) {
        order[i] = (PlacementIndex){.index = i,
                                  .target_start = manifest->records[i].target_start};
    }
    qsort(order, manifest->record_count, sizeof(*order), placement_index_compare);
    for (size_t position = 0; position < manifest->record_count; ++position) {
        if (place_record(volume, manifest, order[position].index,
                         buffer, buffer_bytes, honour_stop,
                         stopped, error) != 0 || *stopped) {
            free(order);
            free(buffer);
            return *stopped ? 0 : -1;
        }
    }
    free(order);
    free(buffer);
    if (fsync(volume->fd) != 0) {
        exfat_set_error(error, "cannot sync canonical exFAT data placement");
        return -1;
    }
    return 0;
}

int exfat_relayout_in_place(const char *device, const char *journal_path,
                            unsigned reserve_percent, size_t ram_bytes,
                            size_t batch_clusters, bool live_updates,
                            ExfatRelayoutStats *stats, char **error) {
    memset(stats, 0, sizeof(*stats));
    if (reserve_percent > 25U) {
        exfat_set_error(error, "exFAT reserve percentage must be between 0 and 25");
        return EXFAT_RELAYOUT_FAILED;
    }

    ExfatVolume source;
    ExfatCatalogue catalogue;
    ExfatPlan plan;
    if (exfat_scan(device, false, &source, &catalogue, error) != 0) {
        return EXFAT_RELAYOUT_FAILED;
    }
    if (exfat_build_plan(&source, &catalogue, reserve_percent, &plan, error) != 0) {
        exfat_catalogue_free(&catalogue);
        exfat_close_volume(&source);
        return EXFAT_RELAYOUT_FAILED;
    }
    if (canonical_layout(&catalogue, &plan)) {
        printf("Not needed; canonical exFAT layout with %u%% post-file reserve policy verified.\n",
               reserve_percent);
        exfat_plan_free(&plan);
        exfat_catalogue_free(&catalogue);
        exfat_close_volume(&source);
        return EXFAT_RELAYOUT_NOT_NEEDED;
    }

    size_t total_clusters = object_cluster_total(&catalogue);
    if (total_clusters == 0U || total_clusters == SIZE_MAX ||
        volume_has_bad_clusters(&source)) {
        exfat_plan_free(&plan);
        exfat_catalogue_free(&catalogue);
        exfat_close_volume(&source);
        return EXFAT_RELAYOUT_FALLBACK;
    }
    uint32_t workspace_start = 0;
    if (!terminal_workspace(&source, &catalogue, plan.final_cluster,
                            total_clusters, &workspace_start)) {
        exfat_plan_free(&plan);
        exfat_catalogue_free(&catalogue);
        exfat_close_volume(&source);
        return EXFAT_RELAYOUT_FALLBACK;
    }
    if (ram_bytes == 0U) ram_bytes = ld_default_ram_limit();
    for (size_t i = 0; i < catalogue.objects.count; ++i) {
        const ExfatObject *object = &catalogue.objects.items[i];
        if ((object->directory || object->kind == EXFAT_OBJ_BITMAP) &&
            (uint64_t)object->clusters.count * source.cluster_size > ram_bytes) {
            exfat_plan_free(&plan);
            exfat_catalogue_free(&catalogue);
            exfat_close_volume(&source);
            return EXFAT_RELAYOUT_FALLBACK;
        }
    }

    printf("Raw userspace native-C exFAT relayout engine %s\n", LD_VERSION);
    printf("exFAT relayout preflight: %zu objects, %zu allocated clusters; canonical reserve policy %u%%.\n",
           catalogue.objects.count, total_clusters, reserve_percent);
    printf("exFAT phase 1: staging the complete live object set into a %zu-cluster terminal safety workspace beginning at cluster %u.\n",
           total_clusters, workspace_start);
    printf("exFAT RAM copy buffer:    %.1f MiB\n",
           (double)choose_buffer_bytes(source.cluster_size, ram_bytes,
                                       batch_clusters) / (1024.0 * 1024.0));
    fflush(stdout);

    exfat_plan_free(&plan);
    exfat_close_volume(&source);

    ExfatVolume volume;
    if (exfat_open_volume(device, true, false, &volume, error) != 0) {
        exfat_catalogue_free(&catalogue);
        return EXFAT_RELAYOUT_FAILED;
    }
    size_t buffer_bytes = choose_buffer_bytes(volume.cluster_size,
                                              ram_bytes, batch_clusters);
    uint8_t *buffer = ld_xmalloc(buffer_bytes);
    ExfatRelayoutManifest manifest;
    if (populate_manifest(&manifest, &volume, &catalogue, reserve_percent,
                          workspace_start, error) != 0) {
        free(buffer);
        exfat_close_volume(&volume);
        exfat_catalogue_free(&catalogue);
        return EXFAT_RELAYOUT_FAILED;
    }

    bool stopped = false;
    for (size_t i = 0; i < catalogue.objects.count; ++i) {
        if (copy_object_to_run(&volume, &catalogue.objects.items[i],
                               manifest.records[i].staged_start,
                               buffer, buffer_bytes, true,
                               &stopped, error) != 0) {
            free(buffer);
            manifest_free(&manifest);
            exfat_close_volume(&volume);
            exfat_catalogue_free(&catalogue);
            return EXFAT_RELAYOUT_FAILED;
        }
        if (stopped) {
            fprintf(stderr,
                    "exFAT relayout stopped safely during workspace staging; the source filesystem was never modified.\n");
            free(buffer);
            manifest_free(&manifest);
            exfat_close_volume(&volume);
            exfat_catalogue_free(&catalogue);
            return EXFAT_RELAYOUT_STOPPED;
        }
    }
    if (fsync(volume.fd) != 0) {
        exfat_set_error(error, "cannot sync the durable exFAT terminal workspace");
        free(buffer);
        manifest_free(&manifest);
        exfat_close_volume(&volume);
        exfat_catalogue_free(&catalogue);
        return EXFAT_RELAYOUT_FAILED;
    }
    stats->workspace_clusters = total_clusters;
    stats->staged_clusters = total_clusters;
    printf("exFAT workspace staging complete: %zu clusters copied in one durable pass; original metadata is still unchanged.\n",
           total_clusters);
    fflush(stdout);

    if (save_manifest(journal_path, &manifest, error) != 0) {
        free(buffer);
        manifest_free(&manifest);
        exfat_close_volume(&volume);
        exfat_catalogue_free(&catalogue);
        return EXFAT_RELAYOUT_FAILED;
    }
    if (ld_stop_requested()) {
        if (remove_manifest(journal_path, error) != 0) {
            free(buffer);
            manifest_free(&manifest);
            exfat_close_volume(&volume);
            exfat_catalogue_free(&catalogue);
            return EXFAT_RELAYOUT_FAILED;
        }
        fprintf(stderr,
                "exFAT relayout stopped safely before source placement; the original filesystem is unchanged.\n");
        free(buffer);
        manifest_free(&manifest);
        exfat_close_volume(&volume);
        exfat_catalogue_free(&catalogue);
        return EXFAT_RELAYOUT_STOPPED;
    }

    if (write_dirty_flag_only(&volume, true, error) != 0) {
        free(buffer);
        manifest_free(&manifest);
        exfat_close_volume(&volume);
        exfat_catalogue_free(&catalogue);
        return EXFAT_RELAYOUT_FAILED;
    }
    printf("exFAT phase 2: placing the canonical layout directly from the durable terminal workspace; no full-filesystem working image is used.\n");
    fflush(stdout);

    if (execute_manifest_layout(&volume, &manifest, ram_bytes,
                                batch_clusters, true, &stopped, error) != 0) {
        free(buffer);
        manifest_free(&manifest);
        exfat_close_volume(&volume);
        exfat_catalogue_free(&catalogue);
        return EXFAT_RELAYOUT_FAILED;
    }
    if (stopped) {
        if (rollback_original_layout(&volume, &catalogue, &manifest,
                                     buffer, buffer_bytes, error) != 0) {
            free(buffer);
            manifest_free(&manifest);
            exfat_close_volume(&volume);
            exfat_catalogue_free(&catalogue);
            return EXFAT_RELAYOUT_FAILED;
        }
        if (remove_manifest(journal_path, error) != 0) {
            free(buffer);
            manifest_free(&manifest);
            exfat_close_volume(&volume);
            exfat_catalogue_free(&catalogue);
            return EXFAT_RELAYOUT_FAILED;
        }
        fprintf(stderr,
                "exFAT relayout stopped safely; the original allocation layout was restored.\n");
        free(buffer);
        manifest_free(&manifest);
        exfat_close_volume(&volume);
        exfat_catalogue_free(&catalogue);
        return EXFAT_RELAYOUT_STOPPED;
    }
    stats->placed_clusters = total_clusters;

    printf("exFAT phase 3: committing the canonical FAT and boot metadata.\n");
    fflush(stdout);
    if (commit_final_metadata(&volume, &manifest, error) != 0) {
        free(buffer);
        manifest_free(&manifest);
        exfat_close_volume(&volume);
        exfat_catalogue_free(&catalogue);
        return EXFAT_RELAYOUT_FAILED;
    }
    free(buffer);
    exfat_close_volume(&volume);
    exfat_catalogue_free(&catalogue);

    if (verify_final_layout(device, reserve_percent, live_updates, error) != 0) {
        manifest_free(&manifest);
        return EXFAT_RELAYOUT_FAILED;
    }
    if (remove_manifest(journal_path, error) != 0) {
        manifest_free(&manifest);
        return EXFAT_RELAYOUT_FAILED;
    }
    stats->objects_repositioned = manifest.record_count;
    stats->completed = true;
    printf("exFAT unified workspace layout: %zu objects, %zu clusters, two direct data passes plus one metadata commit.\n",
           manifest.record_count, total_clusters);
    printf("exFAT relayout completed with serial and full volume capacity preserved.\n");
    manifest_free(&manifest);
    return EXFAT_RELAYOUT_COMPLETED;
}

static bool boot_region_valid(const uint8_t *region, uint32_t bytes_per_sector) {
    if (memcmp(region + 3U, "EXFAT   ", 8U) != 0) return false;
    uint32_t expected = exfat_boot_checksum(region, bytes_per_sector);
    const uint8_t *checksum_sector =
        region + (size_t)11U * bytes_per_sector;
    for (uint32_t offset = 0; offset + 4U <= bytes_per_sector; offset += 4U) {
        if (exfat_u32(checksum_sector, offset) != expected) return false;
    }
    return true;
}

/* A crash can interrupt publication of one of exFAT's two boot regions.  The
   relayout manifest contains the immutable sector geometry, so Recover repairs
   one torn copy from the other valid copy before the normal volume parser is
   allowed to inspect the transaction.  Data placement never begins until both
   boot copies have first been made valid and dirty. */
static int repair_boot_regions_from_survivor(const char *device,
                                             const char *target_identity,
                                             uint64_t device_size,
                                             uint32_t bytes_per_sector,
                                             char **error) {
    if (bytes_per_sector < 512U || bytes_per_sector > 4096U ||
        (bytes_per_sector & (bytes_per_sector - 1U)) != 0U) {
        exfat_set_error(error, "exFAT recovery journal has invalid sector geometry");
        return -1;
    }
    char *real = realpath(device, NULL);
    if (real == NULL) {
        exfat_set_error(error, "cannot resolve exFAT recovery target: %s",
                        strerror(errno));
        return -1;
    }
    struct stat status;
    if (stat(real, &status) != 0 ||
        (!S_ISREG(status.st_mode) && !S_ISBLK(status.st_mode))) {
        exfat_set_error(error, "exFAT recovery target is not a block device or image");
        free(real);
        return -1;
    }
    if (S_ISBLK(status.st_mode) && ld_path_is_mounted(real)) {
        exfat_set_error(error, "exFAT recovery target is mounted");
        free(real);
        return -1;
    }
    int fd = ld_device_open_verified_fd(real, true, target_identity, device_size);
    if (fd < 0) {
        exfat_set_error(error, "cannot open exFAT recovery target: %s",
                        strerror(errno));
        free(real);
        return -1;
    }
    free(real);
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        exfat_set_error(error, "cannot lock exFAT recovery target: %s",
                        strerror(errno));
        close(fd);
        return -1;
    }

    size_t region_bytes = (size_t)12U * bytes_per_sector;
    uint8_t *regions = ld_xmalloc(region_bytes * 2U);
    if (ld_pread_full(fd, regions, region_bytes * 2U, 0U) !=
        (ssize_t)(region_bytes * 2U)) {
        exfat_set_error(error, "cannot read exFAT boot regions during recovery");
        free(regions);
        (void)flock(fd, LOCK_UN);
        close(fd);
        return -1;
    }
    bool main_valid = boot_region_valid(regions, bytes_per_sector);
    bool backup_valid = boot_region_valid(regions + region_bytes,
                                          bytes_per_sector);
    if (!main_valid && !backup_valid) {
        exfat_set_error(error,
                        "both exFAT boot-region copies are invalid; automatic recovery cannot establish geometry");
        free(regions);
        (void)flock(fd, LOCK_UN);
        close(fd);
        return -1;
    }
    if (!main_valid || !backup_valid) {
        const uint8_t *survivor = main_valid ? regions : regions + region_bytes;
        uint64_t destination = main_valid ? region_bytes : 0U;
        if (ld_pwrite_full(fd, survivor, region_bytes, destination) !=
                (ssize_t)region_bytes || fsync(fd) != 0) {
            exfat_set_error(error, "cannot repair the torn exFAT boot-region copy");
            free(regions);
            (void)flock(fd, LOCK_UN);
            close(fd);
            return -1;
        }
        fprintf(stderr,
                "exFAT recovery repaired one interrupted boot-region copy from its valid twin.\n");
    }
    free(regions);
    (void)flock(fd, LOCK_UN);
    close(fd);
    return 0;
}

int exfat_relayout_recover(const char *device, const char *journal_path,
                           size_t ram_bytes, size_t batch_clusters,
                           bool live_updates, bool *handled, char **error) {
    ExfatRelayoutManifest manifest;
    int loaded = load_manifest(journal_path, &manifest, handled, error);
    if (loaded != 0 || !*handled) return loaded != 0 ? 1 : 0;

    if (repair_boot_regions_from_survivor(device, manifest.target_identity,
                                          manifest.device_size,
                                          manifest.bytes_per_sector,
                                          error) != 0) {
        manifest_free(&manifest);
        return 1;
    }

    ExfatVolume volume;
    if (exfat_open_volume(device, true, true, &volume, error) != 0) {
        manifest_free(&manifest);
        return 1;
    }
    if (!ld_fd_matches_identity(volume.fd, manifest.target_identity,
                                manifest.device_size)) {
        exfat_close_volume(&volume);
        manifest_free(&manifest);
        exfat_set_error(error,
                        "exFAT recovery descriptor does not match the journaled target");
        return 1;
    }
    if (volume.serial != manifest.serial ||
        volume.volume_bytes != manifest.volume_bytes ||
        volume.bytes_per_sector != manifest.bytes_per_sector ||
        volume.cluster_size != manifest.cluster_size ||
        volume.cluster_count != manifest.cluster_count ||
        volume.fat_offset != manifest.fat_offset ||
        volume.fat_length != manifest.fat_length ||
        volume.heap_offset != manifest.heap_offset) {
        exfat_close_volume(&volume);
        manifest_free(&manifest);
        exfat_set_error(error, "exFAT relayout journal belongs to a different target geometry");
        return 1;
    }

    fprintf(stderr,
            "Recovering exFAT canonical relayout from the durable terminal workspace; placement is idempotently replayed.\n");
    fflush(stderr);
    bool stopped = false;
    if (execute_manifest_layout(&volume, &manifest, ram_bytes,
                                batch_clusters, false, &stopped, error) != 0 ||
        commit_final_metadata(&volume, &manifest, error) != 0) {
        exfat_close_volume(&volume);
        manifest_free(&manifest);
        return 1;
    }
    exfat_close_volume(&volume);
    if (verify_final_layout(device, manifest.reserve_percent,
                            live_updates, error) != 0) {
        manifest_free(&manifest);
        return 1;
    }
    if (remove_manifest(journal_path, error) != 0) {
        manifest_free(&manifest);
        return 1;
    }
    puts("exFAT relayout recovery completed successfully.");
    manifest_free(&manifest);
    return 0;
}
