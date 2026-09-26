// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Defragmenter engine
 * Author: Shannon Smith
 *
 * Implements FAT12, FAT16 and FAT32 analysis and canonical packed defragmentation,
 * growth-space defragmentation, transaction journalling and recovery. This native
 * worker is owned by the authoritative GUI FAT plugin and remains independently
 * testable without creating a second filesystem registry.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "ld_device.h"
#include "ld_io.h"
#include "ld_runtime.h"
#include "ld_protocol.h"
#include "ld_stop.h"
#include "infiltratr/arithmetic.h"
#include "infiltratr/core.h"
#include "infiltratr/posix.h"
#include "infiltratr/posix_path.h"
#include "infiltratr/quantity.h"
#include "version.h"
#include "fat_analysis.h"
#include "fat_directory.h"
#include "fat_relayout.h"
#include "fat_io.h"
#include "fat_journal.h"
#include "fat_relocation.h"
#include "fat_volume.h"

#define PROGRAM_NAME "linux-defragger-fat-worker"
#define PROGRAM_VERSION LD_VERSION

/* Runtime I/O policy and counters used by the buffered relocation pipeline. */
static FatIoConfig g_io;
static bool g_verbose = false;
static FILE *g_diagnostic_log = NULL;

static double elapsed_since(double started) {
    double finished = infiltratr_monotonic_seconds();
    if (started <= 0.0 || finished < started) return 0.0;
    return finished - started;
}

static void detail_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    if (g_verbose) {
        va_list copy;
        va_copy(copy, args);
        vfprintf(stderr, format, copy);
        va_end(copy);
    }
    if (g_diagnostic_log != NULL) {
        vfprintf(g_diagnostic_log, format, args);
        fflush(g_diagnostic_log);
    }
    va_end(args);
}

static char *default_journal_path(const char *device_path) {
    const char *base = infiltratr_path_basename(device_path);
    size_t n = 0U;
    if (!infiltratr_size_add_checked(strlen(base), 40U, &n))
        ld_die("default FAT journal path is too long");
    char *path = ld_xmalloc(n);
    snprintf(path, n, ".linux-defragger-fat-worker-%s.journal", base);
    return path;
}

static void reserve_relocation_moves(RelocationMove **moves, size_t *capacity,
                                     size_t count, size_t additional) {
    size_t required = 0U;
    if (!infiltratr_size_add_checked(count, additional, &required) ||
        !infiltratr_array_reserve((void **)moves, capacity, sizeof(**moves),
                                  required, additional == 0U ? 1U : additional))
        ld_die("cannot grow FAT relocation move array");
}

static bool cluster_is_movable_allocation(const Fat32 *fs, uint32_t cluster) {
    uint32_t value = fat_value(fs, cluster);
    return value != 0 && value != fat_bad_value(fs) &&
           !(value >= fat_reserved_min(fs) && value < fat_eoc_min(fs));
}

static uint64_t count_free_clusters(const Fat32 *fs) {
    uint64_t count = 0;
    for (uint32_t cluster = 2; cluster <= fs->max_cluster; cluster++) {
        if (fat_is_free(fs, cluster)) count++;
    }
    return count;
}

static uint64_t terminal_free_clusters(const Fat32 *fs) {
    uint64_t free_count = 0;
    for (uint32_t c = fs->max_cluster; c >= 2; c--) {
        if (!fat_is_free(fs, c)) break;
        free_count++;
        if (c == 2) break;
    }
    return free_count;
}

static size_t terminal_workspace_capacity(const Fat32 *fs) {
    size_t capacity = 0;
    for (uint32_t c = fs->max_cluster; c >= 2; c--) {
        uint32_t value = fat_value(fs, c);
        if (value == fat_bad_value(fs) ||
            (value >= fat_reserved_min(fs) && value < fat_eoc_min(fs))) {
            break;
        }
        capacity++;
        if (c == 2) break;
    }
    return capacity;
}

static size_t relayout_object_cluster_total(const FatRelayoutObjectList *objects) {
    size_t total = 0;
    for (size_t i = 0; i < objects->len; i++) {
        if (!infiltratr_size_add_checked(total, objects->v[i].clusters, &total))
            ld_die("FAT relayout object-cluster total overflow");
    }
    return total;
}

typedef struct {
    size_t clusters_moved;
    size_t transactions;
} PackingStats;

static void relocation_execute_moves(Fat32 *fs, const DirRefList *dir_refs,
                                     const char *journal_path,
                                     const RelocationMove *moves,
                                     size_t move_count) {
    fat_relocation_execute(
        fs, dir_refs, journal_path, moves, move_count, &g_io, detail_log
    );
    fat_analysis_emit_live_map_update(fs);
}

/* Canonical relayout keeps one durable terminal safety workspace.  The workspace
   is sized from the actual RAM budget and spare tail capacity, not from a fixed
   object count.  Direct moves use RAM first; the terminal copy is used only when
   overlapping live dependencies must survive power loss. */
static PackingStats prepare_terminal_workspace(Fat32 *fs, const char *journal_path,
                                                uint32_t workspace_start,
                                                size_t workspace_clusters,
                                                size_t batch_clusters) {
    PackingStats stats = {0};
    if (batch_clusters == 0) batch_clusters = 1;
    if (workspace_clusters == 0 || workspace_start < 2 ||
        (uint64_t)workspace_start + workspace_clusters - 1 > fs->max_cluster) {
        ld_die("invalid terminal staging workspace");
    }

    for (;;) {
        if (ld_stop_requested()) {
            fprintf(stderr,
                    "interrupt requested; stopping workspace preparation between transactions\n");
            break;
        }

        DirRefList dir_refs = {0};
        FileList files = scan_files(fs, &dir_refs);
        size_t move_cap = batch_clusters < workspace_clusters
                              ? batch_clusters : workspace_clusters;
        RelocationMove *moves = ld_xmalloc(move_cap * sizeof(*moves));
        size_t move_count = 0;
        uint32_t destination = 2;
        for (uint32_t source = workspace_start;
             source <= fs->max_cluster && move_count < move_cap; source++) {
            if (!cluster_is_movable_allocation(fs, source)) continue;
            while (destination < workspace_start && !fat_is_free(fs, destination)) {
                destination++;
            }
            if (destination >= workspace_start) {
                free(moves);
                filelist_free(&files);
                dirreflist_free(&dir_refs);
                ld_die("terminal workspace evacuation ran out of free clusters below the workspace");
            }
            moves[move_count++] = (RelocationMove){
                .source = source,
                .destination = destination,
            };
            destination++;
        }

        if (move_count == 0) {
            free(moves);
            filelist_free(&files);
            dirreflist_free(&dir_refs);
            break;
        }

        detail_log("workspace-evacuate: %zu cluster%s from terminal range %" PRIu32
                   "-%" PRIu32 "\n",
                   move_count, move_count == 1 ? "" : "s", workspace_start,
                   fs->max_cluster);
        relocation_execute_moves(fs, &dir_refs, journal_path, moves, move_count);
        stats.transactions++;
        stats.clusters_moved += move_count;
        fprintf(stderr,
                "workspace: evacuated %zu cluster%s (total %zu); terminal free run "
                "now %" PRIu64 " clusters\n",
                move_count, move_count == 1 ? "" : "s", stats.clusters_moved,
                terminal_free_clusters(fs));
        free(moves);
        filelist_free(&files);
        dirreflist_free(&dir_refs);
    }

    fat_relocation_update_fsinfo(fs, fat_relocation_first_free_hint(fs));
    fat32_sync(fs);
    return stats;
}

static bool cluster_range_is_free(const Fat32 *fs, uint32_t start, size_t length) {
    if (length == 0) return false;
    uint64_t end64 = (uint64_t)start + length - 1;
    if (start < 2 || end64 > fs->max_cluster) return false;
    for (size_t i = 0; i < length; i++) {
        if (!fat_is_free(fs, start + (uint32_t)i)) return false;
    }
    return true;
}

static const U32Vec *find_relayout_object_chain(Fat32 *fs, const FatRelayoutObject *object,
                                              FileList *files, U32Vec *root_chain,
                                              const FileRecord **file_out) {
    *file_out = NULL;
    if (object->is_root) {
        *root_chain = filesystem_root_chain(fs);
        return root_chain;
    }
    for (size_t i = 0; i < files->len; i++) {
        if (strcmp(files->v[i].path, object->path) == 0 &&
            files->v[i].is_dir == object->is_dir) {
            *file_out = &files->v[i];
            return &files->v[i].chain;
        }
    }
    ld_die("layout object disappeared during rescan");
    return NULL;
}

static uint32_t current_chain_min_cluster(const U32Vec *chain) {
    uint32_t minimum = UINT32_MAX;
    for (size_t i = 0; i < chain->len; i++) {
        if (chain->v[i] < minimum) minimum = chain->v[i];
    }
    return minimum;
}

static void relayout_move_chain(Fat32 *fs, const DirRefList *dir_refs,
                              const U32Vec *chain, uint32_t destination,
                              const char *journal_path, bool emit_map) {
    RelocationMove *moves = ld_xmalloc(chain->len * sizeof(*moves));
    for (size_t i = 0; i < chain->len; i++) {
        moves[i] = (RelocationMove){
            .source = chain->v[i],
            .destination = destination + (uint32_t)i,
        };
    }
    fat_relocation_execute(
        fs, dir_refs, journal_path, moves, chain->len, &g_io, detail_log
    );
    free(moves);
    if (emit_map) fat_analysis_emit_live_map_update(fs);
}

static size_t automatic_relayout_batch_clusters(const Fat32 *fs) {
    size_t by_ram = g_io.ram_limit / (size_t)fs->cluster_size;
    size_t four_gb = (size_t)(UINT64_C(4) * 1024 * 1024 * 1024 / fs->cluster_size);
    if (by_ram > four_gb) by_ram = four_gb;
    if (by_ram == 0) by_ram = 1;
    return by_ram;
}

static bool relayout_batch_can_add(const Fat32 *fs, const U32Vec *chain,
                                 uint32_t destination,
                                 const uint8_t *source_seen,
                                 const uint8_t *destination_seen) {
    if (!cluster_range_is_free(fs, destination, chain->len)) return false;
    for (size_t i = 0; i < chain->len; i++) {
        uint32_t source = chain->v[i];
        uint32_t target = destination + (uint32_t)i;
        if (source_seen[source] || destination_seen[source] ||
            source_seen[target] || destination_seen[target]) return false;
    }
    return true;
}

typedef struct {
    size_t object_index;
    uint32_t staged_at;
} WorkspaceStageItem;

/* A leftward canonical plan is cheapest when walked from the front.  This is a
   plan property, not a Defrag-vs-Growth distinction.  Consecutive dependencies
   are durably staged as one batch and then placed in one second transaction. */
