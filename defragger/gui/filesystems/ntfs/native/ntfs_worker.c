// SPDX-License-Identifier: GPL-3.0-or-later
#include "ntfs_native.h"
#include "ntfs_transaction.h"

#include "ld_device.h"
#include "ld_io.h"
#include "ld_runtime.h"
#include "ld_protocol.h"
#include "ld_path.h"

#include "infiltratr/core.h"
#include "infiltratr/config.h"
#include "infiltratr/posix.h"
#include "ld_stop.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define PROGRAM_NAME "linux-defragger-ntfs-worker"
#define JOURNAL_CLUSTER_INTERVAL UINT64_C(16384)

static void usage(FILE *stream) {
    fprintf(stream,
            "Usage:\n"
            "  %s --version\n"
            "  %s identify DEVICE\n"
            "  %s analyse-json DEVICE\n"
            "  %s defrag|growth-defrag|recover DEVICE --write --confirm DEVICE --journal PATH [--live-updates]\n",
            PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME);
}





static int parse_u64(const char *text, uint64_t *value)
{
    return infiltratr_parse_u64(text, 10U, value) ? 0 : -1;
}

static void remove_partial_stage(const char *path)
{
    if (path == NULL || *path == '\0')
        return;
    const int failure = infiltratr_unlink_durable(path, true);
    if (failure != 0)
        fprintf(stderr,
                "%s: warning: cannot durably remove incomplete NTFS stage %s: %s\n",
                PROGRAM_NAME, path, strerror(failure));
}
static void serial_hex(const uint8_t serial[8], char output[17]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 8U; ++i) {
        output[i * 2U] = digits[serial[i] >> 4];
        output[i * 2U + 1U] = digits[serial[i] & 15U];
    }
    output[16] = '\0';
}

static bool same_serial(const NtfsVolume *volume, const char *serial) {
    char current[17]; serial_hex(volume->serial, current); return strcmp(current, serial) == 0;
}

static int capacity_preflight(const char *journal_path, const NtfsVolume *volume,
                              const NtfsLayout *layout, char **error) {
    char *parent = ld_path_parent_directory(journal_path); struct statvfs info;
    if (statvfs(parent, &info) != 0) {
        ntfs_set_error(error, "cannot inspect NTFS staging capacity: %s", strerror(errno)); free(parent); return -1;
    }
    free(parent);
    uint64_t used = 0;
    for (uint64_t c = 0; c < volume->total_clusters; ++c) if (ntfs_bitmap_bit(layout, c)) used++;
    uint64_t available = (uint64_t)info.f_bavail * (uint64_t)info.f_frsize;
    uint64_t stage_bytes = infiltratr_u64_multiply_saturating(
        used, volume->cluster_size);
    uint64_t plan_bytes = infiltratr_u64_multiply_saturating(used, 80U);
    plan_bytes = infiltratr_u64_add_saturating(
        plan_bytes, UINT64_C(64) * 1024U * 1024U);
    uint64_t required = infiltratr_u64_add_saturating(stage_bytes, plan_bytes);
    required = infiltratr_u64_add_saturating(required, required / 20U);
    if (available < required) {
        ntfs_set_error(error, "NTFS safe staging has only %llu MB available; approximately %llu MB is required",
                       (unsigned long long)(available / (1024U * 1024U)),
                       (unsigned long long)(required / (1024U * 1024U)));
        return -1;
    }
    return 0;
}

static int copy_cluster(int source, int target, uint32_t cluster_size, uint64_t cluster,
                        uint8_t *buffer, char **error) {
    uint64_t offset = cluster * (uint64_t)cluster_size;
    ssize_t got = ld_pread_full(source, buffer, cluster_size, offset);
    if (got < 0 || (size_t)got != cluster_size) { ntfs_set_error(error, "short read cloning NTFS working image"); return -1; }
    ssize_t wrote = ld_pwrite_full(target, buffer, cluster_size, offset);
    if (wrote < 0 || (size_t)wrote != cluster_size) { ntfs_set_error(error, "short write cloning NTFS working image"); return -1; }
    return 0;
}

