// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LINUX_DEFRAGGER_EXT_NATIVE_H
#define LINUX_DEFRAGGER_EXT_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ext_disk.h"
#include <sqlite3.h>
/*
 * Native EXT2/3/4 representation contract.
 *
 * Block numbers and range endpoints are filesystem blocks. ExtRange is
 * half-open [start, end). Catalogue/vector storage is owned by the containing
 * object and released by the matching *_free() routine. Geometry describes the
 * source; plan databases and verified catalogues are derived state and are not
 * authority until verification succeeds.
 */

typedef struct {
    char filesystem[8];
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t physical_blocks;
    uint64_t physical_bytes;
    uint64_t first_data_block;
    uint32_t ro_compat;
    uint32_t incompat;
    uint32_t compat;
    uint8_t uuid[16];
} ExtGeometry;

typedef struct {
    uint64_t start;
    uint64_t end; /* exclusive */
} ExtRange;

typedef struct {
    ExtRange *items;
    size_t count;
    size_t capacity;
} ExtRangeVec;

typedef struct {
    uint64_t regular_files;
    uint64_t directories;
    uint64_t fragmented_files;
    uint64_t fragmented_directories;
    uint64_t inodes_scanned;
    uint64_t malformed_inodes;
    bool growth_10_satisfied;
    ExtRangeVec free_ranges;
    ExtRangeVec fragmented_ranges;
    ExtRangeVec directory_ranges;
} ExtCatalogue;

void ext_set_error(char **error, const char *format, ...);
void ext_range_push(ExtRangeVec *vec, uint64_t start, uint64_t end);
void ext_range_free(ExtRangeVec *vec);
void ext_range_sort_merge(ExtRangeVec *vec);

int ext_read_geometry(const char *path, ExtGeometry *geometry, char **error);
int ext_open_fs(const char *path, bool writable, ExtFs **fs, char **error);
int ext_open_fs_under_lock(const char *path, bool writable, ExtFs **fs,
                           char **error);
int ext_validate_metadata(ExtFs *fs, bool verify_inodes, char **error);
int ext_scan_catalogue(const char *path, ExtGeometry *geometry,
                       ExtCatalogue *catalogue, char **error);
void ext_catalogue_free(ExtCatalogue *catalogue);
int ext_validate_writer_support(ExtFs *fs, const ExtGeometry *geometry,
                                char **error);

int ext_open_plan_db(const char *path, bool create, sqlite3 **db, char **error);
int ext_catalog_plan(ExtFs *fs, int raw_fd, sqlite3 *db,
                     const ExtGeometry *geometry, uint64_t *movable,
                     char **error);
int ext_assign_targets(ExtFs *fs, sqlite3 *db,
                       const ExtGeometry *geometry, bool growth,
                       char **error);
int ext_plan_move_count(sqlite3 *db, uint64_t *count, char **error);
int ext_permute_payloads(const char *stage, sqlite3 *db, uint32_t block_size,
                         uint64_t move_count, char **error);
int ext_apply_mappings(const char *stage, sqlite3 *db, bool allow_stop,
                       char **error);
int ext_apply_mappings_under_lock(const char *stage, sqlite3 *db,
                                  bool allow_stop, char **error);
int ext_verify_stage(const char *stage, sqlite3 *db, const ExtGeometry *geometry,
                     bool growth, ExtCatalogue *verified, char **error);

#endif
