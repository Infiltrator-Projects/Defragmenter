// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LINUX_DEFRAGGER_NTFS_NATIVE_H
#define LINUX_DEFRAGGER_NTFS_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sqlite3.h>
/*
 * Native NTFS representation contract.
 *
 * LCN/run lengths are clusters; stream and record sizes are bytes. Dynamic
 * vectors are zero-initialised by callers and released by matching free
 * routines. NtfsVolume binds raw geometry, NtfsLayout represents allocation
 * metadata, and catalogue/placement structures are derived planning state.
 */

#define NTFS_ATTR_ATTRIBUTE_LIST UINT32_C(0x20)
#define NTFS_ATTR_FILE_NAME UINT32_C(0x30)
#define NTFS_ATTR_VOLUME_INFORMATION UINT32_C(0x70)
#define NTFS_ATTR_DATA UINT32_C(0x80)
#define NTFS_ATTR_INDEX_ALLOCATION UINT32_C(0xA0)
#define NTFS_RECORD_IN_USE UINT16_C(0x0001)
#define NTFS_RECORD_DIRECTORY UINT16_C(0x0002)
#define NTFS_ATTR_COMPRESSED UINT16_C(0x0001)
#define NTFS_ATTR_ENCRYPTED UINT16_C(0x4000)
#define NTFS_ATTR_SPARSE UINT16_C(0x8000)
#define NTFS_FIRST_USER_RECORD UINT64_C(24)
#define NTFS_FILE_REFERENCE_MASK UINT64_C(0x0000ffffffffffff)
#define NTFS_VOLUME_DIRTY UINT16_C(0x0001)
#define NTFS_VOLUME_UNSAFE_MASK UINT16_C(0xc037)
#define NTFS_VOLUME_ACCEPTED_MASK UINT16_C(0xc0bf)

#define NTFS_SUBSET_MEMORY_LIMIT (UINT64_C(256) * 1024U * 1024U)

typedef struct {
    uint64_t lcn;
    uint64_t length;
    bool sparse;
} NtfsRun;

typedef struct {
    NtfsRun *items;
    size_t count;
    size_t capacity;
} NtfsRunVec;

typedef struct {
    char *path;
    int fd;
    uint64_t device_size;
    uint64_t volume_bytes;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint64_t total_clusters;
    uint64_t mft_lcn;
    uint64_t mftmirr_lcn;
    uint32_t record_size;
    uint8_t serial[8];
} NtfsVolume;

typedef struct {
    uint32_t offset;
    uint32_t length;
    uint32_t type;
    uint16_t flags;
    bool nonresident;
    uint64_t lowest_vcn;
    uint64_t highest_vcn;
    uint16_t run_offset;
    uint64_t data_size;
    uint64_t allocated_size;
    uint64_t initialized_size;
    NtfsRunVec runs;
    char name[128];
} NtfsAttribute;

typedef struct {
    NtfsAttribute *items;
    size_t count;
    size_t capacity;
} NtfsAttributeVec;

typedef struct {
    uint64_t record_number;
    uint64_t base_record;
    uint32_t attribute_offset;
    uint32_t attribute_type;
    uint16_t attribute_flags;
    bool directory;
    bool movable;
    bool base_record_present;
    uint64_t lowest_vcn;
    uint64_t clusters;
    uint64_t data_size;
    uint32_t mapping_capacity;
    char file_name[256];
    char attribute_name[128];
    NtfsRunVec runs;
} NtfsStream;

typedef struct {
    NtfsStream *items;
    size_t count;
    size_t capacity;
    uint64_t records_scanned;
    uint64_t malformed_records;
    uint64_t regular_files;
    uint64_t directories;
    uint64_t fragmented_files;
    uint64_t fragmented_directories;
    bool growth_10_satisfied;
    bool hibernation_active;
} NtfsCatalogue;

typedef struct {
    NtfsRunVec mft_runs;
    uint64_t mft_data_size;
    NtfsRunVec bitmap_runs;
    uint64_t bitmap_data_size;
    uint8_t *bitmap;
    size_t bitmap_bytes;
    bool volume_dirty;
} NtfsLayout;

typedef struct {
    uint64_t record_number;
    uint32_t attribute_offset;
    uint64_t start;
    uint64_t clusters;
    uint64_t reserve;
} NtfsPlacement;

typedef struct {
    NtfsPlacement *items;
    size_t count;
    size_t capacity;
    uint64_t envelope_start;
    uint64_t envelope_end;
    uint64_t fixed_streams;
    uint64_t fixed_slack_clusters;
} NtfsPlacementVec;

void ntfs_set_error(char **error, const char *format, ...);
uint16_t ntfs_u16(const void *data, size_t offset);
uint32_t ntfs_u32(const void *data, size_t offset);
uint64_t ntfs_u64(const void *data, size_t offset);
void ntfs_put_u16(void *data, size_t offset, uint16_t value);
void ntfs_put_u64(void *data, size_t offset, uint64_t value);

