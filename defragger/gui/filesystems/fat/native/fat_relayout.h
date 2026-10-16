// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DEFRAGMENTER_FAT_RELAYOUT_H
#define DEFRAGMENTER_FAT_RELAYOUT_H

/* This public model is deliberately FAT-width neutral.  FAT12, FAT16 and
   FAT32 geometry stays in Fat32/FatGeometry; relayout policy is only the
   requested post-file reserve percentage. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fat_directory.h"
#include "fat_volume.h"

typedef struct {
    bool is_root;
    bool is_dir;
    char *path;
    size_t clusters;
    uint32_t physical_first;
    uint32_t target;
    size_t reserve_after;
} FatRelayoutObject;

typedef struct {
    FatRelayoutObject *v;
    size_t len;
    size_t cap;
} FatRelayoutObjectList;

typedef struct {
    size_t objects_moved;
    size_t files_moved;
    size_t directories_moved;
    size_t clusters_copied;
    size_t transactions;
    size_t packing_clusters;
    size_t packing_transactions;
    size_t reserve_clusters;
    unsigned applied_percent;
    bool interrupted;
    bool layout_started;
    bool already_satisfied;
    bool canonical_layout_verified;
    size_t checked_files;
    size_t checked_directories;
    double preflight_seconds;
    double preparation_seconds;
    double layout_seconds;
} FatRelayoutStats;

typedef enum {
    FAT_RELAYOUT_PREFLIGHT_OK = 0,
    FAT_RELAYOUT_PREFLIGHT_NO_FILES,
    FAT_RELAYOUT_PREFLIGHT_ROOT_FRAGMENTED,
    FAT_RELAYOUT_PREFLIGHT_OBJECT_FRAGMENTED,
    FAT_RELAYOUT_PREFLIGHT_RESERVE_SHORT,
} FatRelayoutPreflightIssue;

typedef struct {
    FatRelayoutPreflightIssue issue;
    size_t regular_files;
    size_t directories;
    size_t reserve_clusters;
    size_t required_reserve;
    size_t available_reserve;
    char *problem_path;
} FatRelayoutPreflight;

void fat_relayout_object_list_free(FatRelayoutObjectList *list);
FatRelayoutObjectList fat_relayout_build_objects(
    Fat32 *filesystem,
    const FileList *files,
    size_t *largest_out,
    uint64_t *regular_clusters_out,
    size_t *regular_files_out,
    size_t *directories_out
);
bool fat_relayout_plan_layout(
    Fat32 *filesystem,
    FatRelayoutObjectList *objects,
    unsigned percent,
    uint32_t workspace_start,
    size_t *reserve_total_out,
    uint32_t *layout_end_out
);
bool chain_is_exact_run(const U32Vec *chain, uint32_t start);
FatRelayoutPreflight fat_relayout_preflight(
    Fat32 *filesystem,
    const FileList *files,
    unsigned percent
);
void fat_relayout_preflight_free(FatRelayoutPreflight *preflight);
bool fat_relayout_matches_canonical(
    Fat32 *filesystem,
    FatRelayoutObjectList *objects,
    unsigned percent,
    size_t *reserve_clusters_out
);
void fat_relayout_print_preflight_failure(
    const FatRelayoutPreflight *preflight,
    unsigned percent,
    const char *layout_name
);

#endif
