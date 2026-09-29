// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LINUX_DEFRAGGER_HFSPLUS_NATIVE_H
#define LINUX_DEFRAGGER_HFSPLUS_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t start;
    uint32_t count;
} HfsPlusExtent;

typedef struct {
    uint64_t logical_size;
    uint32_t total_blocks;
    HfsPlusExtent *extents;
    size_t extent_count;
    size_t extent_capacity;
    bool uses_overflow;
    uint64_t catalog_fork_offset;
} HfsPlusFork;

typedef struct {
    uint32_t file_id;
    HfsPlusFork data_fork;
    HfsPlusFork resource_fork;
} HfsPlusFile;

typedef struct {
    HfsPlusFile *items;
    size_t count;
    size_t capacity;
} HfsPlusFileVec;

typedef struct {
    int fd;
    uint64_t bytes;
    uint16_t signature;
    uint16_t version;
    uint32_t attributes;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t journal_info_block;
    bool journal_checked;
    bool journal_empty;
    bool journal_need_init;
    uint64_t journal_offset;
    uint64_t journal_size;
    HfsPlusFork allocation_fork;
    HfsPlusFork extents_fork;
    HfsPlusFork catalog_fork;
    uint8_t *used_map;
    HfsPlusFileVec files;
    uint32_t directories;
    uint32_t catalog_node_size;
} HfsPlusVolume;

void hfsplus_set_error(char **error, const char *fmt, ...);
int hfsplus_identify(const char *path, uint16_t *signature, uint16_t *version,
                     uint32_t *attributes, char **error);
int hfsplus_scan(const char *path, bool writable, HfsPlusVolume *volume, char **error);
void hfsplus_close(HfsPlusVolume *volume);
int hfsplus_analyse_json(const char *path, char **error);
int hfsplus_writer_preflight(const char *path, bool growth,
                             unsigned growth_percent, char **error);
int hfsplus_build_stage(const char *source, const char *stage, bool growth,
                        unsigned growth_percent, bool live_updates,
                        uint64_t *commit_bytes, char **error);
int hfsplus_verify_layout(const char *path, bool growth, unsigned growth_percent,
                          char **error);
int hfsplus_commit_stage(const char *stage, const char *target,
                         const char *target_identity, uint64_t target_bytes,
                         uint64_t *written, char **error);

#endif