void ntfs_runs_free(NtfsRunVec *runs);
int ntfs_runs_push(NtfsRunVec *runs, uint64_t lcn, uint64_t length, bool sparse);
uint64_t ntfs_run_clusters(const NtfsRunVec *runs);
size_t ntfs_fragment_count(const NtfsRunVec *runs);
int ntfs_decode_runlist(const uint8_t *data, size_t length, NtfsRunVec *runs, char **error);
int ntfs_encode_single_run(uint64_t lcn, uint64_t length, uint8_t *output, size_t capacity,
                           size_t *used, char **error);
int ntfs_apply_fixups(const uint8_t *raw, size_t length, uint32_t sector_size,
                      uint8_t *fixed, char **error);
int ntfs_prepare_fixups(const uint8_t *fixed, size_t length, uint32_t sector_size,
                        uint8_t *raw, char **error);

int ntfs_open_volume(const char *path, bool write, NtfsVolume *volume, char **error);
void ntfs_close_volume(NtfsVolume *volume);
int ntfs_read_stream(const NtfsVolume *volume, const NtfsRunVec *runs,
                     uint64_t logical_offset, void *buffer, size_t length, char **error);
int ntfs_write_stream(const NtfsVolume *volume, const NtfsRunVec *runs,
                      uint64_t logical_offset, const void *buffer, size_t length, char **error);
int ntfs_read_record(const NtfsVolume *volume, const NtfsRunVec *mft_runs,
                     uint64_t record_number, uint8_t **raw, uint8_t **fixed, char **error);
int ntfs_write_record(const NtfsVolume *volume, const NtfsRunVec *mft_runs,
                      uint64_t record_number, const uint8_t *raw, char **error);
int ntfs_parse_attributes(const uint8_t *fixed, size_t record_size,
                          NtfsAttributeVec *attributes, char **error);
void ntfs_attributes_free(NtfsAttributeVec *attributes);
int ntfs_read_layout(NtfsVolume *volume, bool allow_dirty, NtfsLayout *layout, char **error);
void ntfs_layout_free(NtfsLayout *layout);
int ntfs_scan_catalogue(NtfsVolume *volume, NtfsLayout *layout,
                        NtfsCatalogue *catalogue, char **error);
/* Counts preserve catalogue order; the caller owns the returned array. */
size_t *ntfs_logical_stream_counts(const NtfsCatalogue *catalogue);
typedef void (*NtfsScanProgress)(uint64_t completed, uint64_t total, void *context);
int ntfs_scan_catalogue_progress(NtfsVolume *volume, NtfsLayout *layout,
    NtfsCatalogue *catalogue, char **error, NtfsScanProgress progress, void *context);
void ntfs_catalogue_free(NtfsCatalogue *catalogue);
int ntfs_bitmap_bit(const NtfsLayout *layout, uint64_t cluster);
void ntfs_bitmap_set(NtfsLayout *layout, uint64_t cluster, bool used);
int ntfs_write_bitmap(const NtfsVolume *volume, const NtfsLayout *layout, char **error);

int ntfs_plan_layout(NtfsLayout *layout, NtfsCatalogue *catalogue, uint64_t total_clusters,
                     bool growth, NtfsPlacementVec *placements, char **error);
void ntfs_placements_free(NtfsPlacementVec *placements);
int ntfs_stream_digest(NtfsVolume *volume, const NtfsStream *stream,
                       uint8_t *digest, char **error);
int ntfs_create_plan_db(const char *path, NtfsVolume *volume, NtfsLayout *layout,
                        NtfsCatalogue *catalogue, const NtfsPlacementVec *placements,
                        bool growth, sqlite3 **db, char **error);
int ntfs_open_plan_db(const char *path, sqlite3 **db, char **error);
int ntfs_permute_stage(const char *stage, sqlite3 *db, uint32_t cluster_size,
                       uint64_t move_count, char **error);
int ntfs_apply_stage_metadata(const char *stage, sqlite3 *db, bool allow_dirty,
                              const char *target_identity, uint64_t physical_bytes,
                              char **error);
int ntfs_prepare_workspace_map(sqlite3 *db, uint64_t workspace_start,
                               uint64_t workspace_clusters, char **error);
int ntfs_stage_workspace(const char *device, sqlite3 *db, uint32_t cluster_size,
                         const char *target_identity, uint64_t physical_bytes,
                         char **error);
int ntfs_verify_workspace(const char *device, sqlite3 *db, uint32_t cluster_size,
                          const char *target_identity, uint64_t physical_bytes,
                          char **error);
int ntfs_place_workspace(const char *device, sqlite3 *db, uint32_t cluster_size,
                         const char *target_identity, uint64_t physical_bytes,
                         bool stop_aware, char **error);
int ntfs_restore_workspace(const char *device, sqlite3 *db, uint32_t cluster_size,
                           const char *target_identity, uint64_t physical_bytes,
                           char **error);
int ntfs_verify_stage(const char *stage, sqlite3 *db, bool growth,
                      bool allow_dirty, char **error);
uint64_t ntfs_plan_move_count(sqlite3 *db, char **error);
int ntfs_set_volume_dirty(NtfsVolume *volume, NtfsLayout *layout, bool dirty, char **error);

#endif
