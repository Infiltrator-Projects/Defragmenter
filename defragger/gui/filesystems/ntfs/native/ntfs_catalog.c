// SPDX-License-Identifier: GPL-3.0-or-later
#include "ntfs_native.h"

#include "infiltratr/core.h"
#include "infiltratr/arithmetic.h"
#include "ld_runtime.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NTFS_MFT_BATCH_BYTES (UINT64_C(32) * 1024U * 1024U)

typedef struct {
    uint64_t record;
    bool present;
    bool directory;
    size_t matching_groups;
    bool fragmented;
    uint64_t expected_vcn;
    size_t growth_streams;
    bool growth_single_valid;
    uint64_t growth_start;
    uint64_t growth_clusters;
} ObjectState;

typedef struct {
    uint64_t key_plus_one;
    size_t index_plus_one;
} ObjectSlot;

typedef struct {
    ObjectState *items;
    size_t count;
    size_t capacity;
    ObjectSlot *slots;
    size_t slot_count;
    size_t slot_used;
} ObjectVec;

typedef struct {
    bool processed;
    bool malformed;
    bool in_use;
    bool fatal;
    uint64_t number;
    uint64_t base;
    bool directory;
    char file_name[256];
    NtfsStream *streams;
    size_t stream_count;
    size_t stream_capacity;
    char *error;
} ParsedRecord;

typedef struct {
    const NtfsVolume *volume;
    const uint8_t *raw;
    const uint8_t *read_ok;
    uint64_t first_record;
    size_t record_count;
    ParsedRecord *results;
    size_t worker_index;
    size_t worker_count;
} ParseWorker;

static uint64_t hash_record(uint64_t value) {
    value ^= value >> 33U;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33U;
    value *= UINT64_C(0xc4ceb9fe1a85ec53);
    value ^= value >> 33U;
    return value;
}

static void object_slots_rebuild(ObjectVec *vec, size_t slots) {
    ObjectSlot *table = calloc(slots, sizeof(*table));
    if (table == NULL) ld_die("cannot grow NTFS object index");
    for (size_t index = 0; index < vec->count; ++index) {
        const uint64_t key = vec->items[index].record + 1U;
        size_t slot = (size_t)(hash_record(vec->items[index].record) & (slots - 1U));
        while (table[slot].key_plus_one != 0U) slot = (slot + 1U) & (slots - 1U);
        table[slot].key_plus_one = key;
        table[slot].index_plus_one = index + 1U;
    }
    free(vec->slots);
    vec->slots = table;
    vec->slot_count = slots;
    vec->slot_used = vec->count;
}

static ObjectState *object_get(ObjectVec *vec, uint64_t record) {
    if (vec->slot_count == 0U) object_slots_rebuild(vec, 1024U);
    if ((vec->slot_used + 1U) * 10U >= vec->slot_count * 7U) {
        if (vec->slot_count > SIZE_MAX / 2U) ld_die("NTFS object index is too large");
        object_slots_rebuild(vec, vec->slot_count * 2U);
    }

    const uint64_t key = record + 1U;
    size_t slot = (size_t)(hash_record(record) & (vec->slot_count - 1U));
    while (vec->slots[slot].key_plus_one != 0U) {
        if (vec->slots[slot].key_plus_one == key) {
            return &vec->items[vec->slots[slot].index_plus_one - 1U];
        }
        slot = (slot + 1U) & (vec->slot_count - 1U);
    }

    if (vec->count == SIZE_MAX ||
        !infiltratr_array_reserve((void **)&vec->items, &vec->capacity,
                                  sizeof(*vec->items), vec->count + 1U, 128U))
        ld_die("cannot grow NTFS object catalogue");

    const size_t index = vec->count++;
    ObjectState *item = &vec->items[index];
    memset(item, 0, sizeof(*item));
    item->record = record;

    vec->slots[slot].key_plus_one = key;
    vec->slots[slot].index_plus_one = index + 1U;
    vec->slot_used++;
    return item;
}

