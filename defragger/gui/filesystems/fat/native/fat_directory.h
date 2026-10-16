// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * FAT directory catalogue and filename decoding.
 *
 * This module owns recursive directory traversal, VFAT long-name decoding,
 * short-name fallback, allocation ownership checks and the file/ref catalogue
 * consumed by placement and reporting.  It does not choose or write layouts.
 */

#ifndef DEFRAGMENTER_FAT_DIRECTORY_H
#define DEFRAGMENTER_FAT_DIRECTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fat_volume.h"

typedef struct {
    char *path;
    uint64_t dirent_offset;
    uint32_t first_cluster;
    uint32_t size_bytes;
    uint8_t attr;
    bool is_dir;
    U32Vec chain;
    size_t fragments;
} FileRecord;

typedef struct {
    FileRecord *v;
    size_t len;
    size_t cap;
} FileList;

typedef struct {
    uint64_t offset;
    uint32_t target_cluster;
} DirRef;

typedef struct {
    DirRef *v;
    size_t len;
    size_t cap;
} DirRefList;

void dirreflist_free(DirRefList *list);
void filelist_free(FileList *list);
size_t chain_fragments(const U32Vec *chain);
U32Vec filesystem_root_chain(Fat32 *fs);
FileList scan_files(Fat32 *fs, DirRefList *dir_refs);

#endif
