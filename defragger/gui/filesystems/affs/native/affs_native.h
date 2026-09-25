// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LINUX_DEFRAGGER_AFFS_NATIVE_H
#define LINUX_DEFRAGGER_AFFS_NATIVE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { uint32_t *v; size_t n, cap; } AffsU32Vec;
typedef struct {
    uint32_t header;
    uint32_t byte_size;
    AffsU32Vec data;
    AffsU32Vec lists;
} AffsFile;
typedef struct { AffsFile *v; size_t n, cap; } AffsFileVec;
typedef struct {
    int fd;
    uint64_t bytes;
    uint32_t blocks;
    uint32_t root;
    uint32_t block_size;
    uint8_t dostype;
    bool ffs;
    uint8_t *free_map;
    uint8_t *fixed_map;
    AffsU32Vec bitmap_blocks;
    AffsU32Vec bitmap_ext_blocks;
    AffsU32Vec directory_blocks;
    AffsFileVec files;
} AffsVolume;

void affs_set_error(char **error, const char *fmt, ...);
int affs_scan(const char *path, bool writable, AffsVolume *v, char **error);
void affs_close(AffsVolume *v);
size_t affs_fragments(const AffsU32Vec *blocks);
int affs_analyse_json(const char *path, char **error);
int affs_build_stage(const char *source, const char *stage, bool growth, unsigned growth_percent,
                     bool live_updates, uint64_t *commit_bytes, char **error);
int affs_verify_layout(const char *path, bool growth, unsigned growth_percent, char **error);
int affs_commit_stage(const char *stage, const char *target, uint64_t *written, char **error);
#endif