static ObjectState *object_find(ObjectVec *vec, uint64_t record) {
    if (vec->slot_count == 0U) return NULL;
    const uint64_t key = record + 1U;
    size_t slot = (size_t)(hash_record(record) & (vec->slot_count - 1U));
    while (vec->slots[slot].key_plus_one != 0U) {
        if (vec->slots[slot].key_plus_one == key)
            return &vec->items[vec->slots[slot].index_plus_one - 1U];
        slot = (slot + 1U) & (vec->slot_count - 1U);
    }
    return NULL;
}

static void object_vec_free(ObjectVec *vec) {
    free(vec->items);
    free(vec->slots);
    memset(vec, 0, sizeof(*vec));
}

static NtfsStream *stream_push(NtfsCatalogue *catalogue) {
    if (catalogue->count == SIZE_MAX ||
        !infiltratr_array_reserve((void **)&catalogue->items, &catalogue->capacity,
                                  sizeof(*catalogue->items),
                                  catalogue->count + 1U, 64U))
        ld_die("cannot grow NTFS stream catalogue");
    NtfsStream *stream = &catalogue->items[catalogue->count++];
    memset(stream, 0, sizeof(*stream));
    return stream;
}

static NtfsStream *parsed_stream_push(ParsedRecord *record) {
    if (record->stream_count == SIZE_MAX ||
        !infiltratr_array_reserve((void **)&record->streams,
                                  &record->stream_capacity,
                                  sizeof(*record->streams),
                                  record->stream_count + 1U, 4U)) {
        record->fatal = true;
        ntfs_set_error(&record->error, "NTFS record stream allocation overflow");
        return NULL;
    }
    NtfsStream *stream = &record->streams[record->stream_count++];
    memset(stream, 0, sizeof(*stream));
    return stream;
}

static int copy_runs(NtfsRunVec *destination, const NtfsRunVec *source) {
    for (size_t index = 0; index < source->count; ++index) {
        const NtfsRun run = source->items[index];
        if (ntfs_runs_push(destination, run.lcn, run.length, run.sparse) != 0)
            return -1;
    }
    return 0;
}

static void best_file_name(const uint8_t *fixed, const NtfsAttributeVec *attrs,
                           uint64_t record, char output[256]) {
    static const char *system_names[] = {
        "$MFT", "$MFTMirr", "$LogFile", "$Volume", "$AttrDef", "$Root",
        "$Bitmap", "$Boot", "$BadClus", "$Secure", "$UpCase", "$Extend"
    };
    output[0] = '\0';
    if (record < sizeof(system_names) / sizeof(system_names[0])) {
        (void)snprintf(output, 256, "%s", system_names[record]);
        return;
    }
    int best_score = -1;
    for (size_t index = 0; index < attrs->count; ++index) {
        const NtfsAttribute *a = &attrs->items[index];
        if (a->type != NTFS_ATTR_FILE_NAME || a->nonresident || a->length < 24U)
            continue;
        const uint32_t value_len = ntfs_u32(fixed, a->offset + 16U);
        const uint16_t value_off = ntfs_u16(fixed, a->offset + 20U);
        if (value_len < 66U || (uint32_t)value_off + value_len > a->length)
            continue;
        const uint8_t *value = fixed + a->offset + value_off;
        const unsigned chars = value[64];
        const unsigned ns = value[65];
        if (66U + chars * 2U > value_len) continue;
        const int score = (ns == 1U || ns == 3U) ? 3 : ns == 0U ? 2 : 1;
        if (score < best_score) continue;
        size_t written = 0;
        for (unsigned pos = 0; pos < chars && written + 1U < 256U; ++pos) {
            const uint16_t ch = ntfs_u16(value + 66U, pos * 2U);
            output[written++] = (ch >= 32U && ch < 127U) ? (char)ch : '?';
        }
        output[written] = '\0';
        best_score = score;
    }
}

static bool desired_attribute(bool directory, const NtfsAttribute *attribute) {
    if (!attribute->nonresident) return false;
    if (directory) return attribute->type == NTFS_ATTR_INDEX_ALLOCATION;
    return attribute->type == NTFS_ATTR_DATA && attribute->name[0] == '\0';
}

static size_t desired_count(bool directory, const NtfsAttributeVec *attributes) {
    size_t count = 0;
    for (size_t index = 0; index < attributes->count; ++index)
        if (desired_attribute(directory, &attributes->items[index])) count++;
    return count;
}