static bool execute_forward_compaction(
    Fat32 *fs,
    const char *journal_path,
    FatRelayoutObjectList *objects,
    uint32_t workspace_start,
    size_t workspace_clusters,
    size_t object_batch_limit,
    size_t cluster_batch_limit,
    const char *layout_name,
    FatRelayoutStats *stats
) {
    size_t next = 0;
    size_t remaining = objects->len;

    while (next < objects->len) {
        if (ld_stop_requested()) {
            stats->interrupted = true;
            fprintf(stderr,
                    "%s stopped safely during layout between complete batches.\n",
                    layout_name);
            return true;
        }

        DirRefList current_refs = {0};
        FileList current_files = scan_files(fs, &current_refs);

        while (next < objects->len) {
            FatRelayoutObject *object = &objects->v[next];
            const FileRecord *current_file = NULL;
            U32Vec local_root = {0};
            const U32Vec *chain = find_relayout_object_chain(
                fs, object, &current_files, &local_root, &current_file);
            (void)current_file;
            if (chain->len != object->clusters) {
                u32vec_free(&local_root);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                ld_die("layout object changed size during forward compaction");
            }
            bool exact = chain_is_exact_run(chain, object->target);
            u32vec_free(&local_root);
            if (!exact) break;
            next++;
            remaining--;
        }
        if (next == objects->len) {
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            break;
        }

        WorkspaceStageItem *items = ld_xmalloc(
            object_batch_limit * sizeof(*items));
        RelocationMove *stage_moves = NULL;
        size_t stage_move_count = 0;
        size_t stage_move_cap = 0;
        size_t batch_objects = 0;
        size_t batch_files = 0;
        size_t batch_directories = 0;
        uint8_t *source_seen = ld_xcalloc(
            (size_t)fs->max_cluster + 1, 1);

        size_t candidate = next;
        while (candidate < objects->len &&
               batch_objects < object_batch_limit) {
            FatRelayoutObject *object = &objects->v[candidate];
            const FileRecord *current_file = NULL;
            U32Vec local_root = {0};
            const U32Vec *chain = find_relayout_object_chain(
                fs, object, &current_files, &local_root, &current_file);
            (void)current_file;
            if (chain->len != object->clusters) {
                u32vec_free(&local_root);
                free(source_seen);
                free(stage_moves);
                free(items);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                ld_die("layout object changed size during forward compaction");
            }
            if (chain_is_exact_run(chain, object->target)) {
                u32vec_free(&local_root);
                break;
            }
            if (object->target > current_chain_min_cluster(chain)) {
                u32vec_free(&local_root);
                free(source_seen);
                free(stage_moves);
                free(items);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                return false;
            }
            if (batch_objects != 0 &&
                (stage_move_count >= cluster_batch_limit ||
                 object->clusters > workspace_clusters - stage_move_count ||
                 object->clusters > cluster_batch_limit - stage_move_count)) {
                u32vec_free(&local_root);
                break;
            }
            if (batch_objects == 0 && object->clusters > workspace_clusters) {
                u32vec_free(&local_root);
                free(source_seen);
                free(stage_moves);
                free(items);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                return false;
            }

            reserve_relocation_moves(
                &stage_moves, &stage_move_cap, stage_move_count, chain->len);
            uint32_t staged_at = workspace_start + (uint32_t)stage_move_count;
            for (size_t i = 0; i < chain->len; i++) {
                uint32_t source = chain->v[i];
                stage_moves[stage_move_count++] = (RelocationMove){
                    .source = source,
                    .destination = staged_at + (uint32_t)i,
                };
                source_seen[source] = 1;
            }
            items[batch_objects++] = (WorkspaceStageItem){
                .object_index = candidate,
                .staged_at = staged_at,
            };
            if (object->is_dir) batch_directories++;
            else batch_files++;
            candidate++;
            u32vec_free(&local_root);
        }

        bool targets_release_with_batch = batch_objects != 0;
        for (size_t item_index = 0;
             item_index < batch_objects && targets_release_with_batch;
             item_index++) {
            const FatRelayoutObject *object =
                &objects->v[items[item_index].object_index];
            uint64_t target_end64 =
                (uint64_t)object->target + object->clusters - 1;
            if (target_end64 > fs->max_cluster) {
                targets_release_with_batch = false;
                break;
            }
            for (uint32_t cluster = object->target;
                 cluster <= (uint32_t)target_end64; cluster++) {
                if (!fat_is_free(fs, cluster) && !source_seen[cluster]) {
                    targets_release_with_batch = false;
                    break;
                }
                if (cluster == UINT32_MAX) break;
            }
        }
        free(source_seen);
        if (!targets_release_with_batch) {
            free(stage_moves);
            free(items);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            return false;
        }

        detail_log(
            "layout-forward-stage: %zu object%s / %zu cluster%s -> workspace "
            "cluster %" PRIu32 "\n",
            batch_objects, batch_objects == 1 ? "" : "s",
            stage_move_count, stage_move_count == 1 ? "" : "s",
            workspace_start);
        fat_relocation_execute(
            fs, &current_refs, journal_path, stage_moves, stage_move_count,
            &g_io, detail_log);
        stats->transactions++;
        stats->clusters_copied += stage_move_count;
        free(stage_moves);
        filelist_free(&current_files);
        dirreflist_free(&current_refs);

        if (ld_stop_requested()) {
            stats->interrupted = true;
            fprintf(stderr,
                    "%s stopped safely after durable workspace staging; final placement "
                    "for this batch was not started.\n",
                    layout_name);
            free(items);
            return true;
        }

        current_refs = (DirRefList){0};
        current_files = scan_files(fs, &current_refs);
        RelocationMove *place_moves = ld_xmalloc(
            stage_move_count * sizeof(*place_moves));
        size_t place_move_count = 0;
        for (size_t item_index = 0; item_index < batch_objects; item_index++) {
            FatRelayoutObject *object = &objects->v[items[item_index].object_index];
            const FileRecord *current_file = NULL;
            U32Vec local_root = {0};
            const U32Vec *chain = find_relayout_object_chain(
                fs, object, &current_files, &local_root, &current_file);
            (void)current_file;
            if (!chain_is_exact_run(chain, items[item_index].staged_at) ||
                !cluster_range_is_free(fs, object->target, object->clusters)) {
                u32vec_free(&local_root);
                free(place_moves);
                free(items);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                ld_die("forward compaction batch could not reopen its released targets");
            }
            for (size_t i = 0; i < chain->len; i++) {
                place_moves[place_move_count++] = (RelocationMove){
                    .source = chain->v[i],
                    .destination = object->target + (uint32_t)i,
                };
            }
            u32vec_free(&local_root);
        }

        detail_log(
            "layout-forward-place: %zu object%s / %zu cluster%s from workspace\n",
            batch_objects, batch_objects == 1 ? "" : "s",
            place_move_count, place_move_count == 1 ? "" : "s");
        relocation_execute_moves(
            fs, &current_refs, journal_path, place_moves, place_move_count);
        stats->transactions++;
        stats->clusters_copied += place_move_count;
        stats->objects_moved += batch_objects;
        stats->files_moved += batch_files;
        stats->directories_moved += batch_directories;
        next += batch_objects;
        remaining -= batch_objects;
        fprintf(stderr,
                "%s forward layout batch: %zu object%s (%zu file%s, "
                "%zu director%s), %zu cluster%s staged and placed in 2 journal "
                "transactions; %zu object%s %s.\n",
                layout_name, batch_objects, batch_objects == 1 ? "" : "s",
                batch_files, batch_files == 1 ? "" : "s",
                batch_directories, batch_directories == 1 ? "y" : "ies",
                place_move_count, place_move_count == 1 ? "" : "s",
                remaining, remaining == 1 ? "" : "s",
                remaining == 1 ? "remains" : "remain");
        free(place_moves);
        free(items);
        filelist_free(&current_files);
        dirreflist_free(&current_refs);
    }
    return true;
}

/* If every outstanding object fits inside the durable workspace and the RAM
   budget, evacuate the entire dependency set together.  This is the safe form
   of the "hold everything in RAM" optimisation: RAM coalesces the I/O, while
   the terminal copy preserves recoverability if power fails before placement. */
