// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DEFRAGMENTER_XFS_NATIVE_H
#define DEFRAGMENTER_XFS_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sqlite3.h>
/*
 * Native XFS v5 representation contract.
 *
 * XfsRange is half-open [start, end) in filesystem blocks. AG-relative block
 * numbers remain AG-relative only where a structure explicitly says so;
 * catalogue extents and plans use data-device block numbers. Hard traversal
 * limits are defensive bounds against corrupt/adversarial metadata, not XFS
 * format maxima.
 */

#define XFS_NATIVE_MAX_BTREE_LEVEL 16U
#define XFS_NATIVE_MAX_BTREE_BLOCKS UINT64_C(8000000)
#define XFS_NATIVE_MAX_INODES UINT64_C(100000000)
#define XFS_NATIVE_MAX_EXTENTS_PER_INODE UINT64_C(16000000)
#define XFS_NATIVE_MAX_BMBT_EXTLEN ((UINT64_C(1) << 21) - 1)

#define XFS_DINODE_FMT_LOCAL 1U
#define XFS_DINODE_FMT_EXTENTS 2U
#define XFS_DINODE_FMT_BTREE 3U
#define XFS_DIFLAG_REALTIME UINT16_C(0x0001)
#define XFS_DIFLAG_NODEFRAG (UINT16_C(1) << 13)
#define XFS_DIFLAG2_REFLINK (UINT64_C(1) << 1)
#define XFS_DIFLAG2_NREXT64 (UINT64_C(1) << 4)
#define XFS_DIFLAG2_METADATA (UINT64_C(1) << 5)

#define XFS_SB_FEAT_INCOMPAT_FTYPE (UINT32_C(1) << 0)
#define XFS_SB_FEAT_INCOMPAT_SPINODES (UINT32_C(1) << 1)
#define XFS_SB_FEAT_INCOMPAT_META_UUID (UINT32_C(1) << 2)
#define XFS_SB_FEAT_INCOMPAT_BIGTIME (UINT32_C(1) << 3)
#define XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR (UINT32_C(1) << 4)
#define XFS_SB_FEAT_INCOMPAT_NREXT64 (UINT32_C(1) << 5)
#define XFS_SB_FEAT_INCOMPAT_EXCHRANGE (UINT32_C(1) << 6)
#define XFS_SB_FEAT_INCOMPAT_PARENT (UINT32_C(1) << 7)
#define XFS_SB_FEAT_INCOMPAT_METADIR (UINT32_C(1) << 8)
#define XFS_SB_FEAT_INCOMPAT_ZONED (UINT32_C(1) << 9)
#define XFS_SB_FEAT_INCOMPAT_ZONE_GAPS (UINT32_C(1) << 10)

#define XFS_SB_FEAT_RO_COMPAT_FINOBT (UINT32_C(1) << 0)
#define XFS_SB_FEAT_RO_COMPAT_RMAPBT (UINT32_C(1) << 1)
#define XFS_SB_FEAT_RO_COMPAT_REFLINK (UINT32_C(1) << 2)
#define XFS_SB_FEAT_RO_COMPAT_INOBTCNT (UINT32_C(1) << 3)

#define XFS_RMAP_OFF_MASK ((UINT64_C(1) << 54) - 1)
#define XFS_RMAP_UNWRITTEN (UINT64_C(1) << 61)
#define XFS_RMAP_BMBT_BLOCK (UINT64_C(1) << 62)
#define XFS_RMAP_ATTR_FORK (UINT64_C(1) << 63)
/* XFS on-disk special owner for AG free-space/rmap B+tree blocks (-5ULL). */
#define XFS_RMAP_OWN_AG UINT64_C(0xfffffffffffffffb)
#define XFS_NULLAGBLOCK UINT32_C(0xffffffff)

typedef struct {
    uint64_t start;
    uint64_t end;
} XfsRange;

typedef struct {
    XfsRange *items;
    size_t count;
    size_t capacity;
} XfsRangeVec;

typedef struct {
    uint64_t *items;
    size_t count;
    size_t capacity;
} XfsU64Vec;

typedef struct {
    uint64_t logical;
    uint64_t physical;
    uint64_t length;
    bool unwritten;
} XfsExtent;

typedef struct {
    XfsExtent *items;
    size_t count;
    size_t capacity;
} XfsExtentVec;

typedef struct {
    uint64_t inode;
    uint64_t inode_offset;
    uint64_t size;
    uint64_t nblocks;
    uint8_t version;
    uint8_t data_format;
    uint32_t core_size;
    uint32_t fork_size;
    uint16_t flags;
    uint64_t flags2;
    bool is_file;
    XfsExtentVec extents;
    XfsU64Vec bmap_blocks;
} XfsObject;

typedef struct {
    XfsObject *items;
    size_t count;
    size_t capacity;
} XfsObjectVec;

typedef struct {
    uint32_t agno;
    uint64_t blocks;
    uint64_t free_blocks;
    uint64_t recorded_free_blocks;
    uint64_t longest_free_extent;
    uint64_t free_extents;
    bool free_count_matches_agf;
} XfsAgDetail;

typedef struct {
    XfsAgDetail *items;
    size_t count;
    size_t capacity;
} XfsAgDetailVec;