static bool has_attribute_list(const NtfsAttributeVec *attributes) {
    for (size_t index = 0; index < attributes->count; ++index)
        if (attributes->items[index].type == NTFS_ATTR_ATTRIBUTE_LIST) return true;
    return false;
}

static uint32_t mapping_capacity(const NtfsAttribute *attribute) {
    if (attribute->run_offset > attribute->length) return 0;
    return attribute->length - attribute->run_offset;
}

static bool all_physical(const NtfsRunVec *runs) {
    for (size_t index = 0; index < runs->count; ++index)
        if (runs->items[index].sparse) return false;
    return true;
}

static int stream_header_nonzero(NtfsVolume *volume, const NtfsRunVec *runs,
                                 uint64_t data_size, bool *active, char **error) {
    *active = false;
    if (data_size == 0 || runs->count == 0) return 0;
    uint8_t header[4] = {0};
    const size_t take = data_size < sizeof(header) ? (size_t)data_size : sizeof(header);
    if (ntfs_read_stream(volume, runs, 0, header, take, error) != 0) return -1;
    for (size_t index = 0; index < take; ++index) {
        if (header[index] != 0) {
            *active = true;
            break;
        }
    }
    return 0;
}

static bool equal_case_ascii(const char *left, const char *right) {
    while (*left && *right) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right))
            return false;
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static bool stream_is_fragmented(const NtfsStream *stream) {
    return ntfs_fragment_count(&stream->runs) > 1U;
}

static bool growth_reserved_space_is_free(const NtfsLayout *layout,
                                          const ObjectState *object) {
    if (object->growth_streams == 0U) return true;
    if (object->growth_streams != 1U || !object->growth_single_valid) return false;

    const uint64_t clusters = object->growth_clusters;
    const uint64_t reserve = clusters / 10U + (clusters % 10U != 0U ? 1U : 0U);
    uint64_t data_end = 0U;
    uint64_t reserve_end = 0U;
    if (!infiltratr_u64_add_checked(object->growth_start, clusters, &data_end) ||
        data_end > layout->bitmap_bytes * 8U ||
        !infiltratr_u64_add_checked(data_end, reserve, &reserve_end))
        return false;
    for (uint64_t cluster = data_end; cluster < reserve_end; ++cluster)
        if (ntfs_bitmap_bit(layout, cluster)) return false;
    return true;
}

static void parsed_record_free(ParsedRecord *record) {
    for (size_t index = 0; index < record->stream_count; ++index)
        ntfs_runs_free(&record->streams[index].runs);
    free(record->streams);
    free(record->error);
    memset(record, 0, sizeof(*record));
}