static bool execute_full_workspace_layout(
    Fat32 *fs,
    const char *journal_path,
    FatRelayoutObjectList *objects,
    uint32_t workspace_start,
    size_t workspace_clusters,
    size_t cluster_batch_limit,
    const char *layout_name,
    FatRelayoutStats *stats
) {
    DirRefList current_refs = {0};
    FileList current_files = scan_files(fs, &current_refs);
    size_t moving_objects = 0;
    size_t moving_clusters = 0;
    size_t moving_files = 0;
    size_t moving_directories = 0;

    for (size_t index = 0; index < objects->len; index++) {
        FatRelayoutObject *object = &objects->v[index];
        const FileRecord *current_file = NULL;
        U32Vec local_root = {0};
        const U32Vec *chain = find_relayout_object_chain(
            fs, object, &current_files, &local_root, &current_file);
        (void)current_file;
        if (chain->len != object->clusters) {
            u32vec_free(&local_root);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            ld_die("layout object changed size during unified workspace planning");
        }
        if (!chain_is_exact_run(chain, object->target)) {
            if (chain->len > SIZE_MAX - moving_clusters) {
                u32vec_free(&local_root);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                ld_die("unified FAT workspace cluster total overflow");
            }
            moving_clusters += chain->len;
            moving_objects++;
            if (object->is_dir) moving_directories++;
            else moving_files++;
        }
        u32vec_free(&local_root);
    }

    if (moving_objects == 0) {
        filelist_free(&current_files);
        dirreflist_free(&current_refs);
        return true;
    }
    if (moving_clusters > workspace_clusters ||
        moving_clusters > cluster_batch_limit) {
        filelist_free(&current_files);
        dirreflist_free(&current_refs);
        return false;
    }
    if (!cluster_range_is_free(fs, workspace_start, moving_clusters)) {
        filelist_free(&current_files);
        dirreflist_free(&current_refs);
        ld_die("unified FAT workspace is unexpectedly occupied");
    }

    WorkspaceStageItem *items = ld_xmalloc(
        moving_objects * sizeof(*items));
    RelocationMove *stage_moves = ld_xmalloc(
        moving_clusters * sizeof(*stage_moves));
    size_t item_count = 0;
    size_t move_count = 0;
    uint32_t staged_cursor = workspace_start;

    for (size_t index = 0; index < objects->len; index++) {
        FatRelayoutObject *object = &objects->v[index];
        const FileRecord *current_file = NULL;
        U32Vec local_root = {0};
        const U32Vec *chain = find_relayout_object_chain(
            fs, object, &current_files, &local_root, &current_file);
        (void)current_file;
        if (chain_is_exact_run(chain, object->target)) {
            u32vec_free(&local_root);
            continue;
        }
        items[item_count++] = (WorkspaceStageItem){
            .object_index = index,
            .staged_at = staged_cursor,
        };
        for (size_t i = 0; i < chain->len; i++) {
            stage_moves[move_count++] = (RelocationMove){
                .source = chain->v[i],
                .destination = staged_cursor + (uint32_t)i,
            };
        }
        staged_cursor += (uint32_t)chain->len;
        u32vec_free(&local_root);
    }

    detail_log(
        "layout-unified-stage: %zu object%s / %zu cluster%s -> workspace "
        "cluster %" PRIu32 "\n",
        item_count, item_count == 1 ? "" : "s",
        move_count, move_count == 1 ? "" : "s", workspace_start);
    fat_relocation_execute(
        fs, &current_refs, journal_path, stage_moves, move_count,
        &g_io, detail_log);
    stats->transactions++;
    stats->clusters_copied += move_count;
    free(stage_moves);
    filelist_free(&current_files);
    dirreflist_free(&current_refs);

    if (ld_stop_requested()) {
        stats->interrupted = true;
        fprintf(stderr,
                "%s stopped safely after durable unified workspace staging; final "
                "placement was not started.\n",
                layout_name);
        free(items);
        return true;
    }

    current_refs = (DirRefList){0};
    current_files = scan_files(fs, &current_refs);
    RelocationMove *place_moves = ld_xmalloc(
        moving_clusters * sizeof(*place_moves));
    size_t place_count = 0;
    for (size_t item_index = 0; item_index < item_count; item_index++) {
        FatRelayoutObject *object = &objects->v[items[item_index].object_index];
        const FileRecord *current_file = NULL;
        U32Vec local_root = {0};
        const U32Vec *chain = find_relayout_object_chain(
            fs, object, &current_files, &local_root, &current_file);
        (void)current_file;
        if (!chain_is_exact_run(chain, items[item_index].staged_at)) {
            u32vec_free(&local_root);
            free(place_moves);
            free(items);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            ld_die("unified FAT workspace object did not reopen at its staged location");
        }
        if (!cluster_range_is_free(fs, object->target, object->clusters)) {
            u32vec_free(&local_root);
            free(place_moves);
            free(items);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            ld_die("unified FAT workspace did not release every canonical target");
        }
        for (size_t i = 0; i < chain->len; i++) {
            place_moves[place_count++] = (RelocationMove){
                .source = chain->v[i],
                .destination = object->target + (uint32_t)i,
            };
        }
        u32vec_free(&local_root);
    }

    detail_log(
        "layout-unified-place: %zu object%s / %zu cluster%s from workspace\n",
        item_count, item_count == 1 ? "" : "s",
        place_count, place_count == 1 ? "" : "s");
    relocation_execute_moves(
        fs, &current_refs, journal_path, place_moves, place_count);
    stats->transactions++;
    stats->clusters_copied += place_count;
    stats->objects_moved += moving_objects;
    stats->files_moved += moving_files;
    stats->directories_moved += moving_directories;
    fprintf(stderr,
            "%s unified workspace batch: %zu object%s (%zu file%s, %zu director%s), "
            "%zu cluster%s staged and placed in 2 journal transactions.\n",
            layout_name, moving_objects, moving_objects == 1 ? "" : "s",
            moving_files, moving_files == 1 ? "" : "s",
            moving_directories, moving_directories == 1 ? "y" : "ies",
            place_count, place_count == 1 ? "" : "s");

    free(place_moves);
    free(items);
    filelist_free(&current_files);
    dirreflist_free(&current_refs);
    return true;
}


static bool chain_is_inside_workspace(const U32Vec *chain,
                            uint32_t workspace_start,
                            size_t workspace_clusters) {
    if (chain->len == 0 || workspace_clusters == 0) return false;
    uint64_t workspace_end =
        (uint64_t)workspace_start + workspace_clusters - 1;
    for (size_t i = 0; i < chain->len; i++) {
        uint64_t cluster = chain->v[i];
        if (cluster < workspace_start || cluster > workspace_end) return false;
    }
    return true;
}

static size_t largest_free_workspace_run(const Fat32 *fs,
                               uint32_t workspace_start,
                               size_t workspace_clusters) {
    size_t best = 0;
    size_t run = 0;
    for (size_t offset = 0; offset < workspace_clusters; offset++) {
        uint32_t cluster = workspace_start + (uint32_t)offset;
        if (fat_is_free(fs, cluster)) {
  run++;
  if (run > best) best = run;
        } else {
  run = 0;
        }
    }
    return best;
}

static uint32_t find_free_workspace_run(const Fat32 *fs,
                              uint32_t workspace_start,
                              size_t workspace_clusters,
                              size_t length) {
    if (length == 0 || length > workspace_clusters) return 0;
    size_t run = 0;
    for (size_t offset = 0; offset < workspace_clusters; offset++) {
        uint32_t cluster = workspace_start + (uint32_t)offset;
        if (fat_is_free(fs, cluster)) {
  run++;
  if (run == length) {
      return cluster - (uint32_t)(length - 1);
  }
        } else {
  run = 0;
        }
    }
    return 0;
}

/* Mixed FAT layouts can contain fragmented objects whose scattered source
   clusters block thousands of otherwise independent final targets.  The
   adaptive scheduler finishes every currently-free target first, then stages a
   RAM-sized set of blockers into the durable terminal workspace.  Candidate
   scores are calculated once per scheduler pass and sorted once; repeatedly
   rescanning every object for every selected blocker turns a few thousand FAT16
   files into an accidental cubic-time planner. */
typedef struct {
    size_t object_index;
    size_t score;
    U32Vec chain;
} AdaptiveBlockerCandidate;

static int compare_adaptive_blocker_candidate(const void *left_ptr,
                                               const void *right_ptr) {
    const AdaptiveBlockerCandidate *left = left_ptr;
    const AdaptiveBlockerCandidate *right = right_ptr;
    if (left->score > right->score) return -1;
    if (left->score < right->score) return 1;
    if (left->chain.len < right->chain.len) return -1;
    if (left->chain.len > right->chain.len) return 1;
    if (left->object_index < right->object_index) return -1;
    if (left->object_index > right->object_index) return 1;
    return 0;
}

static void adaptive_blocker_candidates_free(AdaptiveBlockerCandidate *candidates,
                                             size_t count) {
    if (candidates == NULL) return;
    for (size_t index = 0; index < count; index++) {
        u32vec_free(&candidates[index].chain);
    }
    free(candidates);
}

static bool adaptive_stop_between_transactions(const char *layout_name,
                                                FatRelayoutStats *stats) {
    stats->interrupted = true;
    fprintf(stderr,
            "%s stopped safely during adaptive dependency layout at a complete "
            "transaction boundary.\n",
            layout_name);
    return true;
}

/* Large RAM buffers should increase throughput, but they must not make Stop wait
   behind an arbitrarily large journal transaction.  Sixty-four MiB is a byte
   budget, not an object-count cap: tiny FAT objects still batch by the thousand,
   while a single larger object remains allowed when it is the dependency that
   must be moved. */
static size_t adaptive_transaction_cluster_limit(const Fat32 *fs,
                                                 size_t ram_cluster_limit) {
    const uint64_t stop_bytes = UINT64_C(64) * 1024 * 1024;
    uint64_t by_stop = stop_bytes / fs->cluster_size;
    if (by_stop == 0) by_stop = 1;
    size_t limit = by_stop > SIZE_MAX ? SIZE_MAX : (size_t)by_stop;
    if (limit > ram_cluster_limit) limit = ram_cluster_limit;
    if (limit == 0) limit = 1;
    return limit;
}

