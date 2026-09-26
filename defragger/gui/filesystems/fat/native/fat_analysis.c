// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * FAT analysis, allocation-map presentation and post-write verification.
 *
 * This read-only module consumes the same Fat32/FileList model used by the
 * writer.  It owns no placement or journal policy.
 */

#include "fat_analysis.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fat_relayout.h"
#include "ld_io.h"
#include "ld_runtime.h"
#include "version.h"

#define PROGRAM_NAME "linux-defragger-fat-worker"
#define PROGRAM_VERSION LD_VERSION

typedef struct {
    uint32_t start;
    uint32_t end;
    uint64_t free_count;
    uint64_t used_count;
    uint64_t fragmented_count;
    uint64_t directory_count;
    uint64_t bad_count;
} LiveMapCell;

static size_t live_map_cells = 0;
static LiveMapCell *live_map_previous = NULL;
static size_t live_map_previous_count = 0;

static uint64_t count_free_clusters(const Fat32 *filesystem) {
    uint64_t count = 0;
    for (uint32_t cluster = 2; cluster <= filesystem->max_cluster; cluster++) {
        if (fat_is_free(filesystem, cluster)) count++;
    }
    return count;
}

static bool cluster_is_movable_allocation(
    const Fat32 *filesystem,
    uint32_t cluster
) {
    uint32_t value = fat_value(filesystem, cluster);
    return value != 0 && value != fat_bad_value(filesystem);
}

static uint64_t terminal_free_clusters(const Fat32 *filesystem) {
    uint64_t count = 0;
    for (uint32_t cluster = filesystem->max_cluster; cluster >= 2; cluster--) {
        if (!fat_is_free(filesystem, cluster)) break;
        count++;
        if (cluster == 2) break;
    }
    return count;
}

static uint64_t holes_below_high_water(
    const Fat32 *filesystem,
    uint32_t *highest_out
) {
    uint32_t highest = 1;
    for (uint32_t cluster = filesystem->max_cluster; cluster >= 2; cluster--) {
        if (cluster_is_movable_allocation(filesystem, cluster)) {
            highest = cluster;
            break;
        }
        if (cluster == 2) break;
    }
    uint64_t holes = 0;
    if (highest >= 2) {
        for (uint32_t cluster = 2; cluster <= highest; cluster++) {
            if (fat_is_free(filesystem, cluster)) holes++;
        }
    }
    *highest_out = highest;
    return holes;
}

void fat_analysis_set_live_map_cells(size_t cell_count) {
    live_map_cells = cell_count;
}

void fat_analysis_reset_live_map(void) {
    free(live_map_previous);
    live_map_previous = NULL;
    live_map_previous_count = 0;
    live_map_cells = 0;
}

static size_t filesystem_root_fragments(Fat32 *fs) {
    if (fs->root_is_fixed) return 1;
    U32Vec chain = fat32_read_chain(fs, fs->root_cluster);
    size_t n = chain_fragments(&chain);
    u32vec_free(&chain);
    return n;
}

void fat_analysis_print(Fat32 *fs, const FileList *files) {
    uint64_t regular = 0, dirs = 0, fragmented = 0, dir_fragmented = 0;
    uint64_t allocated_clusters = 0;
    size_t worst_fragments = 0;
    const char *worst_path = NULL;
    for (size_t i = 0; i < files->len; i++) {
        const FileRecord *f = &files->v[i];
        allocated_clusters += f->chain.len;
        if (f->is_dir) {
            dirs++;
            if (f->fragments > 1) dir_fragmented++;
        } else {
            regular++;
            if (f->fragments > 1) fragmented++;
        }
        if (f->fragments > worst_fragments) {
            worst_fragments = f->fragments;
            worst_path = f->path;
        }
    }
    U32Vec root_chain = filesystem_root_chain(fs);
    size_t root_frags = filesystem_root_fragments(fs);
    allocated_clusters += root_chain.len;
    if (root_frags > worst_fragments) {
        worst_fragments = root_frags;
        worst_path = "<root directory>";
    }
    uint64_t free_clusters = count_free_clusters(fs);

    printf("%s volume ID:        %08" PRIx32 "\n", fat_type_name(fs), fs->volume_id);
    printf("Bytes per sector:       %u\n", fs->bytes_per_sector);
    printf("Sectors per cluster:    %u\n", fs->sectors_per_cluster);
    printf("Cluster size:           %" PRIu64 " bytes\n", fs->cluster_size);
    printf("Data clusters:          %" PRIu32 "\n", fs->cluster_count);
    printf("Free clusters:          %" PRIu64 "\n", free_clusters);
    printf("Scanned regular files:  %" PRIu64 "\n", regular);
    printf("Fragmented files:       %" PRIu64 "\n", fragmented);
    printf("Scanned directories:    %" PRIu64 "\n", dirs + 1);
    printf("Fragmented directories: %" PRIu64 " (root fragments: %zu)\n", dir_fragmented, root_frags);
    printf("Referenced clusters:    %" PRIu64 "\n", allocated_clusters);
    if (worst_path != NULL) printf("Worst chain:             %zu fragments: %s\n", worst_fragments, worst_path);
    u32vec_free(&root_chain);
}

