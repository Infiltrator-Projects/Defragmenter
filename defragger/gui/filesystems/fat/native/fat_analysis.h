// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DEFRAGMENTER_FAT_ANALYSIS_H
#define DEFRAGMENTER_FAT_ANALYSIS_H

#include <stddef.h>

#include "fat_directory.h"
#include "fat_volume.h"

void fat_analysis_set_live_map_cells(size_t cell_count);
void fat_analysis_reset_live_map(void);
void fat_analysis_initialise_live_map(
    Fat32 *filesystem,
    const FileList *files
);
void fat_analysis_emit_live_map_update(Fat32 *filesystem);

void fat_analysis_print(Fat32 *filesystem, const FileList *files);
void fat_analysis_list_fragmented(
    Fat32 *filesystem,
    const FileList *files
);
void fat_analysis_print_map_json(
    Fat32 *filesystem,
    const FileList *files,
    size_t requested_cells
);
void fat_analysis_verify_layout_policy(
    Fat32 *filesystem,
    unsigned reserve_percent
);

#endif