static bool execute_adaptive_dependency_layout(
    Fat32 *fs,
    const char *journal_path,
    FatRelayoutObjectList *objects,
    uint32_t workspace_start,
    size_t workspace_clusters,
    size_t cluster_batch_limit,
    const char *layout_name,
    FatRelayoutStats *stats
) {
    size_t transaction_cluster_limit = adaptive_transaction_cluster_limit(
        fs, cluster_batch_limit);
    double transaction_mib =
        ((double)transaction_cluster_limit * (double)fs->cluster_size) /
        (1024.0 * 1024.0);
    fprintf(stderr,
            "%s adaptive dependency transaction budget: up to %zu clusters "
            "(%.1f MiB) per journal transaction for bounded Stop latency.\n",
            layout_name, transaction_cluster_limit, transaction_mib);

    for (;;) {
        if (ld_stop_requested()) {
            return adaptive_stop_between_transactions(layout_name, stats);
        }

        DirRefList current_refs = {0};
        FileList current_files = scan_files(fs, &current_refs);
        uint8_t *nonexact = ld_xcalloc(objects->len, 1);
        uint8_t *staged = ld_xcalloc(objects->len, 1);
        uint8_t *target_needed = ld_xcalloc((size_t)fs->max_cluster + 1, 1);
        size_t remaining = 0;

        for (size_t index = 0; index < objects->len; index++) {
            if (ld_stop_requested()) {
                free(target_needed);
                free(staged);
                free(nonexact);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                return adaptive_stop_between_transactions(layout_name, stats);
            }
            FatRelayoutObject *object = &objects->v[index];
            const FileRecord *current_file = NULL;
            U32Vec local_root = {0};
            const U32Vec *chain = find_relayout_object_chain(
                fs, object, &current_files, &local_root, &current_file);
            (void)current_file;
            if (chain->len != object->clusters) {
                u32vec_free(&local_root);
                free(target_needed);
                free(staged);
                free(nonexact);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                ld_die("layout object changed size during adaptive dependency scan");
            }
            if (!chain_is_exact_run(chain, object->target)) {
                nonexact[index] = 1;
                staged[index] = chain_is_inside_workspace(
                    chain, workspace_start, workspace_clusters) ? 1 : 0;
                remaining++;
                uint64_t target_end =
                    (uint64_t)object->target + object->clusters - 1;
                if (target_end > fs->max_cluster) {
                    u32vec_free(&local_root);
                    free(target_needed);
                    free(staged);
                    free(nonexact);
                    filelist_free(&current_files);
                    dirreflist_free(&current_refs);
                    ld_die("adaptive dependency target exceeds the filesystem");
                }
                for (size_t i = 0; i < object->clusters; i++) {
                    target_needed[object->target + (uint32_t)i] = 1;
                }
            }
            u32vec_free(&local_root);
        }

        if (remaining == 0) {
            free(target_needed);
            free(staged);
            free(nonexact);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            return true;
        }

        uint8_t *source_seen = ld_xcalloc((size_t)fs->max_cluster + 1, 1);
        uint8_t *destination_seen = ld_xcalloc((size_t)fs->max_cluster + 1, 1);
        RelocationMove *moves = NULL;
        size_t move_count = 0;
        size_t move_cap = 0;
        size_t batch_objects = 0;
        size_t batch_files = 0;
        size_t batch_directories = 0;
        bool stop_during_direct_scan = false;

        for (size_t cursor = objects->len; cursor != 0; cursor--) {
            if (ld_stop_requested()) {
                stop_during_direct_scan = true;
                break;
            }
            size_t index = cursor - 1;
            if (!nonexact[index]) continue;
            FatRelayoutObject *object = &objects->v[index];
            const FileRecord *current_file = NULL;
            U32Vec local_root = {0};
            const U32Vec *chain = find_relayout_object_chain(
                fs, object, &current_files, &local_root, &current_file);
            (void)current_file;
            if (chain->len != object->clusters) {
                u32vec_free(&local_root);
                free(moves);
                free(destination_seen);
                free(source_seen);
                free(target_needed);
                free(staged);
                free(nonexact);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                ld_die("layout object changed size during adaptive direct batch");
            }

            if (move_count != 0 &&
                object->clusters > transaction_cluster_limit - move_count) {
                u32vec_free(&local_root);
                continue;
            }
            if (move_count != 0 &&
                move_count + object->clusters > transaction_cluster_limit) {
                u32vec_free(&local_root);
                continue;
            }
            if (!relayout_batch_can_add(fs, chain, object->target,
                                      source_seen, destination_seen)) {
                u32vec_free(&local_root);
                continue;
            }

            reserve_relocation_moves(
                &moves, &move_cap, move_count, chain->len);
            for (size_t i = 0; i < chain->len; i++) {
                uint32_t source = chain->v[i];
                uint32_t destination = object->target + (uint32_t)i;
                moves[move_count++] = (RelocationMove){
                    .source = source,
                    .destination = destination,
                };
                source_seen[source] = 1;
                destination_seen[destination] = 1;
            }
            batch_objects++;
            if (object->is_dir) batch_directories++;
            else batch_files++;
            u32vec_free(&local_root);
            if (move_count >= transaction_cluster_limit) break;
        }

        free(destination_seen);
        free(source_seen);
        if (stop_during_direct_scan || ld_stop_requested()) {
            free(moves);
            free(target_needed);
            free(staged);
            free(nonexact);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            return adaptive_stop_between_transactions(layout_name, stats);
        }
        if (move_count != 0) {
            fprintf(stderr,
                    "%s adaptive direct batch: committing %zu object%s "
                    "(%zu cluster%s) at the next safe journal boundary.\n",
                    layout_name,
                    batch_objects, batch_objects == 1 ? "" : "s",
                    move_count, move_count == 1 ? "" : "s");
            fflush(stderr);
            relocation_execute_moves(
                fs, &current_refs, journal_path, moves, move_count);
            stats->transactions++;
            stats->clusters_copied += move_count;
            stats->objects_moved += batch_objects;
            stats->files_moved += batch_files;
            stats->directories_moved += batch_directories;
            fprintf(stderr,
                    "%s adaptive direct batch: %zu object%s (%zu file%s, "
                    "%zu director%s), %zu cluster%s committed in one journal "
                    "transaction; %zu object%s remain.\n",
                    layout_name,
                    batch_objects, batch_objects == 1 ? "" : "s",
                    batch_files, batch_files == 1 ? "" : "s",
                    batch_directories, batch_directories == 1 ? "y" : "ies",
                    move_count, move_count == 1 ? "" : "s",
                    remaining - batch_objects,
                    remaining - batch_objects == 1 ? "" : "s");
            free(moves);
            free(target_needed);
            free(staged);
            free(nonexact);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            continue;
        }
        free(moves);

        if (ld_stop_requested()) {
            free(target_needed);
            free(staged);
            free(nonexact);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            return adaptive_stop_between_transactions(layout_name, stats);
        }

        size_t max_workspace_run = largest_free_workspace_run(
            fs, workspace_start, workspace_clusters);
        uint32_t first_workspace_cluster = find_free_workspace_run(
            fs, workspace_start, workspace_clusters, 1);
        if (max_workspace_run == 0 || first_workspace_cluster == 0) {
            free(target_needed);
            free(staged);
            free(nonexact);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            return false;
        }

        uint8_t *workspace_available = ld_xcalloc(workspace_clusters, 1);
        for (size_t offset = 0; offset < workspace_clusters; offset++) {
            workspace_available[offset] = fat_is_free(
                fs, workspace_start + (uint32_t)offset) ? 1 : 0;
        }

        AdaptiveBlockerCandidate *candidates = ld_xcalloc(
            objects->len, sizeof(*candidates));
        size_t candidate_count = 0;
        for (size_t index = 0; index < objects->len; index++) {
            if (ld_stop_requested()) {
                adaptive_blocker_candidates_free(candidates, candidate_count);
                free(workspace_available);
                free(target_needed);
                free(staged);
                free(nonexact);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                return adaptive_stop_between_transactions(layout_name, stats);
            }
            if (!nonexact[index] || staged[index]) continue;
            FatRelayoutObject *object = &objects->v[index];
            const FileRecord *current_file = NULL;
            U32Vec local_root = {0};
            const U32Vec *chain = find_relayout_object_chain(
                fs, object, &current_files, &local_root, &current_file);
            (void)current_file;
            if (chain->len != object->clusters) {
                u32vec_free(&local_root);
                adaptive_blocker_candidates_free(candidates, candidate_count);
                free(workspace_available);
                free(target_needed);
                free(staged);
                free(nonexact);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                ld_die("layout object changed size during adaptive blocker planning");
            }

            size_t score = 0;
            for (size_t i = 0; i < chain->len; i++) {
                if (target_needed[chain->v[i]]) score++;
            }
            if (score != 0 && chain->len <= max_workspace_run) {
                AdaptiveBlockerCandidate *candidate =
                    &candidates[candidate_count++];
                candidate->object_index = index;
                candidate->score = score;
                candidate->chain.v = ld_xmalloc(
                    chain->len * sizeof(*candidate->chain.v));
                memcpy(candidate->chain.v, chain->v,
                       chain->len * sizeof(*candidate->chain.v));
                candidate->chain.len = chain->len;
                candidate->chain.cap = chain->len;
            }
            u32vec_free(&local_root);
        }

        qsort(candidates, candidate_count, sizeof(*candidates),
              compare_adaptive_blocker_candidate);
        fprintf(stderr,
                "%s adaptive dependency planning: %zu object%s remain; %zu "
                "blocking candidate%s scored once for a %zu-cluster workspace.\n",
                layout_name,
                remaining, remaining == 1 ? "" : "s",
                candidate_count, candidate_count == 1 ? "" : "s",
                workspace_clusters);
        fflush(stderr);

        RelocationMove *stage_moves = NULL;
        size_t stage_move_count = 0;
        size_t stage_move_cap = 0;
        size_t stage_objects = 0;
        size_t stage_files = 0;
        size_t stage_directories = 0;
        size_t released_targets = 0;

        for (size_t candidate_index = 0;
             candidate_index < candidate_count;
             candidate_index++) {
            if (ld_stop_requested()) {
                free(stage_moves);
                adaptive_blocker_candidates_free(candidates, candidate_count);
                free(workspace_available);
                free(target_needed);
                free(staged);
                free(nonexact);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                return adaptive_stop_between_transactions(layout_name, stats);
            }
            if (stage_move_count >= transaction_cluster_limit) break;

            AdaptiveBlockerCandidate *candidate = &candidates[candidate_index];
            const U32Vec *chain = &candidate->chain;
            if (stage_move_count != 0 &&
                chain->len > transaction_cluster_limit - stage_move_count) {
                continue;
            }

            size_t run = 0;
            uint32_t candidate_at = 0;
            for (size_t offset = 0; offset < workspace_clusters; offset++) {
                if (workspace_available[offset]) {
                    run++;
                    if (run == chain->len) {
                        size_t first_offset = offset + 1 - chain->len;
                        candidate_at = workspace_start + (uint32_t)first_offset;
                        break;
                    }
                } else {
                    run = 0;
                }
            }
            if (candidate_at == 0) continue;

            reserve_relocation_moves(
                &stage_moves, &stage_move_cap, stage_move_count, chain->len);
            for (size_t i = 0; i < chain->len; i++) {
                stage_moves[stage_move_count++] = (RelocationMove){
                    .source = chain->v[i],
                    .destination = candidate_at + (uint32_t)i,
                };
            }
            size_t first_offset = (size_t)(candidate_at - workspace_start);
            for (size_t i = 0; i < chain->len; i++) {
                workspace_available[first_offset + i] = 0;
            }
            stage_objects++;
            if (objects->v[candidate->object_index].is_dir) stage_directories++;
            else stage_files++;
            if (candidate->score > SIZE_MAX - released_targets) {
                free(stage_moves);
                adaptive_blocker_candidates_free(candidates, candidate_count);
                free(workspace_available);
                free(target_needed);
                free(staged);
                free(nonexact);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                ld_die("adaptive blocker score overflow");
            }
            released_targets += candidate->score;
        }

        adaptive_blocker_candidates_free(candidates, candidate_count);
        free(workspace_available);
        if (stage_move_count == 0) {
            free(stage_moves);
            free(target_needed);
            free(staged);
            free(nonexact);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            return false;
        }

        if (ld_stop_requested()) {
            free(stage_moves);
            free(target_needed);
            free(staged);
            free(nonexact);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            return adaptive_stop_between_transactions(layout_name, stats);
        }

        double stage_mib =
            ((double)stage_move_count * (double)fs->cluster_size) /
            (1024.0 * 1024.0);
        fprintf(stderr,
                "%s adaptive dependency batch: staging %zu blocker%s "
                "(%zu cluster%s, %.1f MiB) into the safety workspace in one "
                "journal transaction.\n",
                layout_name,
                stage_objects, stage_objects == 1 ? "" : "s",
                stage_move_count, stage_move_count == 1 ? "" : "s",
                stage_mib);
        fflush(stderr);
        detail_log(
            "layout-adaptive-stage-batch: %zu blockers / %zu clusters -> reusable "
            "terminal workspace; releases %zu outstanding target-cluster references\n",
            stage_objects, stage_move_count, released_targets);
        relocation_execute_moves(
            fs, &current_refs, journal_path, stage_moves, stage_move_count);
        stats->transactions++;
        stats->clusters_copied += stage_move_count;
        fprintf(stderr,
                "%s adaptive dependency batch: staged %zu blocker%s "
                "(%zu file%s, %zu director%s), %zu cluster%s in one journal "
                "transaction to release %zu blocked target-cluster references.\n",
                layout_name,
                stage_objects, stage_objects == 1 ? "" : "s",
                stage_files, stage_files == 1 ? "" : "s",
                stage_directories, stage_directories == 1 ? "y" : "ies",
                stage_move_count, stage_move_count == 1 ? "" : "s",
                released_targets);

        free(stage_moves);
        free(target_needed);
        free(staged);
        free(nonexact);
        filelist_free(&current_files);
        dirreflist_free(&current_refs);
    }
}

