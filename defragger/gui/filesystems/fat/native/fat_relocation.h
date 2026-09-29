// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LINUX_DEFRAGGER_FAT_RELOCATION_H
#define LINUX_DEFRAGGER_FAT_RELOCATION_H

#include <stddef.h>
#include <stdint.h>

#include "fat_directory.h"
#include "fat_io.h"
#include "fat_journal.h"
#include "fat_volume.h"

typedef void (*FatRelocationLog)(const char *format, ...);

void fat_relocation_execute(
    Fat32 *filesystem,
    const DirRefList *directory_references,
    const char *journal_path,
    const RelocationMove *moves,
    size_t move_count,
    FatIoConfig *io,
    FatRelocationLog detail_log
);

void fat_relocation_recover_mapped(
    Fat32 *filesystem,
    const char *journal_path,
    FatIoConfig *io,
    FatRelocationLog detail_log
);

void fat_relocation_update_fsinfo(
    Fat32 *filesystem,
    uint32_t next_free
);
uint32_t fat_relocation_first_free_hint(const Fat32 *filesystem);

#endif