typedef struct {
    uint32_t block_size;
    uint64_t dblocks;
    uint64_t rblocks;
    uint8_t uuid[16];
    uint8_t meta_uuid[16];
    uint64_t logstart;
    uint64_t rootino;
    uint64_t rbmino;
    uint64_t rsumino;
    uint32_t agblocks;
    uint32_t agcount;
    uint32_t logblocks;
    uint16_t version;
    uint16_t sector_size;
    uint16_t inode_size;
    uint16_t inopblock;
    uint8_t blocklog;
    uint8_t sectlog;
    uint8_t inodelog;
    uint8_t inopblog;
    uint8_t agblklog;
    uint64_t icount;
    uint64_t ifree;
    uint64_t fdblocks;
    uint64_t uquotino;
    uint64_t gquotino;
    uint64_t pquotino;
    uint32_t features_ro_compat;
    uint32_t incompat;
    uint32_t log_incompat;
    bool v5;
    bool sparse_inodes;
} XfsGeometry;

typedef struct {
    XfsGeometry geometry;
    XfsObjectVec objects;
    XfsRangeVec free_ranges;
    XfsRangeVec used_ranges;
    XfsAgDetailVec allocation_groups;
    uint64_t malformed_inodes;
    uint64_t skipped_metadata_inodes;
    uint64_t regular_files;
    uint64_t directories;
    uint64_t fragmented_files;
    uint64_t fragmented_directories;
    uint64_t inodes_scanned;
    uint64_t realtime_inodes;
    uint64_t inobt_blocks;
    uint64_t bmap_blocks;
    uint64_t bnobt_blocks;
    XfsRangeVec fragmented_ranges;
    XfsRangeVec directory_ranges;
} XfsCatalogue;

typedef struct {
    XfsObject *item;
    uint64_t target_start;
    uint64_t reserve;
} XfsPlacement;

typedef struct {
    XfsPlacement *items;
    size_t count;
    size_t capacity;
    XfsRangeVec pool_ranges;
    uint64_t final_block;
    uint64_t boundary_slack;
} XfsPlan;

typedef struct {
    uint32_t start;
    uint32_t count;
    uint64_t owner;
    uint64_t offset_flags;
} XfsRmapRecord;

typedef struct {
    XfsRmapRecord *items;
    size_t count;
    size_t capacity;
} XfsRmapVec;

uint16_t xfs_be16(const uint8_t *p);
uint32_t xfs_be32(const uint8_t *p);
uint64_t xfs_be64(const uint8_t *p);
void xfs_put_be16(uint8_t *p, uint16_t value);
void xfs_put_be32(uint8_t *p, uint32_t value);
void xfs_put_be64(uint8_t *p, uint64_t value);
uint32_t xfs_crc32c_intermediate(const uint8_t *data, size_t length, uint32_t seed);
uint32_t xfs_crc_field(const uint8_t *data, size_t length, size_t field);
void xfs_write_crc_le(uint8_t *data, size_t length, size_t field);

void xfs_range_push(XfsRangeVec *vec, uint64_t start, uint64_t end);
void xfs_range_sort_merge(XfsRangeVec *vec);
void xfs_range_free(XfsRangeVec *vec);
void xfs_u64_push(XfsU64Vec *vec, uint64_t value);
void xfs_u64_free(XfsU64Vec *vec);
void xfs_extent_push(XfsExtentVec *vec, XfsExtent extent);
void xfs_extent_sort_coalesce(XfsExtentVec *vec);
void xfs_extent_free(XfsExtentVec *vec);
void xfs_object_free(XfsObject *object);
void xfs_catalogue_free(XfsCatalogue *catalogue);
void xfs_plan_free(XfsPlan *plan);
void xfs_rmap_push(XfsRmapVec *vec, XfsRmapRecord record);
void xfs_rmap_free(XfsRmapVec *vec);

uint64_t xfs_ag_length(const XfsGeometry *g, uint32_t agno);
uint64_t xfs_ag_offset(const XfsGeometry *g, uint32_t agno, uint32_t agbno);
uint64_t xfs_object_block_count(const XfsObject *object);
uint64_t xfs_catalogue_movable_blocks(const XfsCatalogue *catalogue);
bool xfs_range_contains(const XfsRangeVec *ranges, uint64_t start, uint64_t end);

bool xfs_probe_path(const char *path);
int xfs_scan_catalogue(const char *path, bool strict, XfsCatalogue *out, char **error);
int xfs_validate_writer_support(const XfsCatalogue *catalogue, char **error);
int xfs_emit_analysis_json(const char *path, char **error);

int xfs_build_plan(XfsCatalogue *catalogue, const char *operation, XfsPlan *out, char **error);
bool xfs_plan_already_applied(const XfsCatalogue *catalogue, const XfsPlan *plan);
int xfs_open_plan_db(const char *path, bool create, sqlite3 **out, char **error);
int xfs_populate_plan_db(const char *stage, XfsCatalogue *catalogue, XfsPlan *plan,
                         sqlite3 *db, uint64_t *move_count, char **error);
int xfs_permute_payloads(const char *stage, sqlite3 *db, uint32_t block_size,
                         uint64_t move_count, bool live_updates, char **error);
int xfs_apply_inode_mappings(const char *stage, const XfsCatalogue *catalogue,
                             sqlite3 *db, char **error);
int xfs_verify_stage(const char *stage, sqlite3 *db, const XfsCatalogue *source,
                     bool growth, XfsCatalogue *verified, char **error);

int xfs_verify_clean_log(const char *path, const XfsCatalogue *catalogue, char **error);
int xfs_rebuild_allocation_metadata(const char *path, const XfsCatalogue *source,
                                    sqlite3 *db, char **error);
int xfs_verify_allocation_metadata(const char *path, const XfsCatalogue *source,
                                   sqlite3 *db, char **error);

void xfs_set_error(char **error, const char *format, ...);
void xfs_clear_error(char **error);

#endif