static FatRelayoutStats fat_relayout_volume(Fat32 *fs, const char *journal_path,
                                       unsigned requested_percent,
                                       size_t batch_clusters) {
    FatRelayoutStats stats = {0};
    double preflight_started = infiltratr_monotonic_seconds();
    const char *layout_name = requested_percent == 0 ? "Defragment" : "Growth Defrag";
    if (requested_percent > 25) {
        ld_die("growth reserve percentage must be between 0 and 25");
    }

    DirRefList initial_refs = {0};
    FileList initial_files = scan_files(fs, &initial_refs);
    size_t initial_largest = 0, regular_files = 0, directories = 0;
    uint64_t regular_clusters = 0;
    FatRelayoutObjectList initial_objects = fat_relayout_build_objects(
        fs, &initial_files, &initial_largest, &regular_clusters,
        &regular_files, &directories);
    uint64_t free_before = count_free_clusters(fs);

    fprintf(stderr,
            "%s preflight (%s): checking %zu regular file%s and %zu director%s "
            "for contiguity%s.\n",
            layout_name, PROGRAM_VERSION,
            regular_files, regular_files == 1 ? "" : "s",
            directories, directories == 1 ? "y" : "ies",
            requested_percent == 0 ? " and canonical zero-gap packing" :
                                     " with the requested post-file reserve");
    FatRelayoutPreflight preflight = fat_relayout_preflight(
        fs, &initial_files, requested_percent);
    size_t existing_reserve = preflight.reserve_clusters;
    bool canonical_verified = false;
    if (regular_files != 0 && preflight.issue == FAT_RELAYOUT_PREFLIGHT_OK) {
        canonical_verified = fat_relayout_matches_canonical(
            fs, &initial_objects, requested_percent, &existing_reserve);
    }
    if (canonical_verified) {
        stats.preflight_seconds = elapsed_since(preflight_started);
        stats.already_satisfied = true;
        stats.canonical_layout_verified = canonical_verified;
        stats.applied_percent = requested_percent;
        stats.reserve_clusters = existing_reserve;
        stats.checked_files = regular_files;
        stats.checked_directories = directories;
        if (requested_percent == 0) {
            fprintf(stderr,
                    "%s preflight result: every file and directory is contiguous and the "
                    "allocation area is already packed with no internal free clusters.\n",
                    layout_name);
        } else {
            fprintf(stderr,
                    "%s preflight result: the existing FAT layout already satisfies the requested "
                    "%u%% post-file reserve for %zu regular file%s; %zu director%s %s contiguous.\n",
                    layout_name, requested_percent, regular_files, regular_files == 1 ? "" : "s",
                    directories, directories == 1 ? "y" : "ies",
                    directories == 1 ? "is" : "are");
        }
        fprintf(stderr, "No layout rewrite is required.\n");
        fat_relayout_preflight_free(&preflight);
        fat_relayout_object_list_free(&initial_objects);
        filelist_free(&initial_files);
        dirreflist_free(&initial_refs);
        return stats;
    }
    if (preflight.issue == FAT_RELAYOUT_PREFLIGHT_OK) {
        fprintf(stderr,
                "%s preflight result: all objects are contiguous, but the physical layout is "
                "not the canonical earliest packed layout.\n",
                layout_name);
        fprintf(stderr,
                "%s preflight completed read-only; no filesystem writes have occurred yet.\n",
                layout_name);
    } else {
        fat_relayout_print_preflight_failure(&preflight, requested_percent, layout_name);
    }
    fat_relayout_preflight_free(&preflight);
    stats.preflight_seconds = elapsed_since(preflight_started);

    if (initial_objects.len == 0 || regular_files == 0) {
        fat_relayout_object_list_free(&initial_objects);
        filelist_free(&initial_files);
        dirreflist_free(&initial_refs);
        fprintf(stderr, "%s: no allocated regular files require a layout rewrite\n", layout_name);
        return stats;
    }
    if (free_before <= initial_largest || regular_clusters == 0) {
        fat_relayout_object_list_free(&initial_objects);
        filelist_free(&initial_files);
        dirreflist_free(&initial_refs);
        ld_die("layout rewrite needs free space larger than the largest allocated object for safe staging");
    }

    uint64_t requested_reserve = 0;
    for (size_t i = 0; i < initial_objects.len; i++) {
        const FatRelayoutObject *object = &initial_objects.v[i];
        if (object->is_dir) continue;
        uint64_t reserve = ((uint64_t)object->clusters * requested_percent + 99) / 100;
        requested_reserve += reserve;
    }
    if (requested_reserve > free_before - initial_largest) {
        fat_relayout_object_list_free(&initial_objects);
        filelist_free(&initial_files);
        dirreflist_free(&initial_refs);
        ld_die("layout rewrite does not have enough free clusters for the requested reserve plus a safe staging workspace");
    }

    size_t preliminary_reserve = 0;
    uint32_t preliminary_end = 1;
    if (fs->max_cluster == UINT32_MAX ||
        !fat_relayout_plan_layout(fs, &initial_objects, requested_percent,
                            fs->max_cluster + 1,
                            &preliminary_reserve, &preliminary_end)) {
        fat_relayout_object_list_free(&initial_objects);
        filelist_free(&initial_files);
        dirreflist_free(&initial_refs);
        ld_die("layout rewrite could not construct the canonical target layout");
    }
    (void)preliminary_reserve;

    size_t total_object_clusters = relayout_object_cluster_total(&initial_objects);
    size_t ram_cluster_limit = automatic_relayout_batch_clusters(fs);
    size_t tail_slack = (size_t)(fs->max_cluster - preliminary_end);
    size_t terminal_capacity = terminal_workspace_capacity(fs);
    size_t workspace_clusters = total_object_clusters;
    if (workspace_clusters > ram_cluster_limit) workspace_clusters = ram_cluster_limit;
    if (workspace_clusters > tail_slack) workspace_clusters = tail_slack;
    if (workspace_clusters > terminal_capacity) workspace_clusters = terminal_capacity;
    if (workspace_clusters < initial_largest) workspace_clusters = initial_largest;
    if (workspace_clusters > tail_slack || workspace_clusters > terminal_capacity) {
        fat_relayout_object_list_free(&initial_objects);
        filelist_free(&initial_files);
        dirreflist_free(&initial_refs);
        ld_die("layout rewrite cannot create a durable terminal workspace large enough for the largest object");
    }

    filelist_free(&initial_files);
    dirreflist_free(&initial_refs);

    uint32_t workspace_start =
        fs->max_cluster - (uint32_t)workspace_clusters + 1;
    double preparation_started = infiltratr_monotonic_seconds();
    fprintf(stderr,
            "%s phase 1: creating a %zu-cluster terminal safety workspace "
            "from a %zu-cluster RAM budget.\n",
            layout_name, workspace_clusters, ram_cluster_limit);
    size_t preparation_batch_clusters = batch_clusters;
    if (preparation_batch_clusters == 4096) {
        preparation_batch_clusters = ram_cluster_limit;
    }
    fprintf(stderr,
            "%s preparation batching: up to %zu clusters per journal transaction.\n",
            layout_name, preparation_batch_clusters);
    PackingStats packing_stats = prepare_terminal_workspace(
        fs, journal_path, workspace_start, workspace_clusters,
        preparation_batch_clusters);
    stats.packing_clusters = packing_stats.clusters_moved;
    stats.packing_transactions = packing_stats.transactions;
    stats.preparation_seconds = elapsed_since(preparation_started);
    if (ld_stop_requested()) {
        stats.interrupted = true;
        fprintf(stderr,
                "%s stopped safely during preparation after moving %zu cluster%s "
                "in %zu completed transaction%s.\n",
                layout_name, packing_stats.clusters_moved,
                packing_stats.clusters_moved == 1 ? "" : "s",
                packing_stats.transactions,
                packing_stats.transactions == 1 ? "" : "s");
        fprintf(stderr,
                "The canonical layout phase was not started; the filesystem is valid "
                "but %s is incomplete.\n",
                layout_name);
        if (requested_percent != 0) {
            fprintf(stderr, "No growth-space reserve was applied.\n");
        }
        fat_relayout_object_list_free(&initial_objects);
        return stats;
    }
    fprintf(stderr,
            "%s preparation complete: %zu cluster%s moved in %zu transaction%s.\n",
            layout_name, packing_stats.clusters_moved,
            packing_stats.clusters_moved == 1 ? "" : "s",
            packing_stats.transactions,
            packing_stats.transactions == 1 ? "" : "s");

    FatRelayoutObjectList objects = initial_objects;
    initial_objects = (FatRelayoutObjectList){0};
    uint64_t terminal = terminal_free_clusters(fs);
    if (terminal < workspace_clusters) {
        fat_relayout_object_list_free(&objects);
        ld_die("layout rewrite could not create the requested terminal safety workspace");
    }
    unsigned applied_percent = requested_percent;
    size_t reserve_total = 0;
    uint32_t layout_end = 1;
    if (!fat_relayout_plan_layout(fs, &objects, applied_percent, workspace_start,
                            &reserve_total, &layout_end)) {
        fat_relayout_object_list_free(&objects);
        ld_die("layout rewrite could not fit the requested layout below the staging workspace");
    }
    stats.applied_percent = applied_percent;
    stats.reserve_clusters = reserve_total;
    stats.layout_started = true;
    double layout_started = infiltratr_monotonic_seconds();
    fprintf(stderr,
            "%s phase 2: rewriting %zu regular file%s and %zu director%s into the canonical layout%s.\n",
            layout_name, regular_files, regular_files == 1 ? "" : "s",
            directories, directories == 1 ? "y" : "ies",
            applied_percent == 0 ? " with no internal free clusters" :
                                   " with a 10% post-file reserve");
    fprintf(stderr,
            "Final planned layout ends at cluster %" PRIu32 "; reusable safety workspace begins at cluster %" PRIu32 ".\n",
            layout_end, workspace_start);

    size_t reverse = objects.len;
    size_t object_batch_limit = objects.len;
    size_t cluster_batch_limit = ram_cluster_limit;
    fprintf(stderr,
            "%s layout batching: RAM budget up to %zu clusters; up to %zu objects "
            "may share a dependency batch.\n",
            layout_name, cluster_batch_limit, object_batch_limit);

    bool completed_forward = execute_forward_compaction(
        fs, journal_path, &objects, workspace_start, workspace_clusters,
        object_batch_limit, cluster_batch_limit, layout_name, &stats);
    if (completed_forward) {
        fat_relocation_update_fsinfo(fs, fat_relocation_first_free_hint(fs));
        fat32_sync(fs);
        fat_relayout_object_list_free(&objects);
        stats.layout_seconds = elapsed_since(layout_started);
        return stats;
    }
    fprintf(stderr,
            "%s forward dependency pass is not sufficient for the remaining layout; "
            "trying one unified workspace dependency batch.\n",
            layout_name);

    bool completed_workspace = execute_full_workspace_layout(
        fs, journal_path, &objects, workspace_start, workspace_clusters,
        cluster_batch_limit, layout_name, &stats);
    if (completed_workspace) {
        fat_relocation_update_fsinfo(fs, fat_relocation_first_free_hint(fs));
        fat32_sync(fs);
        fat_relayout_object_list_free(&objects);
        stats.layout_seconds = elapsed_since(layout_started);
        return stats;
    }
    fprintf(stderr,
            "%s remaining dependency set exceeds the all-at-once workspace fast path; "
            "switching to the adaptive dependency scheduler.\n",
            layout_name);

    bool completed_adaptive = execute_adaptive_dependency_layout(
        fs, journal_path, &objects, workspace_start, workspace_clusters,
        cluster_batch_limit, layout_name, &stats);
    if (completed_adaptive) {
        fat_relocation_update_fsinfo(fs, fat_relocation_first_free_hint(fs));
        fat32_sync(fs);
        fat_relayout_object_list_free(&objects);
        stats.layout_seconds = elapsed_since(layout_started);
        return stats;
    }
    fprintf(stderr,
            "%s adaptive dependency scheduler exhausted the reusable workspace; "
            "continuing with the final one-object safety fallback.\n",
            layout_name);

    while (reverse != 0) {
        if (ld_stop_requested()) {
            stats.interrupted = true;
            fprintf(stderr,
                    "%s stopped safely during layout between complete batches.\n",
                    layout_name);
            break;
        }

        DirRefList current_refs = {0};
        FileList current_files = scan_files(fs, &current_refs);
        U32Vec root_chain = {0};
        RelocationMove *batch_moves = NULL;
        size_t batch_move_count = 0;
        size_t batch_move_cap = 0;
        size_t batch_objects = 0;
        size_t batch_files = 0;
        size_t batch_directories = 0;
        size_t skipped_exact = 0;
        uint8_t *source_seen = ld_xcalloc((size_t)fs->max_cluster + 1, 1);
        uint8_t *destination_seen = ld_xcalloc((size_t)fs->max_cluster + 1, 1);

        while (reverse != 0 && batch_objects < object_batch_limit) {
            FatRelayoutObject *object = &objects.v[reverse - 1];
            const FileRecord *current_file = NULL;
            U32Vec local_root = {0};
            const U32Vec *chain = find_relayout_object_chain(
                fs, object, &current_files, &local_root, &current_file);
            (void)current_file;
            if (chain->len != object->clusters) {
                u32vec_free(&local_root);
                free(source_seen);
                free(destination_seen);
                free(batch_moves);
                u32vec_free(&root_chain);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                fat_relayout_object_list_free(&objects);
                ld_die("layout object changed size during offline operation");
            }
            if (chain_is_exact_run(chain, object->target)) {
                reverse--;
                skipped_exact++;
                u32vec_free(&local_root);
                continue;
            }
            if (batch_move_count != 0 &&
                object->clusters > cluster_batch_limit - batch_move_count) {
                u32vec_free(&local_root);
                break;
            }
            if (batch_move_count == 0 && object->clusters > cluster_batch_limit) {
                /* A single large object may exceed the normal batch cap; the copy
                   pipeline will still stream it through the RAM budget safely. */
            } else if (batch_move_count + object->clusters > cluster_batch_limit) {
                u32vec_free(&local_root);
                break;
            }
            if (!relayout_batch_can_add(fs, chain, object->target,
                                      source_seen, destination_seen)) {
                u32vec_free(&local_root);
                break;
            }

            reserve_relocation_moves(
                &batch_moves, &batch_move_cap, batch_move_count, chain->len);
            for (size_t i = 0; i < chain->len; i++) {
                uint32_t source = chain->v[i];
                uint32_t destination = object->target + (uint32_t)i;
                batch_moves[batch_move_count++] = (RelocationMove){
                    .source = source,
                    .destination = destination,
                };
                source_seen[source] = 1;
                destination_seen[destination] = 1;
            }
            detail_log(
                "layout-place: %s %s (%zu clusters) -> cluster %" PRIu32
                " with %zu reserved cluster%s after it\n",
                object->is_dir ? "DIR" : "FILE", object->path,
                object->clusters, object->target, object->reserve_after,
                object->reserve_after == 1 ? "" : "s");
            batch_objects++;
            if (object->is_dir) batch_directories++;
            else batch_files++;
            reverse--;
            u32vec_free(&local_root);
            if (batch_move_count >= cluster_batch_limit) break;
        }

        free(source_seen);
        free(destination_seen);
        if (batch_move_count != 0) {
            relocation_execute_moves(fs, &current_refs, journal_path,
                                     batch_moves, batch_move_count);
            stats.transactions++;
            stats.clusters_copied += batch_move_count;
            stats.objects_moved += batch_objects;
            stats.files_moved += batch_files;
            stats.directories_moved += batch_directories;
            fprintf(stderr,
                    "%s layout batch: %zu object%s (%zu file%s, %zu director%s), "
                    "%zu cluster%s committed in one journal transaction; %zu object%s remain.\n",
                    layout_name, batch_objects, batch_objects == 1 ? "" : "s",
                    batch_files, batch_files == 1 ? "" : "s",
                    batch_directories, batch_directories == 1 ? "y" : "ies",
                    batch_move_count, batch_move_count == 1 ? "" : "s",
                    reverse, reverse == 1 ? "" : "s");
            free(batch_moves);
            u32vec_free(&root_chain);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            continue;
        }
        free(batch_moves);
        if (skipped_exact != 0) {
            u32vec_free(&root_chain);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            continue;
        }

        /* The next target is not currently free.  This final fallback stages one
           object at a time.  It is retained for very full filesystems whose
           dependency set cannot fit in the RAM-sized durable workspace. */
        FatRelayoutObject *object = &objects.v[reverse - 1];
        const FileRecord *current_file = NULL;
        const U32Vec *chain = find_relayout_object_chain(
            fs, object, &current_files, &root_chain, &current_file);
        (void)current_file;
        if (chain->len != object->clusters) {
            u32vec_free(&root_chain);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            fat_relayout_object_list_free(&objects);
            ld_die("layout object changed size during offline operation");
        }
        U32Vec original_chain = {0};
        for (size_t i = 0; i < chain->len; i++) u32vec_push(&original_chain, chain->v[i]);
        if (!cluster_range_is_free(fs, workspace_start, object->clusters)) {
            u32vec_free(&original_chain);
            u32vec_free(&root_chain);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            fat_relayout_object_list_free(&objects);
            ld_die("layout staging workspace is unexpectedly occupied");
        }
        detail_log("layout-stage: %s %s (%zu clusters) -> workspace cluster %" PRIu32 "\n",
                   object->is_dir ? "DIR" : "FILE", object->path,
                   object->clusters, workspace_start);
        relayout_move_chain(fs, &current_refs, chain, workspace_start,
                          journal_path, false);
        stats.transactions++;
        stats.clusters_copied += object->clusters;
        u32vec_free(&root_chain);
        filelist_free(&current_files);
        dirreflist_free(&current_refs);

        current_refs = (DirRefList){0};
        current_files = scan_files(fs, &current_refs);
        root_chain = (U32Vec){0};
        current_file = NULL;
        chain = find_relayout_object_chain(
            fs, object, &current_files, &root_chain, &current_file);
        if (!chain_is_exact_run(chain, workspace_start)) {
            u32vec_free(&original_chain);
            u32vec_free(&root_chain);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            fat_relayout_object_list_free(&objects);
            ld_die("FAT relayout staged object did not reopen at the workspace");
        }

        bool target_blockers_evacuated = false;
        if (!cluster_range_is_free(fs, object->target, object->clusters)) {
            uint64_t target_end64 = (uint64_t)object->target + object->clusters - 1;
            if (target_end64 > fs->max_cluster) {
                u32vec_free(&original_chain);
                u32vec_free(&root_chain);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                fat_relayout_object_list_free(&objects);
                ld_die("layout target range exceeds the filesystem");
            }
            uint32_t target_end = (uint32_t)target_end64;
            U32Vec blocker_destinations = {0};
            for (size_t i = 0; i < original_chain.len; i++) {
                uint32_t candidate = original_chain.v[i];
                if (candidate >= object->target && candidate <= target_end) continue;
                if (!fat_is_free(fs, candidate)) {
                    u32vec_free(&blocker_destinations);
                    u32vec_free(&original_chain);
                    u32vec_free(&root_chain);
                    filelist_free(&current_files);
                    dirreflist_free(&current_refs);
                    fat_relayout_object_list_free(&objects);
                    ld_die("layout staged source cluster was unexpectedly reused");
                }
                u32vec_push(&blocker_destinations, candidate);
            }

            size_t blocker_count = 0;
            for (uint32_t cluster = object->target; cluster <= target_end; cluster++) {
                if (!fat_is_free(fs, cluster)) blocker_count++;
                if (cluster == UINT32_MAX) break;
            }
            if (blocker_count == 0 || blocker_count > blocker_destinations.len) {
                u32vec_free(&blocker_destinations);
                u32vec_free(&original_chain);
                u32vec_free(&root_chain);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                fat_relayout_object_list_free(&objects);
                ld_die("layout rewrite could not allocate safe source slots for target blockers");
            }

            RelocationMove *blocker_moves = ld_xmalloc(blocker_count * sizeof(*blocker_moves));
            size_t blocker_index = 0;
            for (uint32_t cluster = object->target; cluster <= target_end; cluster++) {
                if (!fat_is_free(fs, cluster)) {
                    blocker_moves[blocker_index] = (RelocationMove){
                        .source = cluster,
                        .destination = blocker_destinations.v[blocker_index],
                    };
                    blocker_index++;
                }
                if (cluster == UINT32_MAX) break;
            }
            detail_log(
                "layout-unblock: moved %zu blocking cluster%s out of target %" PRIu32
                "-%" PRIu32 " using the staged object's released source slots\n",
                blocker_count, blocker_count == 1 ? "" : "s",
                object->target, target_end);
            relocation_execute_moves(fs, &current_refs, journal_path,
                                     blocker_moves, blocker_count);
            target_blockers_evacuated = true;
            stats.transactions++;
            stats.clusters_copied += blocker_count;
            fprintf(stderr,
                    "%s layout cleared %zu blocking cluster%s from %s's target range.\n",
                    layout_name, blocker_count,
                    blocker_count == 1 ? "" : "s", object->path);
            free(blocker_moves);
            u32vec_free(&blocker_destinations);

            u32vec_free(&root_chain);
            filelist_free(&current_files);
            dirreflist_free(&current_refs);
            current_refs = (DirRefList){0};
            current_files = scan_files(fs, &current_refs);
            root_chain = (U32Vec){0};
            current_file = NULL;
            chain = find_relayout_object_chain(
                fs, object, &current_files, &root_chain, &current_file);
            if (!chain_is_exact_run(chain, workspace_start)) {
                u32vec_free(&original_chain);
                u32vec_free(&root_chain);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                fat_relayout_object_list_free(&objects);
                ld_die("layout staged object changed while clearing its target");
            }
            if (!cluster_range_is_free(fs, object->target, object->clusters)) {
                u32vec_free(&original_chain);
                u32vec_free(&root_chain);
                filelist_free(&current_files);
                dirreflist_free(&current_refs);
                fat_relayout_object_list_free(&objects);
                ld_die("layout target range remained occupied after blocker evacuation");
            }
        }

        detail_log(
            "layout-place: %s %s -> cluster %" PRIu32
            " with %zu reserved cluster%s after it\n",
            object->is_dir ? "DIR" : "FILE", object->path,
            object->target, object->reserve_after,
            object->reserve_after == 1 ? "" : "s");
        relayout_move_chain(fs, &current_refs, chain, object->target,
                          journal_path, true);
        stats.transactions++;
        stats.clusters_copied += object->clusters;
        stats.objects_moved++;
        if (object->is_dir) stats.directories_moved++;
        else stats.files_moved++;
        reverse--;
        fprintf(stderr,
                "%s layout staged one %s in %u journal transactions; %zu object%s remain.\n",
                layout_name, object->is_dir ? "directory" : "file",
                target_blockers_evacuated ? 3U : 2U,
                reverse, reverse == 1 ? "" : "s");
        u32vec_free(&original_chain);
        u32vec_free(&root_chain);
        filelist_free(&current_files);
        dirreflist_free(&current_refs);
    }

    fat_relocation_update_fsinfo(fs, fat_relocation_first_free_hint(fs));
    fat32_sync(fs);
    fat_relayout_object_list_free(&objects);
    stats.layout_seconds = elapsed_since(layout_started);
    return stats;
}