void fat_analysis_list_fragmented(Fat32 *fs, const FileList *files) {
    U32Vec root_chain = filesystem_root_chain(fs);
    size_t root_fragments = filesystem_root_fragments(fs);
    if (root_fragments > 1) {
        printf("DIR   %6zu clusters  %4zu fragments  <root directory>\n",
               root_chain.len, root_fragments);
    }
    u32vec_free(&root_chain);

    for (size_t i = 0; i < files->len; i++) {
        const FileRecord *f = &files->v[i];
        if (f->fragments > 1) {
            printf("%-4s  %6zu clusters  %4zu fragments  %s\n",
                   f->is_dir ? "DIR" : "FILE", f->chain.len, f->fragments, f->path);
        }
    }
}


typedef enum {
    MAP_CLUSTER_USED = 1u << 0,
    MAP_CLUSTER_FRAGMENTED = 1u << 1,
    MAP_CLUSTER_DIRECTORY = 1u << 2,
    MAP_CLUSTER_BAD = 1u << 3
} MapClusterFlag;

typedef struct {
    uint8_t *used;
    uint8_t *fragmented;
    uint8_t *directory;
    uint8_t *bad;
} MapClusterFlags;

static MapClusterFlags map_cluster_flags_create(uint64_t bits) {
    MapClusterFlags flags = {
        .used = ld_bitmap_calloc(bits),
        .fragmented = ld_bitmap_calloc(bits),
        .directory = ld_bitmap_calloc(bits),
        .bad = ld_bitmap_calloc(bits),
    };
    if (flags.used == NULL || flags.fragmented == NULL ||
        flags.directory == NULL || flags.bad == NULL) {
        free(flags.used);
        free(flags.fragmented);
        free(flags.directory);
        free(flags.bad);
        ld_die("cannot allocate packed FAT allocation map");
    }
    return flags;
}

static void map_cluster_flags_free(MapClusterFlags *flags) {
    if (flags == NULL) return;
    free(flags->used);
    free(flags->fragmented);
    free(flags->directory);
    free(flags->bad);
    memset(flags, 0, sizeof(*flags));
}

static uint8_t *map_cluster_flag_bitmap(MapClusterFlags *flags,
                                        MapClusterFlag flag) {
    switch (flag) {
    case MAP_CLUSTER_USED: return flags->used;
    case MAP_CLUSTER_FRAGMENTED: return flags->fragmented;
    case MAP_CLUSTER_DIRECTORY: return flags->directory;
    case MAP_CLUSTER_BAD: return flags->bad;
    }
    ld_die("invalid FAT map flag");
    return NULL;
}

static const uint8_t *map_cluster_flag_bitmap_const(
    const MapClusterFlags *flags, MapClusterFlag flag) {
    switch (flag) {
    case MAP_CLUSTER_USED: return flags->used;
    case MAP_CLUSTER_FRAGMENTED: return flags->fragmented;
    case MAP_CLUSTER_DIRECTORY: return flags->directory;
    case MAP_CLUSTER_BAD: return flags->bad;
    }
    ld_die("invalid FAT map flag");
    return NULL;
}

static void map_cluster_flag_set(MapClusterFlags *flags, uint64_t cluster,
                                 MapClusterFlag flag) {
    ld_bitmap_set(map_cluster_flag_bitmap(flags, flag), cluster, true);
}