static void parse_one_record(ParseWorker *worker, size_t index, uint8_t *fixed) {
    ParsedRecord *result = &worker->results[index];
    result->processed = true;
    result->number = worker->first_record + index;

    if (!worker->read_ok[index]) {
        result->malformed = true;
        return;
    }

    const uint8_t *raw = worker->raw + index * worker->volume->record_size;
    if (memcmp(raw, "FILE", 4) != 0) return;

    char *local_error = NULL;
    if (ntfs_apply_fixups(raw, worker->volume->record_size,
                          worker->volume->bytes_per_sector,
                          fixed, &local_error) != 0) {
        free(local_error);
        result->malformed = true;
        return;
    }
    if ((ntfs_u16(fixed, 22U) & NTFS_RECORD_IN_USE) == 0U) return;

    NtfsAttributeVec attributes = {0};
    if (ntfs_parse_attributes(fixed, worker->volume->record_size,
                              &attributes, &local_error) != 0) {
        free(local_error);
        result->malformed = true;
        return;
    }

    result->in_use = true;
    result->base = ntfs_u64(fixed, 32U) & NTFS_FILE_REFERENCE_MASK;
    result->directory = (ntfs_u16(fixed, 22U) & NTFS_RECORD_DIRECTORY) != 0U;
    best_file_name(fixed, &attributes, result->number, result->file_name);

    const bool attribute_list = has_attribute_list(&attributes);
    const size_t wanted = desired_count(result->directory, &attributes);

    for (size_t attr_index = 0; attr_index < attributes.count; ++attr_index) {
        NtfsAttribute *attribute = &attributes.items[attr_index];
        if (!attribute->nonresident ||
            (attribute->type != NTFS_ATTR_DATA &&
             attribute->type != NTFS_ATTR_INDEX_ALLOCATION))
            continue;

        NtfsStream *stream = parsed_stream_push(result);
        if (stream == NULL) break;
        stream->record_number = result->number;
        stream->base_record = result->base;
        stream->attribute_offset = attribute->offset;
        stream->attribute_type = attribute->type;
        stream->attribute_flags = attribute->flags;
        stream->directory = result->directory;
        stream->lowest_vcn = attribute->lowest_vcn;
        stream->clusters = ntfs_run_clusters(&attribute->runs);
        stream->data_size = attribute->data_size;
        stream->mapping_capacity = mapping_capacity(attribute);
        (void)snprintf(stream->file_name, sizeof(stream->file_name),
                       "%s", result->file_name);
        (void)snprintf(stream->attribute_name, sizeof(stream->attribute_name),
                       "%s", attribute->name);
        if (copy_runs(&stream->runs, &attribute->runs) != 0) {
            result->fatal = true;
            ntfs_set_error(&result->error, "NTFS runlist allocation overflow");
            break;
        }
        stream->movable =
            result->number >= NTFS_FIRST_USER_RECORD &&
            result->base == 0U &&
            !attribute_list &&
            wanted == 1U &&
            desired_attribute(result->directory, attribute) &&
            attribute->lowest_vcn == 0U &&
            (attribute->flags &
             (NTFS_ATTR_COMPRESSED | NTFS_ATTR_ENCRYPTED | NTFS_ATTR_SPARSE)) == 0U &&
            all_physical(&attribute->runs) &&
            attribute->data_size != 0U &&
            stream->mapping_capacity >= 4U;
    }

    ntfs_attributes_free(&attributes);
}

static void *parse_worker_main(void *argument) {
    ParseWorker *worker = argument;
    uint8_t *fixed = ld_xmalloc(worker->volume->record_size);
    for (size_t index = worker->worker_index;
         index < worker->record_count;
         index += worker->worker_count) {
        parse_one_record(worker, index, fixed);
    }
    free(fixed);
    return NULL;
}

static size_t ntfs_catalogue_worker_count(size_t records) {
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    size_t workers = online > 1 ? (size_t)online - 1U : 1U;
    if (workers > records) workers = records;
    return workers == 0U ? 1U : workers;
}

static void parse_batch_parallel(const NtfsVolume *volume,
                                 const uint8_t *raw,
                                 const uint8_t *read_ok,
                                 uint64_t first_record,
                                 size_t record_count,
                                 ParsedRecord *results) {
    const size_t workers = ntfs_catalogue_worker_count(record_count);
    if (workers == 1U) {
        ParseWorker worker = {
            .volume = volume,
            .raw = raw,
            .read_ok = read_ok,
            .first_record = first_record,
            .record_count = record_count,
            .results = results,
            .worker_index = 0U,
            .worker_count = 1U,
        };
        (void)parse_worker_main(&worker);
        return;
    }

    pthread_t *threads = ld_xmalloc(workers * sizeof(*threads));
    ParseWorker *contexts = ld_xmalloc(workers * sizeof(*contexts));
    size_t started = 0U;
    for (; started < workers; ++started) {
        contexts[started] = (ParseWorker){
            .volume = volume,
            .raw = raw,
            .read_ok = read_ok,
            .first_record = first_record,
            .record_count = record_count,
            .results = results,
            .worker_index = started,
            .worker_count = workers,
        };
        if (pthread_create(&threads[started], NULL,
                           parse_worker_main, &contexts[started]) != 0)
            break;
    }
    for (size_t index = 0; index < started; ++index)
        (void)pthread_join(threads[index], NULL);

    if (started < workers) {
        uint8_t *fixed = ld_xmalloc(volume->record_size);
        for (size_t index = 0; index < record_count; ++index)
            if (!results[index].processed)
                parse_one_record(&contexts[0], index, fixed);
        free(fixed);
    }

    free(contexts);
    free(threads);
}