static void usage(FILE *out) {
    fprintf(out,
        "Usage:\n"
        "  %s identify DEVICE\n"
        "  %s analyze DEVICE [--list]\n"
        "  %s map DEVICE [--cells N]\n"
        "  %s defrag DEVICE --write --confirm DEVICE [--journal PATH]\n"
        "       [--batch-clusters N] [--ram-buffer auto|SIZE] [--workers auto|N]\n"
        "       [--live-map-cells N] [--diagnostic-log PATH] [--verbose]\n"
        "  %s growth-defrag DEVICE --write --confirm DEVICE [--journal PATH]\n"
        "       [--growth-percent 10] [--batch-clusters N]\n"
        "       [--ram-buffer auto|SIZE] [--workers auto|N] [--live-map-cells N]\n"
        "       [--diagnostic-log PATH] [--verbose]\n"
        "  %s recover DEVICE --write --confirm DEVICE [--journal PATH]\n"
        "       [--ram-buffer auto|SIZE] [--workers auto|N]\n\n"
        "DEVICE may be an unmounted block-device partition or a regular FAT12/FAT16/FAT32 image.\n"
        "Defragment and Growth Defrag call the same canonical FAT relayout engine.\n"
        "Defragment passes a zero-percent post-file reserve; Growth Defrag passes ten percent.\n"
        "The loaded BPB supplies FAT12/FAT16/FAT32 geometry and cluster size, so callers never\n"
        "duplicate or override on-disk filesystem geometry. Direct dependency-free relocation\n"
        "is RAM-buffered; overlapping dependency sets use the minimum durable terminal safety\n"
        "workspace needed to remain recoverable across power loss. Every completed transaction\n"
        "is journalled and live allocation-map updates are published only for committed layouts.\n"
        "SIZE accepts suffixes such as 512M, 2G, or 8GB. Ctrl-C requests a clean stop after the\n"
        "current journalled transaction.\n",
        PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME,
        PROGRAM_NAME);
}