static bool map_cluster_flag_test(const MapClusterFlags *flags,
                                  uint64_t cluster, MapClusterFlag flag) {
    return ld_bitmap_get(map_cluster_flag_bitmap_const(flags, flag), cluster);
}

static void json_print_string(const char *value) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; p++) {
        switch (*p) {
            case '"': fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\b': fputs("\\b", stdout); break;
            case '\f': fputs("\\f", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:
                if (*p < 0x20) printf("\\u%04x", (unsigned)*p);
                else putchar((int)*p);
                break;
        }
    }
    putchar('"');
}

/* Blue/purple already identifies the first physical extent of every object.
   Red identifies the displaced extents after a chain discontinuity.  Marking
   every cluster of a fragmented file red made a large file look as though its
   entire allocation were physically fragmented, which was both misleading and
   visually overwhelmed small FAT maps. */
static void mark_map_chain(MapClusterFlags *flags, const U32Vec *chain,
                           bool directory) {
    bool displaced_extent = false;
    for (size_t i = 0; i < chain->len; i++) {
        if (i != 0 && chain->v[i] != chain->v[i - 1] + 1)
            displaced_extent = true;
        map_cluster_flag_set(flags, chain->v[i], MAP_CLUSTER_USED);
        if (directory)
            map_cluster_flag_set(flags, chain->v[i], MAP_CLUSTER_DIRECTORY);
        if (displaced_extent)
            map_cluster_flag_set(flags, chain->v[i], MAP_CLUSTER_FRAGMENTED);
    }
}

static bool growth_layout_satisfied_for_map(Fat32 *fs, const FileList *files,
                                            unsigned percent) {
    FatRelayoutPreflight preflight = fat_relayout_preflight(fs, files, percent);
    bool satisfied = preflight.issue == FAT_RELAYOUT_PREFLIGHT_OK;
    if (!satisfied && preflight.issue != FAT_RELAYOUT_PREFLIGHT_OBJECT_FRAGMENTED &&
        preflight.issue != FAT_RELAYOUT_PREFLIGHT_ROOT_FRAGMENTED) {
        size_t largest = 0;
        uint64_t regular_clusters = 0;
        size_t regular_files = 0;
        size_t directories = 0;
        FatRelayoutObjectList objects = fat_relayout_build_objects(
            fs, files, &largest, &regular_clusters, &regular_files, &directories);
        size_t reserve_clusters = 0;
        (void)largest;
        (void)regular_clusters;
        (void)directories;
        if (regular_files != 0) {
            satisfied = fat_relayout_matches_canonical(
                fs, &objects, percent, &reserve_clusters);
        }
        fat_relayout_object_list_free(&objects);
    }
    fat_relayout_preflight_free(&preflight);
    return satisfied;
}