static int read_mft_batch(NtfsVolume *volume, NtfsLayout *layout,
                          uint64_t first_record, size_t record_count,
                          uint8_t *raw, uint8_t *read_ok, char **error) {
    size_t bytes = 0U;
    if (!infiltratr_size_multiply_checked(record_count,
                                          (size_t)volume->record_size,
                                          &bytes)) {
        ntfs_set_error(error, "NTFS MFT batch size overflow");
        return -1;
    }

    uint64_t logical = 0U;
    if (!infiltratr_u64_multiply_checked(first_record, volume->record_size,
                                         &logical)) {
        ntfs_set_error(error, "NTFS MFT batch offset overflow");
        return -1;
    }

    memset(read_ok, 1, record_count);
    if (ntfs_read_stream(volume, &layout->mft_runs, logical,
                         raw, bytes, error) == 0)
        return 0;

    /* A healthy volume takes the fast path above: one large sequential read.
       Preserve the old per-record tolerance only as an I/O recovery fallback. */
    free(*error);
    *error = NULL;
    for (size_t index = 0; index < record_count; ++index) {
        uint64_t record_offset = 0U;
        const uint64_t number = first_record + index;
        if (!infiltratr_u64_multiply_checked(number, volume->record_size,
                                             &record_offset) ||
            ntfs_read_stream(volume, &layout->mft_runs, record_offset,
                             raw + index * volume->record_size,
                             volume->record_size, error) != 0) {
            read_ok[index] = 0U;
            free(*error);
            *error = NULL;
            memset(raw + index * volume->record_size, 0, volume->record_size);
        }
    }
    return 0;
}

static int merge_parsed_record(NtfsVolume *volume,
                               ParsedRecord *record,
                               ObjectVec *objects,
                               NtfsCatalogue *catalogue,
                               char **error) {
    catalogue->records_scanned++;
    if (record->malformed) catalogue->malformed_records++;
    if (record->fatal) {
        ntfs_set_error(error, "%s",
                       record->error != NULL ? record->error :
                       "fatal NTFS catalogue parsing error");
        return -1;
    }
    if (!record->in_use) return 0;

    const uint64_t owner = record->base != 0U ? record->base : record->number;
    ObjectState *object = object_get(objects, owner);
    if (record->base == 0U) {
        object->present = true;
        object->directory = record->directory;
    }

    for (size_t index = 0; index < record->stream_count; ++index) {
        NtfsStream *source = &record->streams[index];

        if (!catalogue->hibernation_active &&
            equal_case_ascii(record->file_name, "hiberfil.sys") &&
            source->attribute_type == NTFS_ATTR_DATA &&
            source->attribute_name[0] == '\0') {
            bool active = false;
            if (stream_header_nonzero(volume, &source->runs,
                                      source->data_size, &active, error) != 0)
                return -1;
            catalogue->hibernation_active = active;
        }

        NtfsStream *destination = stream_push(catalogue);
        *destination = *source;
        memset(&source->runs, 0, sizeof(source->runs));
    }
    return 0;
}

