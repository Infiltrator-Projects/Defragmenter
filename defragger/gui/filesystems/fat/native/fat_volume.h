// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Filesystem-neutral FAT12/16/32 volume model and allocation-table access.
 *
 * Placement, journalling, directory policy and command handling deliberately
 * remain outside this module.  This boundary owns on-disk FAT encoding,
 * geometry validation, mirrored-table writes and cluster-chain traversal.
 */

#ifndef LINUX_DEFRAGGER_FAT_VOLUME_H
#define LINUX_DEFRAGGER_FAT_VOLUME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ld_device.h"

#define FAT32_MASK UINT32_C(0x0FFFFFFF)
#define FAT32_EOC_MIN UINT32_C(0x0FFFFFF8)
#define FAT32_BAD UINT32_C(0x0FFFFFF7)

typedef enum {
    FAT_TYPE_12 = 12,
    FAT_TYPE_16 = 16,
    FAT_TYPE_32 = 32
} FatType;

/*
 * Validated BIOS Parameter Block geometry.  The same parser feeds native
 * probing, the Python-facing identify command and the writable volume loader,
 * so those paths cannot disagree about the FAT width or data boundary.
 */
typedef struct {
    FatType fat_type;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t ext_flags;
    bool fat_mirroring;
    uint8_t active_fat;
    uint32_t sectors_per_fat;
    uint32_t total_sectors;
    uint32_t root_cluster;
    uint16_t root_entry_count;
    uint16_t fsinfo_sector;
    uint16_t backup_boot_sector;
    uint32_t volume_id;
    uint32_t root_dir_sectors;
    uint64_t fat_sectors_total;
    uint64_t volume_bytes;
    uint32_t cluster_count;
    uint32_t max_cluster;
    uint32_t fat_entry_count;
} FatGeometry;

typedef LdDevice Device;

typedef struct {
    uint32_t *v;
    size_t len;
    size_t cap;
} U32Vec;

typedef struct {
    Device dev;
    FatType fat_type;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t ext_flags;
    bool fat_mirroring;
    uint8_t active_fat;
    uint32_t sectors_per_fat;
    uint32_t total_sectors;
    uint32_t root_cluster;
    uint16_t root_entry_count;
    uint64_t root_dir_offset;
    uint64_t root_dir_size;
    bool root_is_fixed;
    uint16_t fsinfo_sector;
    uint16_t backup_boot_sector;
    uint32_t volume_id;
    uint64_t fat0_offset;
    uint64_t data_offset;
    uint64_t cluster_size;
    uint32_t cluster_count;
    uint32_t max_cluster;
    uint32_t fat_entry_count;
    uint32_t *fat;
    uint8_t *visited_dirs;
    uint8_t *claimed_clusters;
    uint8_t *chain_seen;
    bool recovery_mode;
} Fat32;

typedef struct {
    uint32_t cluster;
    uint32_t value;
} FatUpdate;

void u32vec_push(U32Vec *vector, uint32_t value);
void u32vec_free(U32Vec *vector);

uint64_t cluster_offset(const Fat32 *fs, uint32_t cluster);
uint32_t fat_mask(const Fat32 *fs);
uint32_t fat_eoc_min(const Fat32 *fs);
uint32_t fat_bad_value(const Fat32 *fs);
uint32_t fat_reserved_min(const Fat32 *fs);
uint32_t fat_eoc_value(const Fat32 *fs);
const char *fat_type_name(const Fat32 *fs);
uint32_t fat_value(const Fat32 *fs, uint32_t cluster);
bool fat_is_eoc_for(const Fat32 *fs, uint32_t value);
bool fat_is_free(const Fat32 *fs, uint32_t cluster);

bool fat_geometry_parse(
    const uint8_t boot[512],
    uint64_t target_bytes,
    FatGeometry *geometry,
    char *error,
    size_t error_size
);
bool fat_geometry_probe_path(const char *path, FatType *type_out);

void fat32_load(Fat32 *fs, Device device, bool allow_mirror_mismatch);
void fat32_unload(Fat32 *fs);
void fat32_sync(Fat32 *fs);
void fat32_write_entry(Fat32 *fs, uint32_t cluster, uint32_t new_value);
void fat32_apply_updates(
    Fat32 *fs,
    const FatUpdate *updates,
    size_t count
);
U32Vec fat32_read_chain(Fat32 *fs, uint32_t first);

#endif