void fat_analysis_print_map_json(Fat32 *fs, const FileList *files, size_t requested_cells) {
    size_t cells = requested_cells;
    if (cells == 0) cells = 4096;
    if (cells > fs->cluster_count) cells = fs->cluster_count;
    if (cells == 0) cells = 1;

    MapClusterFlags flags = map_cluster_flags_create((uint64_t)fs->max_cluster + 1U);
    uint64_t regular = 0, directories = 1, fragmented_files = 0, fragmented_dirs = 0;
    size_t worst_fragments = 0;
    const char *worst_path = NULL;

    for (uint32_t c = 2; c <= fs->max_cluster; c++) {
        uint32_t v = fat_value(fs, c);
        if (v == 0) continue;
        map_cluster_flag_set(&flags, c, MAP_CLUSTER_USED);
        if (v == fat_bad_value(fs) || (v >= fat_reserved_min(fs) && v < fat_eoc_min(fs))) {
            map_cluster_flag_set(&flags, c, MAP_CLUSTER_BAD);
        }
    }

    U32Vec root_chain = filesystem_root_chain(fs);
    size_t root_fragments = filesystem_root_fragments(fs);
    mark_map_chain(&flags, &root_chain, true);
    if (root_fragments > worst_fragments) {
        worst_fragments = root_fragments;
        worst_path = "<root directory>";
    }

    for (size_t i = 0; i < files->len; i++) {
        const FileRecord *f = &files->v[i];
        if (f->is_dir) {
            directories++;
            if (f->fragments > 1) fragmented_dirs++;
        } else {
            regular++;
            if (f->fragments > 1) fragmented_files++;
        }
        mark_map_chain(&flags, &f->chain, f->is_dir);
        if (f->fragments > worst_fragments) {
            worst_fragments = f->fragments;
            worst_path = f->path;
        }
    }

    uint64_t free_clusters = count_free_clusters(fs);
    uint32_t highest = 1;
    uint64_t gaps = holes_below_high_water(fs, &highest);
    uint64_t terminal = terminal_free_clusters(fs);
    bool growth_10_satisfied = growth_layout_satisfied_for_map(fs, files, 10);

    fputs("{\n", stdout);
    fputs("  \"program\": \"linux-defragger-fat-worker\",\n", stdout);
    printf("  \"version\": \"%s\",\n", PROGRAM_VERSION);
    fputs("  \"device\": ", stdout); json_print_string(fs->dev.path); fputs(",\n", stdout);
    printf("  \"filesystem\": \"%s\",\n", fat_type_name(fs));
    printf("  \"volume_id\": \"%08" PRIx32 "\",\n", fs->volume_id);
    printf("  \"bytes_per_sector\": %u,\n", fs->bytes_per_sector);
    printf("  \"sectors_per_cluster\": %u,\n", fs->sectors_per_cluster);
    printf("  \"cluster_size\": %" PRIu64 ",\n", fs->cluster_size);
    printf("  \"data_clusters\": %" PRIu32 ",\n", fs->cluster_count);
    printf("  \"free_clusters\": %" PRIu64 ",\n", free_clusters);
    printf("  \"used_clusters\": %" PRIu64 ",\n", (uint64_t)fs->cluster_count - free_clusters);
    printf("  \"regular_files\": %" PRIu64 ",\n", regular);
    printf("  \"fragmented_files\": %" PRIu64 ",\n", fragmented_files);
    printf("  \"directories\": %" PRIu64 ",\n", directories);
    printf("  \"fragmented_directories\": %" PRIu64 ",\n", fragmented_dirs);
    printf("  \"root_fragments\": %zu,\n", root_fragments);
    printf("  \"worst_fragments\": %zu,\n", worst_fragments);
    fputs("  \"worst_path\": ", stdout);
    if (worst_path == NULL) fputs("null", stdout); else json_print_string(worst_path);
    fputs(",\n", stdout);
    printf("  \"highest_allocated_cluster\": %" PRIu32 ",\n", highest);
    printf("  \"free_gaps_below_highest\": %" PRIu64 ",\n", gaps);
    printf("  \"terminal_free_clusters\": %" PRIu64 ",\n", terminal);
    printf("  \"growth_10_satisfied\": %s,\n", growth_10_satisfied ? "true" : "false");
    printf("  \"cell_count\": %zu,\n", cells);
    fputs("  \"cells\": [\n", stdout);

    for (size_t i = 0; i < cells; i++) {
        uint64_t first_index = ((uint64_t)i * fs->cluster_count) / cells;
        uint64_t end_index = ((uint64_t)(i + 1) * fs->cluster_count) / cells;
        if (end_index <= first_index) end_index = first_index + 1;
        uint32_t start = (uint32_t)(first_index + 2);
        uint32_t end = (uint32_t)(end_index + 1);
        if (end > fs->max_cluster) end = fs->max_cluster;
        uint64_t free_count = 0, used_count = 0, fragmented_count = 0;
        uint64_t directory_count = 0, bad_count = 0;
        for (uint32_t c = start; c <= end; c++) {
            if (!map_cluster_flag_test(&flags, c, MAP_CLUSTER_USED))
                free_count++;
            else
                used_count++;
            if (map_cluster_flag_test(&flags, c, MAP_CLUSTER_FRAGMENTED))
                fragmented_count++;
            if (map_cluster_flag_test(&flags, c, MAP_CLUSTER_DIRECTORY))
                directory_count++;
            if (map_cluster_flag_test(&flags, c, MAP_CLUSTER_BAD))
                bad_count++;
        }
        printf("    {\"start\":%" PRIu32 ",\"end\":%" PRIu32
               ",\"free\":%" PRIu64 ",\"used\":%" PRIu64
               ",\"fragmented\":%" PRIu64 ",\"directory\":%" PRIu64
               ",\"bad\":%" PRIu64 "}%s\n",
               start, end, free_count, used_count, fragmented_count,
               directory_count, bad_count, i + 1 == cells ? "" : ",");
    }
    fputs("  ]\n}\n", stdout);

    map_cluster_flags_free(&flags);
    u32vec_free(&root_chain);
}