static void aggregate_object_state(NtfsLayout *layout,
                                   NtfsCatalogue *catalogue,
                                   ObjectVec *objects) {
    for (size_t index = 0; index < catalogue->count; ++index) {
        NtfsStream *stream = &catalogue->items[index];
        const uint64_t owner =
            stream->base_record != 0U ? stream->base_record : stream->record_number;
        ObjectState *object = object_find(objects, owner);
        if (object == NULL || !object->present) continue;
        stream->base_record_present = true;

        const bool matches_fragmentation =
            stream->attribute_type ==
                (object->directory ? NTFS_ATTR_INDEX_ALLOCATION : NTFS_ATTR_DATA) &&
            (object->directory || stream->attribute_name[0] == '\0');

        if (matches_fragmentation) {
            object->matching_groups++;
            if (stream->lowest_vcn != object->expected_vcn ||
                stream_is_fragmented(stream))
                object->fragmented = true;
            object->expected_vcn =
                stream->lowest_vcn + ntfs_run_clusters(&stream->runs);
        }

        if (!object->directory &&
            stream->attribute_type == NTFS_ATTR_DATA &&
            stream->attribute_name[0] == '\0') {
            object->growth_streams++;
            if (object->growth_streams == 1U) {
                object->growth_clusters = stream->clusters;
                object->growth_single_valid =
                    ntfs_fragment_count(&stream->runs) == 1U &&
                    !stream->runs.items[0].sparse;
                if (object->growth_single_valid)
                    object->growth_start = stream->runs.items[0].lcn;
            } else {
                object->growth_single_valid = false;
            }
        }
    }

    for (size_t index = 0; index < objects->count; ++index) {
        ObjectState *object = &objects->items[index];
        if (!object->present) continue;
        const bool fragmented =
            object->matching_groups > 1U || object->fragmented;
        if (object->directory) {
            catalogue->directories++;
            if (fragmented) catalogue->fragmented_directories++;
        } else {
            catalogue->regular_files++;
            if (fragmented) catalogue->fragmented_files++;
            if (object->record >= NTFS_FIRST_USER_RECORD &&
                !growth_reserved_space_is_free(layout, object))
                catalogue->growth_10_satisfied = false;
        }
    }
}

int ntfs_scan_catalogue(NtfsVolume *volume, NtfsLayout *layout,
                        NtfsCatalogue *catalogue, char **error) {
    memset(catalogue, 0, sizeof(*catalogue));
    catalogue->growth_10_satisfied = true;

    ObjectVec objects = {0};
    uint64_t record_count = layout->mft_data_size / volume->record_size;
    if (record_count > UINT32_MAX) record_count = UINT32_MAX;
    if (record_count == 0U) {
        catalogue->growth_10_satisfied = false;
        return 0;
    }

    size_t records_per_batch =
        (size_t)(NTFS_MFT_BATCH_BYTES / volume->record_size);
    if (records_per_batch == 0U) records_per_batch = 1U;
    if ((uint64_t)records_per_batch > record_count)
        records_per_batch = (size_t)record_count;

    size_t raw_bytes = 0U;
    if (!infiltratr_size_multiply_checked(records_per_batch,
                                          (size_t)volume->record_size,
                                          &raw_bytes)) {
        ntfs_set_error(error, "NTFS MFT batch allocation overflow");
        return -1;
    }

    uint8_t *raw = ld_xmalloc(raw_bytes);
    uint8_t *read_ok = ld_xmalloc(records_per_batch);
    ParsedRecord *results = calloc(records_per_batch, sizeof(*results));
    if (results == NULL) ld_die("cannot allocate NTFS MFT parse batch");

    int result = 0;
    for (uint64_t first = 0U; first < record_count; first += records_per_batch) {
        const uint64_t remaining = record_count - first;
        const size_t count =
            remaining < records_per_batch ? (size_t)remaining : records_per_batch;

        memset(results, 0, count * sizeof(*results));
        if (read_mft_batch(volume, layout, first, count,
                           raw, read_ok, error) != 0) {
            result = -1;
            break;
        }

        parse_batch_parallel(volume, raw, read_ok, first, count, results);

        for (size_t index = 0; index < count; ++index) {
            if (merge_parsed_record(volume, &results[index], &objects,
                                    catalogue, error) != 0) {
                result = -1;
                break;
            }
        }
        for (size_t index = 0; index < count; ++index)
            parsed_record_free(&results[index]);
        if (result != 0) break;
    }

    if (result == 0) {
        aggregate_object_state(layout, catalogue, &objects);
        if (catalogue->regular_files == 0U ||
            catalogue->fragmented_directories != 0U)
            catalogue->growth_10_satisfied = false;
    }

    free(results);
    free(read_ok);
    free(raw);
    object_vec_free(&objects);

    if (result != 0) ntfs_catalogue_free(catalogue);
    return result;
}

void ntfs_catalogue_free(NtfsCatalogue *catalogue) {
    if (catalogue == NULL) return;
    for (size_t index = 0; index < catalogue->count; ++index)
        ntfs_runs_free(&catalogue->items[index].runs);
    free(catalogue->items);
    memset(catalogue, 0, sizeof(*catalogue));
}