static size_t parse_size(const char *s) {
    uint64_t value = 0;
    if (!infiltratr_parse_u64_range(s, 10U, 0U, (uint64_t)SIZE_MAX, &value))
        ld_die("invalid numeric argument");
    return (size_t)value;
}

static size_t parse_byte_size(const char *s) {
    uint64_t bytes = 0U;
    if (!infiltratr_parse_binary_quantity_u64(s, &bytes))
        ld_die("invalid RAM buffer size");
    if (bytes == 0U || bytes > SIZE_MAX)
        ld_die("RAM buffer size is outside this build's range");
    return (size_t)bytes;
}

static void close_diagnostic_log(void) {
    if (g_diagnostic_log != NULL) {
        fclose(g_diagnostic_log);
        g_diagnostic_log = NULL;
    }
}

static void print_io_configuration(void) {
    double mb = (double)g_io.ram_limit / (1024.0 * 1024.0);
    printf("RAM I/O buffer:          %.1f MB\n", mb);
    printf("Source read workers:     %zu\n", g_io.workers);
    printf("Rotational target:       %s\n", g_io.rotational ? "yes" : "no");
    printf("Serial flash target:     %s\n", g_io.serial_flash ? "yes" : "no");
}

static void print_io_statistics(void) {
    double read_mb = (double)g_io.bytes_read / (1024.0 * 1024.0);
    double write_mb = (double)g_io.bytes_written / (1024.0 * 1024.0);
    printf("Buffered data read:      %.1f MB in %" PRIu64 " extent%s\n",
           read_mb, g_io.read_extents, g_io.read_extents == 1 ? "" : "s");
    printf("Buffered data written:   %.1f MB in %" PRIu64 " extent%s\n",
           write_mb, g_io.write_extents, g_io.write_extents == 1 ? "" : "s");
}

static void format_wall_time(time_t value, char *buffer, size_t size) {
    struct tm local = {0};
    if (buffer == NULL || size == 0) return;
    buffer[0] = '\0';
    if (localtime_r(&value, &local) == NULL ||
        strftime(buffer, size, "%Y-%m-%d %H:%M:%S %z", &local) == 0) {
        snprintf(buffer, size, "unavailable");
    }
}

static void print_relayout_timing(const char *layout_name,
                                  time_t wall_started,
                                  double engine_started,
                                  double setup_analysis_seconds,
                                  const FatRelayoutStats *stats,
                                  double verification_seconds) {
    time_t wall_finished = time(NULL);
    char started_text[64];
    char finished_text[64];
    format_wall_time(wall_started, started_text, sizeof(started_text));
    format_wall_time(wall_finished, finished_text, sizeof(finished_text));
    double total_seconds = elapsed_since(engine_started);
    double io_seconds = stats->preparation_seconds + stats->layout_seconds;
    double read_rate = 0.0;
    double write_rate = 0.0;
    bool have_read_rate = infiltratr_u64_counter_rate(
        g_io.bytes_read, 0, 1.0L, io_seconds, &read_rate);
    bool have_write_rate = infiltratr_u64_counter_rate(
        g_io.bytes_written, 0, 1.0L, io_seconds, &write_rate);

    printf("%s engine started:      %s\n", layout_name, started_text);
    printf("%s engine finished:     %s\n", layout_name, finished_text);
    printf("%s phase timings:\n", layout_name);
    printf("  Setup and initial analysis: %.3f s\n", setup_analysis_seconds);
    printf("  Read-only preflight/planning: %.3f s\n", stats->preflight_seconds);
    printf("  Safety workspace preparation: %.3f s\n", stats->preparation_seconds);
    printf("  Canonical layout: %.3f s\n", stats->layout_seconds);
    printf("  Post-layout verification: %.3f s\n", verification_seconds);
    printf("%s engine elapsed:      %.3f s\n", layout_name, total_seconds);
    if (have_read_rate && have_write_rate) {
        printf("Buffered read throughput:  %.1f MiB/s\n",
               read_rate / (1024.0 * 1024.0));
        printf("Buffered write throughput: %.1f MiB/s\n",
               write_rate / (1024.0 * 1024.0));
    } else {
        printf("Buffered I/O throughput:   unavailable (no relocation phase elapsed)\n");
    }
}