static LiveMapCell *build_live_map_cells(Fat32 *fs, const FileList *files, size_t requested_cells,
                                         size_t *actual_cells, uint64_t *fragmented_files,
                                         uint64_t *fragmented_dirs, uint64_t *free_clusters,
                                         uint64_t *free_gaps) {
    size_t cells = requested_cells == 0 ? 4096 : requested_cells;
    if (cells > fs->cluster_count) cells = fs->cluster_count;
    if (cells == 0) cells = 1;
    MapClusterFlags flags = map_cluster_flags_create((uint64_t)fs->max_cluster + 1U);
    uint64_t ff = 0, fd = 0;
    for (uint32_t c = 2; c <= fs->max_cluster; c++) {
        uint32_t v = fat_value(fs, c);
        if (v == 0) continue;
        map_cluster_flag_set(&flags, c, MAP_CLUSTER_USED);
        if (v == fat_bad_value(fs) || (v >= fat_reserved_min(fs) && v < fat_eoc_min(fs)))
            map_cluster_flag_set(&flags, c, MAP_CLUSTER_BAD);
    }
    U32Vec root_chain = filesystem_root_chain(fs);
    size_t root_fragments = filesystem_root_fragments(fs);
    mark_map_chain(&flags, &root_chain, true);
    if (root_fragments > 1) fd++;
    for (size_t i = 0; i < files->len; i++) {
        const FileRecord *f = &files->v[i];
        if (f->is_dir) { if (f->fragments > 1) fd++; }
        else { if (f->fragments > 1) ff++; }
        mark_map_chain(&flags, &f->chain, f->is_dir);
    }
    LiveMapCell *out = ld_xcalloc(cells, sizeof(*out));
    for (size_t i = 0; i < cells; i++) {
        uint64_t first_index = ((uint64_t)i * fs->cluster_count) / cells;
        uint64_t end_index = ((uint64_t)(i + 1) * fs->cluster_count) / cells;
        if (end_index <= first_index) end_index = first_index + 1;
        uint32_t start = (uint32_t)(first_index + 2);
        uint32_t end = (uint32_t)(end_index + 1);
        if (end > fs->max_cluster) end = fs->max_cluster;
        out[i].start = start; out[i].end = end;
        for (uint32_t c = start; c <= end; c++) {
            if (!map_cluster_flag_test(&flags, c, MAP_CLUSTER_USED))
                out[i].free_count++;
            else
                out[i].used_count++;
            if (map_cluster_flag_test(&flags, c, MAP_CLUSTER_FRAGMENTED))
                out[i].fragmented_count++;
            if (map_cluster_flag_test(&flags, c, MAP_CLUSTER_DIRECTORY))
                out[i].directory_count++;
            if (map_cluster_flag_test(&flags, c, MAP_CLUSTER_BAD))
                out[i].bad_count++;
        }
    }
    map_cluster_flags_free(&flags);
    u32vec_free(&root_chain);
    uint32_t highest = 1;
    if (actual_cells) *actual_cells = cells;
    if (fragmented_files) *fragmented_files = ff;
    if (fragmented_dirs) *fragmented_dirs = fd;
    if (free_clusters) *free_clusters = count_free_clusters(fs);
    if (free_gaps) *free_gaps = holes_below_high_water(fs, &highest);
    return out;
}

void fat_analysis_initialise_live_map(Fat32 *fs, const FileList *files) {
    if (live_map_cells == 0) return;
    free(live_map_previous);
    live_map_previous = build_live_map_cells(fs, files, live_map_cells,
                                               &live_map_previous_count, NULL, NULL, NULL, NULL);
}