static int create_stage(const char *source_path, const char *stage_path,
                        const NtfsVolume *source, const NtfsLayout *layout, char **error) {
    int input = open(source_path, O_RDONLY | O_CLOEXEC);
    if (input < 0) { ntfs_set_error(error, "cannot open NTFS source for staging: %s", strerror(errno)); return -1; }
    int output = open(stage_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (output < 0) { close(input); ntfs_set_error(error, "cannot create NTFS working image: %s", strerror(errno)); return -1; }
    if (ftruncate(output, (off_t)source->volume_bytes) != 0) {
        ntfs_set_error(error, "cannot size NTFS working image: %s", strerror(errno));
        close(input); close(output); remove_partial_stage(stage_path); return -1;
    }
    uint8_t *buffer = ld_xmalloc(source->cluster_size); uint64_t copied = 0;
    for (uint64_t cluster = 0; cluster < source->total_clusters; ++cluster) {
        bool needed = ntfs_bitmap_bit(layout, cluster) != 0 || cluster == 0 || cluster + 1U == source->total_clusters;
        if (!needed) continue;
        if (copy_cluster(input, output, source->cluster_size, cluster, buffer, error) != 0) {
            free(buffer); close(input); close(output); remove_partial_stage(stage_path); return -1;
        }
        copied++;
        if ((copied % 8192U) == 0U && ld_stop_requested()) {
            free(buffer); close(input); close(output); remove_partial_stage(stage_path); return -2;
        }
    }
    free(buffer);
    if (ld_sync_fd(output) != 0) {
        ntfs_set_error(error, "cannot sync NTFS working image: %s", strerror(errno));
        close(input); close(output); remove_partial_stage(stage_path); return -1;
    }
    close(input); close(output); return 0;
}

static int check_unchanged_target(const char *device, const NtfsJournal *state, char **error) {
    char *canonical = NULL, *identity = NULL; uint64_t size = 0;
    if (ld_device_capture_binding(device, &canonical, &identity, &size) != 0) {
        ntfs_set_error(error, "cannot rebind NTFS target: %s", strerror(errno));
        return -1;
    }
    int result = 0;
    if (strcmp(canonical, state->device) != 0 ||
        strcmp(identity, state->target_identity) != 0 || size != state->physical_bytes) {
        ntfs_set_error(error, "NTFS target path, identity or capacity changed before source commit"); result = -1;
    }
    free(canonical); free(identity); if (result != 0) return result;
    NtfsVolume volume;
    if (ntfs_open_volume(device, false, &volume, error) != 0) return -1;
    if (!same_serial(&volume, state->serial) || volume.volume_bytes != state->filesystem_bytes) {
        ntfs_set_error(error, "NTFS serial or filesystem capacity changed before source commit"); result = -1;
    }
    ntfs_close_volume(&volume); return result;
}

static int set_source_dirty(const char *device, const char *target_identity,
                            uint64_t physical_bytes, bool dirty, char **error) {
    NtfsVolume volume; NtfsLayout layout;
    if (ntfs_open_volume(device, true, &volume, error) != 0) return -1;
    if (!ld_fd_matches_identity(volume.fd, target_identity, physical_bytes)) {
        ntfs_set_error(error,
                       "NTFS dirty-state descriptor does not match the journaled target");
        ntfs_close_volume(&volume);
        return -1;
    }
    if (ntfs_read_layout(&volume, dirty ? false : true, &layout, error) != 0) {
        ntfs_close_volume(&volume);
        return -1;
    }
    int result = ntfs_set_volume_dirty(&volume, &layout, dirty, error);
    ntfs_layout_free(&layout); ntfs_close_volume(&volume); return result;
}

static int commit_stage(const char *device, const char *journal_path, NtfsJournal *state,
                        uint64_t resume_cluster, char **error) {
    NtfsVolume stage; NtfsLayout layout;
    if (ntfs_open_volume(state->stage, false, &stage, error) != 0) return -1;
    if (ntfs_read_layout(&stage, true, &layout, error) != 0) { ntfs_close_volume(&stage); return -1; }
    int source = ld_device_open_verified_fd(device, true, state->target_identity,
                                            state->physical_bytes);
    if (source < 0) {
        ntfs_set_error(error,
                       "cannot open journal-bound NTFS source for persistent commit: %s",
                       strerror(errno));
        goto fail_no_source;
    }
    if (flock(source, LOCK_EX | LOCK_NB) != 0) {
        ntfs_set_error(error, "cannot lock NTFS source for persistent commit: %s", strerror(errno)); close(source); goto fail_no_source;
    }
    uint8_t *buffer = ld_xmalloc(stage.cluster_size); uint64_t copied = 0, total = 0;
    for (uint64_t c = resume_cluster; c < stage.total_clusters; ++c)
        if (ntfs_bitmap_bit(&layout, c) != 0 || c == 0 || c + 1U == stage.total_clusters) total++;
    for (uint64_t c = resume_cluster; c < stage.total_clusters; ++c) {
        bool needed = ntfs_bitmap_bit(&layout, c) != 0 || c == 0 || c + 1U == stage.total_clusters;
        if (!needed) continue;
        uint64_t offset = c * (uint64_t)stage.cluster_size;
        ssize_t got = ld_pread_full(stage.fd, buffer, stage.cluster_size, offset);
        if (got < 0 || (size_t)got != stage.cluster_size) { ntfs_set_error(error, "short read from verified NTFS working image"); goto fail; }
        ssize_t wrote = ld_pwrite_full(source, buffer, stage.cluster_size, offset);
        if (wrote < 0 || (size_t)wrote != stage.cluster_size) { ntfs_set_error(error, "short write during NTFS source commit"); goto fail; }
        copied++; state->commit_cluster = c + 1U;
        if ((copied % JOURNAL_CLUSTER_INTERVAL) == 0U) {
            if (ld_sync_fd(source) != 0) { ntfs_set_error(error, "syncing NTFS source commit failed: %s", strerror(errno)); goto fail; }
            if (ntfs_journal_save(journal_path, state, error) != 0) goto fail;
            unsigned percent = total == 0 ? 100U : (unsigned)((copied * 100U) / total);
            printf("NTFS source commit: %u%%\n", percent); fflush(stdout);
        }
    }
    if (ld_sync_fd(source) != 0) { ntfs_set_error(error, "syncing NTFS source commit failed: %s", strerror(errno)); goto fail; }
    if (ntfs_journal_save(journal_path, state, error) != 0) goto fail;
    free(buffer); (void)flock(source, LOCK_UN); close(source);
    ntfs_layout_free(&layout); ntfs_close_volume(&stage); return 0;
fail:
    free(buffer); (void)flock(source, LOCK_UN); close(source);
fail_no_source:
    ntfs_layout_free(&layout); ntfs_close_volume(&stage); return -1;
}

static void emit_bitmap_ranges(const NtfsLayout *layout, uint64_t total, bool want_free) {
    bool active = false, first = true; uint64_t start = 0; putchar('[');
    for (uint64_t c = 0; c < total; ++c) {
        bool matches = (ntfs_bitmap_bit(layout, c) == 0) == want_free;
        if (matches && !active) { active = true; start = c; }
        else if (!matches && active) {
            if (!first) putchar(',');
            printf("[%" PRIu64 ",%" PRIu64 "]", start, c);
            first = false; active = false;
        }
    }
    if (active) {
        if (!first) putchar(',');
        printf("[%" PRIu64 ",%" PRIu64 "]", start, total);
    }
    putchar(']');
}

static void emit_stream_ranges(const NtfsCatalogue *catalogue, bool directories, bool fragmented_only) {
    bool first = true; putchar('[');
    for (size_t i = 0; i < catalogue->count; ++i) {
        const NtfsStream *stream = &catalogue->items[i];
        if (stream->directory != directories) continue;
        if (fragmented_only && ntfs_fragment_count(&stream->runs) <= 1U) continue;
        for (size_t r = 0; r < stream->runs.count; ++r) {
            NtfsRun run = stream->runs.items[r]; if (run.sparse || run.length == 0) continue;
            if (!first) putchar(',');
            printf("[%" PRIu64 ",%" PRIu64 "]", run.lcn, run.lcn + run.length);
            first = false;
        }
    }
    putchar(']');
}

static int analyse_json(const char *path, char **error) {
    NtfsVolume volume; NtfsLayout layout; NtfsCatalogue catalogue;
    if (ntfs_open_volume(path, false, &volume, error) != 0) return -1;
    if (ntfs_read_layout(&volume, true, &layout, error) != 0) { ntfs_close_volume(&volume); return -1; }
    if (ntfs_scan_catalogue(&volume, &layout, &catalogue, error) != 0) {
        ntfs_layout_free(&layout); ntfs_close_volume(&volume); return -1;
    }
    char serial[17]; serial_hex(volume.serial, serial);
    double percentage = infiltratr_percent_u64(
        catalogue.fragmented_files, catalogue.regular_files);
    printf("{\"filesystem\":\"ntfs\",\"cluster_size\":%u,\"total_clusters\":%" PRIu64
           ",\"serial\":\"%s\",\"mft_records_scanned\":%" PRIu64
           ",\"mft_malformed_records\":%" PRIu64 ",\"regular_files\":%" PRIu64
           ",\"directories\":%" PRIu64 ",\"fragmented_files\":%" PRIu64
           ",\"fragmented_directories\":%" PRIu64 ",\"fragmentation_percent\":%.6f"
           ",\"growth_10_satisfied\":%s,\"hibernation_active\":%s,\"free_ranges\":",
           volume.cluster_size, volume.total_clusters, serial, catalogue.records_scanned,
           catalogue.malformed_records, catalogue.regular_files, catalogue.directories,
           catalogue.fragmented_files, catalogue.fragmented_directories, percentage,
           catalogue.growth_10_satisfied ? "true" : "false", catalogue.hibernation_active ? "true" : "false");
    emit_bitmap_ranges(&layout, volume.total_clusters, true);
    fputs(",\"fragmented_ranges\":", stdout); emit_stream_ranges(&catalogue, false, true);
    fputs(",\"directory_ranges\":", stdout); emit_stream_ranges(&catalogue, true, false);
    fputs("}\n", stdout);
    ntfs_catalogue_free(&catalogue); ntfs_layout_free(&layout); ntfs_close_volume(&volume); return 0;
}

static void emit_live_reset(const NtfsVolume *volume, const NtfsLayout *layout) {
    printf("@@LIVE_RESET {\"filesystem\":\"ntfs\",\"block_size\":%u,\"total_blocks\":%" PRIu64 ",\"free_ranges\":",
           volume->cluster_size, volume->total_clusters);
    emit_bitmap_ranges(layout, volume->total_clusters, true);
    fputs("}\n", stdout); fflush(stdout);
}



static bool snapshot_bitmap_bit(const uint8_t *bitmap, size_t bitmap_bytes, uint64_t cluster) {
    if (cluster >= bitmap_bytes * 8U) return true;
    return (bitmap[cluster >> 3] & (uint8_t)(1U << (cluster & 7U))) != 0;
}

static bool find_terminal_workspace(const uint8_t *original_bitmap, size_t original_bytes,
                                    const NtfsLayout *planned, uint64_t total_clusters,
                                    uint64_t needed, uint64_t *workspace_start) {
    if (needed == 0 || total_clusters < 4U) return false;
    uint64_t end = total_clusters - 1U; /* keep the final NTFS backup-boot cluster untouched */
    uint64_t cursor = end;
    while (cursor > 1U) {
        uint64_t cluster = cursor - 1U;
        if (snapshot_bitmap_bit(original_bitmap, original_bytes, cluster) ||
            ntfs_bitmap_bit(planned, cluster) != 0) break;
        cursor--;
    }
    uint64_t available = end - cursor;
    if (available < needed) return false;
    *workspace_start = end - needed;
    return true;
}

static void emit_committed_live_reset(const char *device, char **error) {
    NtfsVolume committed;
    NtfsLayout committed_layout;
    memset(&committed, 0, sizeof(committed));
    committed.fd = -1;
    memset(&committed_layout, 0, sizeof(committed_layout));
    if (ntfs_open_volume(device, false, &committed, error) == 0 &&
        ntfs_read_layout(&committed, false, &committed_layout, error) == 0) {
        emit_live_reset(&committed, &committed_layout);
        ntfs_layout_free(&committed_layout);
        ntfs_close_volume(&committed);
    }
}

/* Return 0 when completed/not-needed, 130 when stopped, 2 when the safe
   terminal-workspace fast path cannot be used and the verified image fallback
   should run, or -1 for a hard failure. */
static int try_terminal_workspace_relayout(const char *device, const char *operation,
                                           const char *journal_path, bool live_updates,
                                           NtfsJournal *state, NtfsVolume *source,
                                           NtfsLayout *source_layout,
                                           NtfsCatalogue *source_catalogue,
                                           char **error) {
    bool growth = strcmp(operation, "growth-defrag") == 0;
    uint8_t *original_bitmap = ld_xmalloc(source_layout->bitmap_bytes);
    memcpy(original_bitmap, source_layout->bitmap, source_layout->bitmap_bytes);
    NtfsPlacementVec placements = {0};
    sqlite3 *db = NULL;
    int result = -1;

    if (ntfs_plan_layout(source_layout, source_catalogue, source->total_clusters,
                         growth, &placements, error) != 0) {
        if (error != NULL && *error != NULL && strstr(*error, "no supported movable") != NULL &&
            source_catalogue->fragmented_files == 0 &&
            source_catalogue->fragmented_directories == 0 &&
            (!growth || source_catalogue->growth_10_satisfied)) {
            free(*error); *error = NULL;
            ntfs_transaction_cleanup(journal_path, state);
            puts("Not needed; canonical NTFS layout already verified.");
            ld_emit_result_event(stdout, operation, "not-needed", "");
            result = 0;
        }
        goto done;
    }
    if (ntfs_create_plan_db(state->plan, source, source_layout, source_catalogue,
                            &placements, growth, &db, error) != 0) goto done;
    state->move_clusters = ntfs_plan_move_count(db, error);
    if (error != NULL && *error != NULL) goto done;
    if (state->move_clusters == 0) {
        printf("Raw userspace native-C NTFS relayout engine %s\n", LD_VERSION);
        printf("NTFS direct metadata layout: %zu supported streams already have their canonical payload placement; no payload relocation is required.\n",
               placements.count);
        if (placements.fixed_streams != 0)
            printf("Preserving %llu unsupported-but-safe NTFS user stream%s in place as fixed allocation obstacle%s.\n",
                   (unsigned long long)placements.fixed_streams,
                   placements.fixed_streams == 1U ? "" : "s",
                   placements.fixed_streams == 1U ? "" : "s");
        if (placements.fixed_slack_clusters != 0)
            printf("NTFS fixed allocations leave %llu unavoidable low-address free cluster%s too small to fill with complete movable streams; preserving them.\n",
                   (unsigned long long)placements.fixed_slack_clusters,
                   placements.fixed_slack_clusters == 1U ? "" : "s");
        fflush(stdout);
        if (ld_stop_requested()) { result = 130; goto stopped_unchanged; }
        if (check_unchanged_target(device, state, error) != 0) goto done;
        if (ntfs_journal_phase(journal_path, state, "direct-metadata", error) != 0) goto done;
        if (set_source_dirty(device, state->target_identity, state->physical_bytes, true, error) != 0) goto metadata_recover_required;
        if (ntfs_apply_stage_metadata(device, db, true, state->target_identity, state->physical_bytes, error) != 0) goto metadata_recover_required;
        if (ntfs_journal_phase(journal_path, state, "direct-verifying-source", error) != 0)
            goto metadata_recover_required;
        if (ntfs_verify_stage(device, db, growth, true, error) != 0)
            goto metadata_recover_required;
        if (set_source_dirty(device, state->target_identity, state->physical_bytes, false, error) != 0) goto metadata_recover_required;
        if (live_updates) emit_committed_live_reset(device, error);
        ntfs_transaction_cleanup(journal_path, state);
        puts("NTFS direct metadata layout: canonical metadata verified without payload relocation or a filesystem-sized working image.");
        printf("NTFS %s completed with serial and full device capacity preserved.\n",
               growth ? "Growth Defrag" : "Defragment");
        ld_emit_result_event(stdout, operation, "completed", "");
        result = 0;
        goto done;
    }
    uint64_t workspace_start = 0;
    if (!find_terminal_workspace(original_bitmap, source_layout->bitmap_bytes,
                                 source_layout, source->total_clusters,
                                 state->move_clusters, &workspace_start)) {
        result = 2;
        goto fallback;
    }
    state->workspace_start = workspace_start;
    state->workspace_clusters = state->move_clusters;
    if (ntfs_prepare_workspace_map(db, workspace_start, state->workspace_clusters,
                                   error) != 0) goto done;

    printf("Raw userspace native-C NTFS relayout engine %s\n", LD_VERSION);
    printf("NTFS direct relayout: %zu supported streams; %llu clusters require relocation.\n",
           placements.count, (unsigned long long)state->move_clusters);
    if (placements.fixed_streams != 0)
        printf("Preserving %llu unsupported-but-safe NTFS user stream%s in place as fixed allocation obstacle%s.\n",
               (unsigned long long)placements.fixed_streams,
               placements.fixed_streams == 1U ? "" : "s",
               placements.fixed_streams == 1U ? "" : "s");
    fflush(stdout);

    if (ld_stop_requested()) { result = 130; goto stopped_unchanged; }
    if (ntfs_journal_phase(journal_path, state, "workspace-staging", error) != 0) goto done;
    printf("NTFS phase 1: staging %llu moved clusters into the durable terminal safety workspace at cluster %llu.\n",
           (unsigned long long)state->workspace_clusters,
           (unsigned long long)state->workspace_start);
    fflush(stdout);
    int stage_result = ntfs_stage_workspace(device, db, source->cluster_size,
                                        state->target_identity, state->physical_bytes,
                                        error);
    if (stage_result == -2 || ld_stop_requested()) { result = 130; goto stopped_unchanged; }
    if (stage_result != 0) goto done;
    if (ntfs_verify_workspace(device, db, source->cluster_size,
                              state->target_identity, state->physical_bytes,
                              error) != 0) goto done;
    if (ntfs_journal_phase(journal_path, state, "workspace-staged", error) != 0) goto done;
    printf("NTFS workspace staging complete: %llu clusters durably copied and checksummed; source metadata is still unchanged.\n",
           (unsigned long long)state->workspace_clusters);
    fflush(stdout);
    if (ld_stop_requested()) { result = 130; goto stopped_unchanged; }
    if (check_unchanged_target(device, state, error) != 0) goto done;
    if (set_source_dirty(device, state->target_identity, state->physical_bytes, true, error) != 0) goto done;
    if (ntfs_journal_phase(journal_path, state, "workspace-placing", error) != 0) goto recover_required;

    puts("NTFS phase 2: placing the canonical layout directly from the durable terminal workspace.");
    fflush(stdout);
    int place_result = ntfs_place_workspace(device, db, source->cluster_size,
                                      state->target_identity, state->physical_bytes,
                                      true, error);
    if (place_result == -2 || ld_stop_requested()) {
        char *restore_error = NULL;
        if (ntfs_restore_workspace(device, db, source->cluster_size,
                                   state->target_identity, state->physical_bytes,
                                   &restore_error) != 0 ||
            set_source_dirty(device, state->target_identity, state->physical_bytes, false, &restore_error) != 0) {
            if (error != NULL && *error == NULL) *error = restore_error;
            else free(restore_error);
            goto recover_required;
        }
        free(restore_error);
        result = 130;
        goto stopped_unchanged;
    }
    if (place_result != 0) {
        char *restore_error = NULL;
        if (ntfs_restore_workspace(device, db, source->cluster_size,
                                   state->target_identity, state->physical_bytes,
                                   &restore_error) == 0 &&
            set_source_dirty(device, state->target_identity, state->physical_bytes, false, &restore_error) == 0) {
            free(restore_error);
            ntfs_transaction_cleanup(journal_path, state);
            goto done;
        }
        free(restore_error);
        goto recover_required;
    }
    if (ntfs_journal_phase(journal_path, state, "workspace-metadata", error) != 0) goto recover_required;
    puts("NTFS phase 3: committing canonical MFT mapping pairs and $Bitmap metadata.");
    fflush(stdout);
    if (ntfs_apply_stage_metadata(device, db, true, state->target_identity, state->physical_bytes, error) != 0) goto recover_required;
    if (ntfs_journal_phase(journal_path, state, "workspace-verifying-source", error) != 0)
        goto recover_required;
    if (ntfs_verify_stage(device, db, growth, true, error) != 0) goto recover_required;
    if (set_source_dirty(device, state->target_identity, state->physical_bytes, false, error) != 0) goto recover_required;
    if (live_updates) emit_committed_live_reset(device, error);
    ntfs_transaction_cleanup(journal_path, state);
    printf("NTFS unified workspace layout: %llu clusters staged and placed without a filesystem-sized working image.\n",
           (unsigned long long)state->workspace_clusters);
    printf("NTFS %s completed with serial and full device capacity preserved.\n",
           growth ? "Growth Defrag" : "Defragment");
    ld_emit_result_event(stdout, operation, "completed", "");
    result = 0;
    goto done;

fallback:
    if (db != NULL) { sqlite3_close(db); db = NULL; }
    ntfs_transaction_cleanup(journal_path, state);
    memcpy(source_layout->bitmap, original_bitmap, source_layout->bitmap_bytes);
    ntfs_placements_free(&placements);
    state->workspace_start = 0;
    state->workspace_clusters = 0;
    state->move_clusters = 0;
    puts("NTFS direct relayout: no safe terminal workspace can hold the complete dependency set; using the verified working-image fallback.");
    fflush(stdout);
    free(original_bitmap);
    return 2;

stopped_unchanged:
    ntfs_transaction_cleanup(journal_path, state);
    puts("Stop requested at a safe NTFS workspace boundary; the original filesystem layout is preserved.");
    ld_emit_result_event(stdout, operation, "stopped", "");
    goto done;

metadata_recover_required:
    /* No payload was moved. Keep the durable plan and journal so Recover can
       idempotently finish the metadata transaction. */
    result = -1;
    goto done;

recover_required:
    /* Durable workspace and plan are deliberately retained. Recovery can
       replay placement and metadata from their checksummed copies. */
    result = -1;

done:
    if (db != NULL) sqlite3_close(db);
    ntfs_placements_free(&placements);
    free(original_bitmap);
    return result;
}

static int build_and_commit(const char *device, const char *operation, const char *journal_path,
                            bool live_updates, char **error) {
    if (ld_path_is_mounted(device)) {
        ntfs_set_error(error,
            "NTFS target is mounted; raw mutation requires an unmounted filesystem");
        return 1;
    }
    int result = 1; char *real = NULL, *identity = NULL; uint64_t physical_bytes = 0;
    NtfsVolume source; NtfsLayout source_layout; NtfsCatalogue source_catalogue;
    memset(&source, 0, sizeof(source)); source.fd = -1; memset(&source_layout, 0, sizeof(source_layout)); memset(&source_catalogue, 0, sizeof(source_catalogue));
    NtfsJournal state; memset(&state, 0, sizeof(state)); sqlite3 *db = NULL;
    NtfsPlacementVec placements = {0}; NtfsVolume staged; NtfsLayout staged_layout; NtfsCatalogue staged_catalogue;
    memset(&staged,0,sizeof(staged)); staged.fd = -1; memset(&staged_layout,0,sizeof(staged_layout)); memset(&staged_catalogue,0,sizeof(staged_catalogue));

    if (ld_device_capture_binding(device, &real, &identity, &physical_bytes) != 0) {
        ntfs_set_error(error, "cannot bind NTFS target: %s", strerror(errno));
        goto done;
    }
    if (ntfs_open_volume(device, false, &source, error) != 0) goto done;
    if (ntfs_read_layout(&source, false, &source_layout, error) != 0) goto done;
    if (ntfs_scan_catalogue(&source, &source_layout, &source_catalogue, error) != 0) goto done;
    if (source_catalogue.malformed_records != 0) { ntfs_set_error(error, "NTFS contains malformed MFT records; refusing mutation"); goto done; }
    if (source_catalogue.hibernation_active) { ntfs_set_error(error, "NTFS hibernation image is active; resume and shut down Windows fully first"); goto done; }
    state.device = ld_xstrdup(real); state.target_identity = ld_xstrdup(identity); serial_hex(source.serial, state.serial);
    snprintf(state.operation, sizeof(state.operation), "%s", operation); snprintf(state.phase, sizeof(state.phase), "prepared");
    state.stage = ld_path_append_suffix(journal_path, ".ntfs-stage.img"); state.plan = ld_path_append_suffix(journal_path, ".ntfs-plan.sqlite");
    state.physical_bytes = physical_bytes; state.filesystem_bytes = source.volume_bytes;
    int workspace_result = try_terminal_workspace_relayout(device, operation, journal_path,
                                                            live_updates, &state, &source,
                                                            &source_layout, &source_catalogue, error);
    if (workspace_result == 0 || workspace_result == 130) { result = workspace_result; goto done; }
    if (workspace_result < 0) goto done;
    if (capacity_preflight(journal_path, &source, &source_layout, error) != 0 || ntfs_journal_save(journal_path, &state, error) != 0) goto precommit_fail;
    printf("Raw userspace native-C NTFS engine %s\n", LD_VERSION); fflush(stdout);
    if (ld_stop_requested()) goto stopped;
    if (ntfs_journal_phase(journal_path, &state, "cloning", error) != 0) goto precommit_fail;
    int clone_result = create_stage(device, state.stage, &source, &source_layout, error);
    if (clone_result == -2 || ld_stop_requested()) goto stopped;
    if (clone_result != 0) goto precommit_fail;

    if (ntfs_open_volume(state.stage, true, &staged, error) != 0) goto precommit_fail;
    if (ntfs_read_layout(&staged, false, &staged_layout, error) != 0) goto precommit_fail;
    if (ntfs_scan_catalogue(&staged, &staged_layout, &staged_catalogue, error) != 0) goto precommit_fail;
    if (ntfs_journal_phase(journal_path, &state, "planning", error) != 0) goto precommit_fail;
    bool growth = strcmp(operation, "growth-defrag") == 0;
    if (ntfs_plan_layout(&staged_layout, &staged_catalogue, staged.total_clusters, growth, &placements, error) != 0) {
        if (error != NULL && *error != NULL && strstr(*error, "no supported movable") != NULL &&
            staged_catalogue.fragmented_files == 0 && staged_catalogue.fragmented_directories == 0 &&
            (!growth || staged_catalogue.growth_10_satisfied)) {
            free(*error); *error = NULL; ntfs_transaction_cleanup(journal_path, &state);
            puts("Not needed; canonical NTFS layout already verified."); ld_emit_result_event(stdout, operation, "not-needed", ""); result = 0; goto done;
        }
        goto precommit_fail;
    }
    if (ntfs_create_plan_db(state.plan, &staged, &staged_layout, &staged_catalogue, &placements, growth, &db, error) != 0) goto precommit_fail;
    state.move_clusters = ntfs_plan_move_count(db, error);
    if (error != NULL && *error != NULL) goto precommit_fail;
    uint32_t stage_cluster_size = staged.cluster_size;
    ntfs_catalogue_free(&staged_catalogue);
    ntfs_layout_free(&staged_layout);
    ntfs_close_volume(&staged);
    if (ntfs_journal_phase(journal_path, &state, "arranging", error) != 0) goto precommit_fail;
    printf("Arranging %zu supported NTFS streams; %" PRIu64 " clusters require relocation.\n", placements.count, state.move_clusters);
    if (placements.fixed_streams != 0)
        printf("Preserving %" PRIu64 " unsupported-but-safe NTFS user stream%s in place as fixed allocation obstacle%s.\n",
               placements.fixed_streams, placements.fixed_streams == 1U ? "" : "s",
               placements.fixed_streams == 1U ? "" : "s");
    if (placements.fixed_slack_clusters != 0)
        printf("NTFS fixed allocations leave %" PRIu64 " unavoidable low-address free cluster%s too small to fill with complete movable streams; preserving them.\n",
               placements.fixed_slack_clusters, placements.fixed_slack_clusters == 1U ? "" : "s");
    fflush(stdout);
    if (ntfs_permute_stage(state.stage, db, stage_cluster_size, state.move_clusters, error) != 0) {
        if (ld_stop_requested()) goto stopped;
        goto precommit_fail;
    }
    if (ntfs_apply_stage_metadata(state.stage, db, false, NULL, 0U, error) != 0) goto precommit_fail;
    if (ntfs_journal_phase(journal_path, &state, "verifying-stage", error) != 0 ||
        ntfs_verify_stage(state.stage, db, growth, false, error) != 0) goto precommit_fail;
    if (ld_stop_requested()) goto stopped;
    if (check_unchanged_target(device, &state, error) != 0) goto precommit_fail;
    if (set_source_dirty(device, state.target_identity, state.physical_bytes, true, error) != 0) goto precommit_fail;
    state.commit_cluster = 0;
    if (ntfs_journal_phase(journal_path, &state, "commit", error) != 0) goto commit_fail;
    puts("The internally verified raw NTFS working image is complete. Starting the persistent source commit."); fflush(stdout);
    if (commit_stage(device, journal_path, &state, 0, error) != 0) goto commit_fail;
    if (ntfs_journal_phase(journal_path, &state, "verifying-source", error) != 0) goto commit_fail;
    if (ntfs_verify_stage(device, db, growth, true, error) != 0) goto commit_fail;
    if (set_source_dirty(device, state.target_identity, state.physical_bytes, false, error) != 0) goto commit_fail;
    if (live_updates) {
        NtfsVolume committed; NtfsLayout committed_layout;
        if (ntfs_open_volume(device,false,&committed,error)==0 && ntfs_read_layout(&committed,false,&committed_layout,error)==0) {
            emit_live_reset(&committed,&committed_layout); ntfs_layout_free(&committed_layout); ntfs_close_volume(&committed);
        }
    }
    ntfs_transaction_cleanup(journal_path, &state);
    printf("NTFS %s completed with serial and full device capacity preserved.\n", growth ? "Growth Defrag" : "Defragment");
    ld_emit_result_event(stdout, operation, ld_stop_requested() ? "stopped" : "completed", ""); result = ld_stop_requested() ? 130 : 0; goto done;
stopped:
    ntfs_transaction_cleanup(journal_path, &state); puts("Stop requested before source commit; the original NTFS filesystem is unchanged.");
    ld_emit_result_event(stdout, operation, "stopped", ""); result = 130; goto done;
precommit_fail:
    ntfs_transaction_cleanup(journal_path, &state); goto done;
commit_fail:
    /* The dirty source, verified stage and journal are deliberately retained for Recover. */
    goto done;
done:
    if (db != NULL) sqlite3_close(db);
    ntfs_placements_free(&placements);
    ntfs_catalogue_free(&staged_catalogue); ntfs_layout_free(&staged_layout); ntfs_close_volume(&staged);
    ntfs_catalogue_free(&source_catalogue); ntfs_layout_free(&source_layout); ntfs_close_volume(&source);
    ntfs_journal_free(&state); free(identity); free(real); return result;
}

static int recover_transaction(const char *device, const char *journal_path, char **error) {
    if (ld_path_is_mounted(device)) {
        ntfs_set_error(error,
            "NTFS target is mounted; raw recovery requires an unmounted filesystem");
        return 1;
    }
    NtfsJournal state;
    if (ntfs_journal_load(journal_path, &state, error) != 0) return 1;
    if (!ld_path_is_derived_from(state.stage, journal_path, ".ntfs-stage.img") ||
        !ld_path_is_derived_from(state.plan, journal_path, ".ntfs-plan.sqlite")) {
        ntfs_set_error(error,
            "NTFS recovery artifacts are not derived from the selected journal path");
        ntfs_journal_free(&state);
        return 1;
    }
    int result = 1; char *real = NULL, *identity = NULL; uint64_t size = 0;
    if (ld_device_capture_binding(device, &real, &identity, &size) != 0) {
        ntfs_set_error(error, "cannot bind NTFS recovery target: %s", strerror(errno));
        goto done;
    }
    if (strcmp(real, state.device) != 0 || strcmp(identity, state.target_identity) != 0 || size != state.physical_bytes) {
        ntfs_set_error(error, "recovery journal belongs to a different NTFS target"); goto done;
    }

    if (strcmp(state.phase, "direct-metadata") == 0 ||
        strcmp(state.phase, "direct-verifying-source") == 0) {
        sqlite3 *metadata_db = NULL;
        if (ntfs_open_plan_db(state.plan, &metadata_db, error) != 0) goto done;
        bool growth = strcmp(state.operation, "growth-defrag") == 0;
        puts("Recovering an NTFS metadata-only canonical relayout.");
        fflush(stdout);
        if (set_source_dirty(device, state.target_identity, state.physical_bytes, true, error) != 0 ||
            ntfs_journal_phase(journal_path, &state, "direct-metadata", error) != 0 ||
            ntfs_apply_stage_metadata(device, metadata_db, true, state.target_identity, state.physical_bytes, error) != 0 ||
            ntfs_journal_phase(journal_path, &state, "direct-verifying-source", error) != 0 ||
            ntfs_verify_stage(device, metadata_db, growth, true, error) != 0 ||
            set_source_dirty(device, state.target_identity, state.physical_bytes, false, error) != 0) {
            sqlite3_close(metadata_db);
            goto done;
        }
        sqlite3_close(metadata_db);
        ntfs_transaction_cleanup(journal_path, &state);
        puts("NTFS metadata-only recovery completed successfully.");
        ld_emit_result_event(stdout, "recover", "completed", "");
        result = 0;
        goto done;
    }

    if (state.workspace_clusters != 0 && infiltratr_string_starts_with(state.phase, "workspace-")) {
        if (state.workspace_start == 0 || state.workspace_clusters != state.move_clusters) {
            ntfs_set_error(error, "NTFS recovery journal has invalid terminal-workspace geometry");
            goto done;
        }
        if (strcmp(state.phase, "workspace-staging") == 0) {
            ntfs_transaction_cleanup(journal_path, &state);
            puts("Discarded an incomplete NTFS terminal workspace; source metadata was unchanged.");
            result = 0;
            goto done;
        }
        sqlite3 *workspace_db = NULL;
        if (ntfs_open_plan_db(state.plan, &workspace_db, error) != 0) goto done;
        NtfsVolume workspace_volume;
        memset(&workspace_volume, 0, sizeof(workspace_volume));
        workspace_volume.fd = -1;
        if (ntfs_open_volume(device, false, &workspace_volume, error) != 0) {
            sqlite3_close(workspace_db); goto done;
        }
        uint32_t cluster_size = workspace_volume.cluster_size;
        ntfs_close_volume(&workspace_volume);
        if (ntfs_verify_workspace(device, workspace_db, cluster_size,
                                  state.target_identity, state.physical_bytes,
                                  error) != 0) {
            sqlite3_close(workspace_db); goto done;
        }
        bool growth = strcmp(state.operation, "growth-defrag") == 0;
        puts("Recovering NTFS directly from the checksummed terminal safety workspace.");
        fflush(stdout);
        if (set_source_dirty(device, state.target_identity, state.physical_bytes, true, error) != 0 ||
            ntfs_journal_phase(journal_path, &state, "workspace-placing", error) != 0 ||
            ntfs_place_workspace(device, workspace_db, cluster_size,
                                 state.target_identity, state.physical_bytes,
                                 false, error) != 0 ||
            ntfs_journal_phase(journal_path, &state, "workspace-metadata", error) != 0 ||
            ntfs_apply_stage_metadata(device, workspace_db, true, state.target_identity, state.physical_bytes, error) != 0 ||
            ntfs_journal_phase(journal_path, &state, "workspace-verifying-source", error) != 0 ||
            ntfs_verify_stage(device, workspace_db, growth, true, error) != 0 ||
            set_source_dirty(device, state.target_identity, state.physical_bytes, false, error) != 0) {
            sqlite3_close(workspace_db); goto done;
        }
        sqlite3_close(workspace_db);
        ntfs_transaction_cleanup(journal_path, &state);
        puts("NTFS terminal-workspace recovery completed successfully.");
        ld_emit_result_event(stdout, "recover", "completed", "");
        result = 0;
        goto done;
    }
    if (strcmp(state.phase, "commit") != 0 && strcmp(state.phase, "verifying-source") != 0) {
        ntfs_transaction_cleanup(journal_path, &state);
        puts("Discarded an incomplete NTFS working image; the source was unchanged."); result = 0; goto done;
    }
    struct stat stage_status;
    if (stat(state.stage, &stage_status) != 0 || !S_ISREG(stage_status.st_mode) ||
        (uint64_t)stage_status.st_size < state.filesystem_bytes) {
        ntfs_set_error(error, "the persistent NTFS working image is missing or truncated"); goto done;
    }
    sqlite3 *db = NULL;
    if (ntfs_open_plan_db(state.plan, &db, error) != 0) goto done;
    bool growth = strcmp(state.operation, "growth-defrag") == 0;
    if (ntfs_verify_stage(state.stage, db, growth, false, error) != 0) { sqlite3_close(db); goto done; }
    NtfsVolume staged; memset(&staged, 0, sizeof(staged)); staged.fd = -1;
    if (ntfs_open_volume(state.stage, false, &staged, error) != 0 || !same_serial(&staged, state.serial)) {
        if (error != NULL && *error == NULL) ntfs_set_error(error, "persistent NTFS working image has the wrong serial");
        ntfs_close_volume(&staged); sqlite3_close(db); goto done;
    }
    uint64_t total_clusters = staged.total_clusters; ntfs_close_volume(&staged);
    if (strcmp(state.phase, "commit") == 0 && state.commit_cluster < total_clusters) {
        printf("Resuming the NTFS source commit at cluster %" PRIu64 ".\n", state.commit_cluster); fflush(stdout);
        if (commit_stage(device, journal_path, &state, state.commit_cluster, error) != 0) { sqlite3_close(db); goto done; }
    }
    if (ntfs_journal_phase(journal_path, &state, "verifying-source", error) != 0) { sqlite3_close(db); goto done; }
    if (ntfs_verify_stage(device, db, growth, true, error) != 0) { sqlite3_close(db); goto done; }
    if (set_source_dirty(device, state.target_identity, state.physical_bytes, false, error) != 0) { sqlite3_close(db); goto done; }
    sqlite3_close(db); ntfs_transaction_cleanup(journal_path, &state);
    puts("NTFS recovery completed successfully."); ld_emit_result_event(stdout, "recover", "completed", ""); result = 0;
done:
    free(real); free(identity); ntfs_journal_free(&state); return result;
}

int main(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) { usage(stdout); return 0; }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) { puts(LD_VERSION); return 0; }
    if (argc < 3) { usage(stderr); return 2; }
    const char *operation = argv[1], *device = argv[2];
    if (strcmp(operation, "identify") == 0) {
        NtfsVolume volume; memset(&volume, 0, sizeof(volume)); volume.fd = -1; char *error = NULL;
        if (ntfs_open_volume(device, false, &volume, &error) != 0) { free(error); return 1; }
        ntfs_close_volume(&volume); puts("{\"filesystem\":\"ntfs\"}"); return 0;
    }
    if (strcmp(operation, "analyse-json") == 0) {
        char *error = NULL; int result = analyse_json(device, &error);
        if (result != 0 && error != NULL) fprintf(stderr, "%s\n", error);
        free(error); return result == 0 ? 0 : 1;
    }
    if (strcmp(operation, "defrag") != 0 && strcmp(operation, "growth-defrag") != 0 && strcmp(operation, "recover") != 0) {
        usage(stderr); return 2;
    }
    const char *confirm = NULL, *journal = NULL; bool write = false, live_updates = false; int growth_percent = 10;
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
        else if ((strcmp(argv[i], "--workers") == 0 || strcmp(argv[i], "--ram-buffer") == 0 || strcmp(argv[i], "--batch-clusters") == 0) && i + 1 < argc) { i++; }
        else { fprintf(stderr, "%s: unknown or incomplete option: %s\n", PROGRAM_NAME, argv[i]); return 2; }
    }
    if (!write || confirm == NULL || journal == NULL || strcmp(confirm, device) != 0) {
        fprintf(stderr, "%s: raw NTFS mutation requires --write --confirm DEVICE --journal PATH\n", PROGRAM_NAME); return 2;
    }
    if (strcmp(operation, "growth-defrag") == 0 && growth_percent != 10) {
        fprintf(stderr, "%s: Growth Defrag requires exactly 10%%%%\n", PROGRAM_NAME); return 2;
    }
    ld_stop_install_handlers();
    ld_stop_report_ready();
    char *error = NULL; int result;
    if (strcmp(operation, "recover") == 0) result = recover_transaction(device, journal, &error);
    else if (access(journal, F_OK) == 0) {
        ntfs_set_error(&error, "an unfinished NTFS journal exists; run Recover first");
        result = 1;
    } else result = build_and_commit(device, operation, journal, live_updates, &error);
    if (result != 0 && result != 130) {
        const char *message = error == NULL ? "native NTFS operation failed" : error;
        fprintf(stderr, "%s\n", message); ld_emit_result_event(stdout, operation, "failed", message);
    }
    free(error); return result;
}