int main(int argc, char **argv) {
    (void)setvbuf(stdout, NULL, _IOLBF, 0);
    (void)setvbuf(stderr, NULL, _IOLBF, 0);
    ld_runtime_set_program_name(PROGRAM_NAME);
    ld_stop_clear();
    if (atexit(close_diagnostic_log) != 0) ld_die("cannot register diagnostic-log cleanup");
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("%s %s\n", PROGRAM_NAME, PROGRAM_VERSION);
        return EXIT_SUCCESS;
    }
    if (argc < 3) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    const char *command = argv[1];
    const char *device_path = argv[2];
    bool write_flag = false;
    const char *confirm = NULL;
    const char *journal_arg = NULL;
    bool list = false;
    size_t map_cells = 4096;
    size_t live_map_cells = 0;
    size_t batch_clusters = 4096;
    unsigned growth_percent = 10;
    bool growth_percent_set = false;
    const char *ram_buffer_arg = "auto";
    const char *workers_arg = "auto";
    const char *diagnostic_log_path = NULL;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--write") == 0) write_flag = true;
        else if (strcmp(argv[i], "--list") == 0) list = true;
        else if (strcmp(argv[i], "--cells") == 0 && i + 1 < argc) {
            map_cells = parse_size(argv[++i]);
            if (map_cells == 0 || map_cells > 1048576) {
                ld_die("--cells must be between 1 and 1048576");
            }
        }
        else if (strcmp(argv[i], "--live-map-cells") == 0 && i + 1 < argc) {
            live_map_cells = parse_size(argv[++i]);
            if (live_map_cells == 0 || live_map_cells > 1048576)
                ld_die("--live-map-cells must be between 1 and 1048576");
        }
        else if (strcmp(argv[i], "--confirm") == 0 && i + 1 < argc) confirm = argv[++i];
        else if (strcmp(argv[i], "--journal") == 0 && i + 1 < argc) journal_arg = argv[++i];
        else if (strcmp(argv[i], "--batch-clusters") == 0 && i + 1 < argc) {
            batch_clusters = parse_size(argv[++i]);
            if (batch_clusters == 0) ld_die("--batch-clusters must be at least 1");
        }
        else if (strcmp(argv[i], "--growth-percent") == 0 && i + 1 < argc) {
            size_t parsed = parse_size(argv[++i]);
            if (parsed != 10) ld_die("--growth-percent is fixed at 10");
            growth_percent = (unsigned)parsed;
            growth_percent_set = true;
        }
        else if (strcmp(argv[i], "--ram-buffer") == 0 && i + 1 < argc) {
            ram_buffer_arg = argv[++i];
        }
        else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            workers_arg = argv[++i];
        }
        else if (strcmp(argv[i], "--diagnostic-log") == 0 && i + 1 < argc) {
            diagnostic_log_path = argv[++i];
        }
        else if (strcmp(argv[i], "--verbose") == 0) {
            g_verbose = true;
        }
        else if (strcmp(argv[i], "--version") == 0) {
            printf("%s %s\n", PROGRAM_NAME, PROGRAM_VERSION);
            return EXIT_SUCCESS;
        } else {
            usage(stderr);
            return EXIT_FAILURE;
        }
    }

    if (growth_percent_set && strcmp(command, "growth-defrag") != 0) {
        ld_die("--growth-percent is valid only with growth-defrag");
    }
    if (map_cells != 4096 && strcmp(command, "map") != 0) {
        ld_die("--cells is valid only with map");
    }
    if (live_map_cells != 0 && strcmp(command, "defrag") != 0 &&
        strcmp(command, "growth-defrag") != 0) {
        ld_die("--live-map-cells is valid only with defrag or growth-defrag");
    }
    fat_analysis_set_live_map_cells(live_map_cells);

    bool mutating = strcmp(command, "defrag") == 0 ||
                    strcmp(command, "growth-defrag") == 0 || strcmp(command, "recover") == 0;
    if (mutating && (!write_flag || confirm == NULL || strcmp(confirm, device_path) != 0)) {
        ld_die("writes require both --write and --confirm with the exact DEVICE path");
    }
    if (!mutating && strcmp(command, "identify") != 0
            && strcmp(command, "analyze") != 0 && strcmp(command, "map") != 0) {
        usage(stderr);
        return EXIT_FAILURE;
    }

    bool relayout_operation = strcmp(command, "defrag") == 0 ||
                              strcmp(command, "growth-defrag") == 0;
    double engine_started = relayout_operation
                                ? infiltratr_monotonic_seconds()
                                : 0.0;
    time_t engine_wall_started = relayout_operation ? time(NULL) : (time_t)0;

    if (diagnostic_log_path != NULL) {
        g_diagnostic_log = fopen(diagnostic_log_path, "a");
        if (g_diagnostic_log == NULL) ld_die_errno("open diagnostic log");
        fprintf(g_diagnostic_log, "\n=== %s %s ===\n", PROGRAM_NAME, PROGRAM_VERSION);
        fflush(g_diagnostic_log);
    }
    if (strcmp(command, "identify") == 0) {
        FatType type;
        if (!fat_geometry_probe_path(device_path, &type)) {
            ld_die("target does not contain a supported FAT12/16/32 geometry");
        }
        printf("{\"filesystem\":\"FAT%u\",\"bits\":%u}\n",
               (unsigned)type, (unsigned)type);
        return EXIT_SUCCESS;
    }
    char *journal_path = journal_arg == NULL ? default_journal_path(device_path) : ld_xstrdup(journal_arg);
    Device dev = ld_device_open(device_path, mutating);
    g_io.rotational = ld_device_is_rotational(&dev);
    g_io.serial_flash = ld_device_is_serial_flash(&dev);
    g_io.ram_limit = strcmp(ram_buffer_arg, "auto") == 0
                         ? ld_default_ram_limit()
                         : parse_byte_size(ram_buffer_arg);
    g_io.workers = strcmp(workers_arg, "auto") == 0
                       ? ld_default_worker_count(g_io.rotational, g_io.serial_flash)
                       : parse_size(workers_arg);
    if (g_io.workers == 0) ld_die("--workers must be at least 1");
    size_t cpus = ld_online_cpu_count();
    if (g_io.workers > cpus * 4) ld_die("--workers is unreasonably larger than the available CPU count");
    if (mutating) {
        ld_stop_install_handlers();
        ld_stop_report_ready();
    }
    Fat32 fs;
    fat32_load(&fs, dev, strcmp(command, "recover") == 0);
    if (g_io.ram_limit < fs.cluster_size) g_io.ram_limit = (size_t)fs.cluster_size;

    if (mutating) print_io_configuration();

    if (strcmp(command, "recover") == 0) {
        if (!path_exists(journal_path)) ld_die("journal file does not exist");
        if (journal_has_magic(journal_path, RELOCATION_JOURNAL_MAGIC)) {
            fat_relocation_recover_mapped(
                &fs, journal_path, &g_io, detail_log
            );
        } else if (journal_has_magic(journal_path, JOURNAL_MAGIC)) {
            fat_relocation_recover_legacy(&fs, journal_path);
        } else {
            ld_die("journal has an unrecognised format");
        }
        print_io_statistics();
        ld_emit_result_event(stdout, "recover", "completed", "");
        fat32_unload(&fs);
        free(journal_path);
        return EXIT_SUCCESS;
    }

    if (path_exists(journal_path)) {
        ld_die("an unfinished journal exists; run recover before analysis or filesystem relocation");
    }

    DirRefList dir_refs = {0};
    bool need_dir_refs = strcmp(command, "defrag") == 0 ||
                         strcmp(command, "growth-defrag") == 0;
    FileList files = scan_files(&fs, need_dir_refs ? &dir_refs : NULL);
    fat_analysis_initialise_live_map(&fs, &files);
    if (strcmp(command, "map") == 0) {
        fat_analysis_print_map_json(&fs, &files, map_cells);
        filelist_free(&files);
        dirreflist_free(&dir_refs);
        fat32_unload(&fs);
        free(journal_path);
        return EXIT_SUCCESS;
    }
    fat_analysis_print(&fs, &files);
    if (list) fat_analysis_list_fragmented(&fs, &files);

    const char *result_operation = NULL;
    const char *result_status = NULL;
    double setup_analysis_seconds = relayout_operation
                                        ? elapsed_since(engine_started)
                                        : 0.0;
    double verification_seconds = 0.0;
    FatRelayoutStats operation_stats = {0};
    if (strcmp(command, "defrag") == 0) {
        result_operation = "defrag";
        filelist_free(&files);
        dirreflist_free(&dir_refs);
        FatRelayoutStats packed = fat_relayout_volume(&fs, journal_path, 0, batch_clusters);
        operation_stats = packed;
        if (packed.already_satisfied) {
            result_status = "not-needed";
            printf("Defragment status:        Not needed; canonical packed layout verified\n");
            printf("Defragment layout check:  %zu regular file%s and %zu director%s verified\n",
                   packed.checked_files, packed.checked_files == 1 ? "" : "s",
                   packed.checked_directories,
                   packed.checked_directories == 1 ? "y" : "ies");
        } else if (packed.interrupted) {
            result_status = "stopped";
            size_t transactions = packed.transactions + packed.packing_transactions;
            printf("Defragment status:        Stopped safely after completed transactions\n");
            printf("Defragment layout I/O:    %zu clusters in %zu completed transaction%s\n",
                   packed.clusters_copied + packed.packing_clusters,
                   transactions, transactions == 1 ? "" : "s");
            printf("Defragment canonical layout: Incomplete; phase 2 %s\n",
                   packed.layout_started ? "stopped between batches" : "was not started");
            printf("Defragment verification:  Not run; a zero-fragment scan does not prove zero-gap packing\n");
        } else {
            result_status = "completed";
            size_t transactions = packed.transactions + packed.packing_transactions;
            printf("Defragment status:        Completed\n");
            printf("Defragment layout phase:  %zu file%s and %zu director%s repositioned\n",
                   packed.files_moved, packed.files_moved == 1 ? "" : "s",
                   packed.directories_moved, packed.directories_moved == 1 ? "y" : "ies");
            printf("Defragment layout I/O:    %zu clusters in %zu transaction%s\n",
                   packed.clusters_copied + packed.packing_clusters,
                   transactions, transactions == 1 ? "" : "s");
        }
        double verification_started = infiltratr_monotonic_seconds();
        files = scan_files(&fs, NULL);
        fat_analysis_print(&fs, &files);
        if (!packed.interrupted) fat_analysis_verify_layout_policy(&fs, 0);
        verification_seconds = elapsed_since(verification_started);
    } else if (strcmp(command, "growth-defrag") == 0) {
        result_operation = "growth-defrag";
        filelist_free(&files);
        dirreflist_free(&dir_refs);
        FatRelayoutStats growth = fat_relayout_volume(&fs, journal_path, growth_percent, batch_clusters);
        operation_stats = growth;
        if (growth.already_satisfied) {
            result_status = "not-needed";
            printf("Growth Defrag status:          Not needed; layout already satisfies %u%% reserve\n",
                   growth.applied_percent);
            printf("Growth Defrag layout check:    %zu regular file%s and %zu director%s verified\n",
                   growth.checked_files, growth.checked_files == 1 ? "" : "s",
                   growth.checked_directories,
                   growth.checked_directories == 1 ? "y" : "ies");
            printf("Growth Defrag reserve verified: %zu requested clusters are free after files\n",
                   growth.reserve_clusters);
            printf("Growth Defrag changes:         None\n");
        } else if (growth.interrupted && !growth.layout_started) {
            result_status = "stopped";
            printf("Growth Defrag status:          Stopped safely during preparation\n");
            printf("Growth Defrag preparation:     %zu clusters in %zu completed transaction%s\n",
                   growth.packing_clusters, growth.packing_transactions,
                   growth.packing_transactions == 1 ? "" : "s");
            printf("Growth Defrag reserve applied: No\n");
            printf("Growth Defrag layout phase:    Not started\n");
            printf("Growth Defrag verification:    Not run; canonical 10%% layout is incomplete\n");
        } else if (growth.interrupted) {
            result_status = "stopped";
            printf("Growth Defrag status:          Stopped safely during layout\n");
            printf("Growth Defrag preparation:     %zu clusters in %zu transaction%s\n",
                   growth.packing_clusters, growth.packing_transactions,
                   growth.packing_transactions == 1 ? "" : "s");
            printf("Growth Defrag planned reserve: %u%% (%zu clusters)\n",
                   growth.applied_percent, growth.reserve_clusters);
            printf("Growth Defrag layout progress: %zu file%s and %zu director%s repositioned\n",
                   growth.files_moved, growth.files_moved == 1 ? "" : "s",
                   growth.directories_moved, growth.directories_moved == 1 ? "y" : "ies");
            printf("Growth Defrag layout I/O:      %zu clusters in %zu completed transaction%s\n",
                   growth.clusters_copied, growth.transactions,
                   growth.transactions == 1 ? "" : "s");
            printf("Growth Defrag reserve applied: Partial; the requested layout is incomplete\n");
            printf("Growth Defrag verification:    Not run; canonical 10%% layout is incomplete\n");
        } else {
            result_status = "completed";
            printf("Growth Defrag status:          Completed\n");
            printf("Growth Defrag applied reserve: %u%% (%zu clusters)\n",
                   growth.applied_percent, growth.reserve_clusters);
            printf("Growth Defrag packing phase:   %zu clusters in %zu transaction%s\n",
                   growth.packing_clusters, growth.packing_transactions,
                   growth.packing_transactions == 1 ? "" : "s");
            printf("Growth Defrag layout phase:    %zu file%s and %zu director%s repositioned\n",
                   growth.files_moved, growth.files_moved == 1 ? "" : "s",
                   growth.directories_moved, growth.directories_moved == 1 ? "y" : "ies");
            printf("Growth Defrag layout I/O:      %zu clusters in %zu transaction%s\n",
                   growth.clusters_copied, growth.transactions,
                   growth.transactions == 1 ? "" : "s");
        }
        double verification_started = infiltratr_monotonic_seconds();
        files = scan_files(&fs, NULL);
        fat_analysis_print(&fs, &files);
        if (!growth.interrupted) {
            fat_analysis_verify_layout_policy(&fs, growth_percent);
        }
        verification_seconds = elapsed_since(verification_started);
    }
    if (mutating) print_io_statistics();
    if (relayout_operation) {
        print_relayout_timing(
            strcmp(command, "defrag") == 0 ? "Defragment" : "Growth Defrag",
            engine_wall_started, engine_started, setup_analysis_seconds,
            &operation_stats, verification_seconds);
    }
    if (result_operation != NULL && result_status != NULL) {
        ld_emit_result_event(stdout, result_operation, result_status, "");
    }

    filelist_free(&files);
    dirreflist_free(&dir_refs);
    fat32_unload(&fs);
    fat_analysis_reset_live_map();
    free(journal_path);
    return ld_stop_requested() ? 130 : EXIT_SUCCESS;
}