void fat_analysis_emit_live_map_update(Fat32 *fs) {
    if (live_map_cells == 0) return;
    DirRefList refs = {0};
    FileList files = scan_files(fs, &refs);
    size_t count = 0;
    uint64_t fragmented_files = 0, fragmented_dirs = 0, free_clusters = 0, free_gaps = 0;
    LiveMapCell *now = build_live_map_cells(fs, &files, live_map_cells, &count,
                                            &fragmented_files, &fragmented_dirs,
                                            &free_clusters, &free_gaps);
    fputs("@@LIVE_MAP {\"fragmented_files\":", stderr);
    fprintf(stderr, "%" PRIu64 ",\"fragmented_directories\":%" PRIu64
                    ",\"free_clusters\":%" PRIu64 ",\"free_gaps_below_highest\":%" PRIu64
                    ",\"cells\":[",
            fragmented_files, fragmented_dirs, free_clusters, free_gaps);
    bool first = true;
    for (size_t i = 0; i < count; i++) {
        bool changed = live_map_previous == NULL || i >= live_map_previous_count ||
                       memcmp(&now[i], &live_map_previous[i], sizeof(now[i])) != 0;
        if (!changed) continue;
        fprintf(stderr, "%s{\"i\":%zu,\"start\":%" PRIu32 ",\"end\":%" PRIu32
                        ",\"free\":%" PRIu64 ",\"used\":%" PRIu64
                        ",\"fragmented\":%" PRIu64 ",\"directory\":%" PRIu64
                        ",\"bad\":%" PRIu64 "}",
                first ? "" : ",", i, now[i].start, now[i].end, now[i].free_count,
                now[i].used_count, now[i].fragmented_count, now[i].directory_count,
                now[i].bad_count);
        first = false;
    }
    fputs("]}\n", stderr);
    fflush(stderr);
    free(live_map_previous);
    live_map_previous = now;
    live_map_previous_count = count;
    filelist_free(&files);
    dirreflist_free(&refs);
}

void fat_analysis_verify_layout_policy(Fat32 *fs, unsigned reserve_percent) {
    DirRefList refs = {0};
    FileList files = scan_files(fs, &refs);
    U32Vec root_chain = filesystem_root_chain(fs);
    if (chain_fragments(&root_chain) > 1) {
        u32vec_free(&root_chain);
        filelist_free(&files);
        dirreflist_free(&refs);
        ld_die("layout verification failed: the root directory is fragmented");
    }
    u32vec_free(&root_chain);
    for (size_t i = 0; i < files.len; i++) {
        if (files.v[i].chain.len != 0 && files.v[i].fragments > 1) {
            fprintf(stderr, "%s: layout verification failed: %s remains fragmented\n",
                    PROGRAM_NAME, files.v[i].path);
            filelist_free(&files);
            dirreflist_free(&refs);
            exit(EXIT_FAILURE);
        }
    }

    size_t largest = 0, regular_files = 0, directories = 0;
    uint64_t regular_clusters = 0;
    FatRelayoutObjectList objects = fat_relayout_build_objects(
        fs, &files, &largest, &regular_clusters, &regular_files, &directories);
    size_t reserve_clusters = 0;
    bool canonical = fat_relayout_matches_canonical(
        fs, &objects, reserve_percent, &reserve_clusters);
    fat_relayout_object_list_free(&objects);
    if (!canonical && (regular_files != 0 || directories != 0)) {
        filelist_free(&files);
        dirreflist_free(&refs);
        ld_die("layout verification failed: allocations do not match the canonical earliest layout");
    }

    uint32_t highest = 1;
    uint64_t holes = holes_below_high_water(fs, &highest);
    if (reserve_percent == 0 && holes != 0) {
        fprintf(stderr,
                "%s: layout verification failed: %" PRIu64
                " free cluster%s remain below highest allocated cluster %" PRIu32 "\n",
                PROGRAM_NAME, holes, holes == 1 ? "" : "s", highest);
        filelist_free(&files);
        dirreflist_free(&refs);
        exit(EXIT_FAILURE);
    }

    printf("Layout verification:      %zu file%s and %zu director%s contiguous; ",
           regular_files, regular_files == 1 ? "" : "s",
           directories, directories == 1 ? "y" : "ies");
    if (reserve_percent == 0) {
        printf("no free clusters below the final allocation\n");
    } else {
        printf("canonical %u%% growth gaps verified (%zu clusters)\n",
               reserve_percent, reserve_clusters);
    }
    filelist_free(&files);
    dirreflist_free(&refs);
}

