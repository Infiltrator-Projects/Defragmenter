// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LINUX_DEFRAGGER_FAT_JOURNAL_H
#define LINUX_DEFRAGGER_FAT_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fat_volume.h"

#define JOURNAL_MAGIC "LINUX-DEFRAGGER-JOURNAL-1"
#define RELOCATION_JOURNAL_MAGIC_V1 "LINUX-DEFRAGGER-RELOCATION-JOURNAL-1"
#define RELOCATION_JOURNAL_MAGIC "LINUX-DEFRAGGER-RELOCATION-JOURNAL-2"

typedef enum {
    J_PREPARED = 0,
    J_DATA_COPIED = 1,
    J_DEST_LINKED = 2,
    J_SWITCHED = 3,
    J_OLD_FREED = 4
} JournalStage;

typedef struct {
    char *device_path;
    uint32_t volume_id;
    JournalStage stage;
    uint64_t dirent_offset;
    uint32_t old_first;
    uint32_t dest_start;
    U32Vec source;
} Journal;

typedef struct {
    uint32_t source;
    uint32_t destination;
    uint32_t next;
    uint32_t predecessor;
} RelocationMove;

typedef struct {
    uint64_t offset;
    uint32_t old_target;
    uint32_t new_target;
} RelocationDirPatch;

typedef struct {
    char *device_path;
    char *target_identity;
    uint64_t physical_bytes;
    uint32_t volume_id;
    JournalStage stage;
    uint32_t root_old;
    uint32_t root_new;
    RelocationMove *moves;
    size_t move_count;
    size_t move_capacity;
    RelocationDirPatch *dir_patches;
    size_t dir_patch_count;
    size_t dir_patch_capacity;
} RelocationJournal;

void journal_free(Journal *journal);
void relocation_journal_free(RelocationJournal *journal);
void relocation_journal_add_move(
    RelocationJournal *journal,
    RelocationMove move
);
void relocation_journal_add_dir_patch(
    RelocationJournal *journal,
    RelocationDirPatch patch
);
void relocation_journal_write(
    const char *path,
    const RelocationJournal *journal
);
bool journal_has_magic(const char *path, const char *magic);
Journal journal_read(const char *path);
RelocationJournal relocation_journal_read(const char *path);
void journal_remove(const char *path);
bool path_exists(const char *path);

#endif
