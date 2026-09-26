// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * FAT12/16/32 geometry, allocation-table encoding and chain traversal.
 */

#include "fat_volume.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ld_io.h"
#include "ld_runtime.h"
#include "infiltratr/arithmetic.h"
#include "infiltratr/endian.h"

#define PROGRAM_NAME "linux-defragger-fat-worker"

void u32vec_push(U32Vec *vector, uint32_t value) {
    if (vector->len == SIZE_MAX ||
        !infiltratr_array_reserve((void **)&vector->v, &vector->cap,
                                  sizeof(*vector->v), vector->len + 1U, 16U))
        ld_die("cannot grow FAT cluster vector");
    vector->v[vector->len++] = value;
}

void u32vec_free(U32Vec *vector) {
    free(vector->v);
    memset(vector, 0, sizeof(*vector));
}

uint64_t cluster_offset(const Fat32 *fs, uint32_t cluster) {
    if (cluster < 2 || cluster > fs->max_cluster) {
        ld_die("cluster number outside data region");
    }
    return fs->data_offset
        + (uint64_t)(cluster - 2) * fs->cluster_size;
}

uint32_t fat_mask(const Fat32 *fs) {
    return fs->fat_type == FAT_TYPE_12 ? UINT32_C(0x0FFF) :
           fs->fat_type == FAT_TYPE_16 ? UINT32_C(0xFFFF) : FAT32_MASK;
}

uint32_t fat_eoc_min(const Fat32 *fs) {
    return fs->fat_type == FAT_TYPE_12 ? UINT32_C(0x0FF8) :
           fs->fat_type == FAT_TYPE_16 ? UINT32_C(0xFFF8) : FAT32_EOC_MIN;
}

uint32_t fat_bad_value(const Fat32 *fs) {
    return fs->fat_type == FAT_TYPE_12 ? UINT32_C(0x0FF7) :
           fs->fat_type == FAT_TYPE_16 ? UINT32_C(0xFFF7) : FAT32_BAD;
}

uint32_t fat_reserved_min(const Fat32 *fs) {
    return fs->fat_type == FAT_TYPE_12 ? UINT32_C(0x0FF0) :
           fs->fat_type == FAT_TYPE_16 ? UINT32_C(0xFFF0) :
           UINT32_C(0x0FFFFFF0);
}

uint32_t fat_eoc_value(const Fat32 *fs) {
    return fat_mask(fs);
}

const char *fat_type_name(const Fat32 *fs) {
    return fs->fat_type == FAT_TYPE_12 ? "FAT12" :
           fs->fat_type == FAT_TYPE_16 ? "FAT16" : "FAT32";
}

static size_t fat_entry_byte_offset(
    const Fat32 *fs,
    uint32_t cluster
) {
    if (fs->fat_type == FAT_TYPE_12) {
        return (size_t)cluster + (size_t)cluster / 2;
    }
    if (fs->fat_type == FAT_TYPE_16) {
        return (size_t)cluster * 2;
    }
    return (size_t)cluster * 4;
}

static uint32_t decode_fat_entry(
    const Fat32 *fs,
    const uint8_t *raw,
    size_t fat_bytes,
    uint32_t cluster
) {
    size_t offset = fat_entry_byte_offset(fs, cluster);
    if (fs->fat_type == FAT_TYPE_12) {
        if (offset + 1 >= fat_bytes) {
            ld_die("FAT12 entry outside loaded table");
        }
        uint16_t pair = infiltratr_load_le16(raw + offset);
        return (cluster & 1u)
            ? (uint32_t)(pair >> 4)
            : (uint32_t)(pair & 0x0FFFu);
    }
    if (fs->fat_type == FAT_TYPE_16) {
        if (offset + 1 >= fat_bytes) {
            ld_die("FAT16 entry outside loaded table");
        }
        return infiltratr_load_le16(raw + offset);
    }
    if (offset + 3 >= fat_bytes) {
        ld_die("FAT32 entry outside loaded table");
    }
    return infiltratr_load_le32(raw + offset) & FAT32_MASK;
}

uint32_t fat_value(const Fat32 *fs, uint32_t cluster) {
    if (cluster >= fs->fat_entry_count) {
        ld_die("FAT entry outside loaded table");
    }
    return fs->fat[cluster] & fat_mask(fs);
}

bool fat_is_eoc_for(const Fat32 *fs, uint32_t value) {
    return (value & fat_mask(fs)) >= fat_eoc_min(fs);
}

bool fat_is_free(const Fat32 *fs, uint32_t cluster) {
    return fat_value(fs, cluster) == 0;
}

static bool geometry_error(char *error, size_t error_size, const char *message) {
    if (error != NULL && error_size != 0) {
        (void)snprintf(error, error_size, "%s", message);
    }
    return false;
}

bool fat_geometry_parse(
    const uint8_t boot[512],
    uint64_t target_bytes,
    FatGeometry *geometry,
    char *error,
    size_t error_size
) {
    if (boot == NULL || geometry == NULL) {
        return geometry_error(error, error_size, "missing FAT geometry input");
    }
    memset(geometry, 0, sizeof(*geometry));
    if (boot[510] != 0x55 || boot[511] != 0xAA) {
        return geometry_error(error, error_size, "invalid boot-sector signature");
    }

    geometry->bytes_per_sector = infiltratr_load_le16(boot + 11);
    geometry->sectors_per_cluster = boot[13];
    geometry->reserved_sectors = infiltratr_load_le16(boot + 14);
    geometry->fat_count = boot[16];
    geometry->root_entry_count = infiltratr_load_le16(boot + 17);
    uint16_t total16 = infiltratr_load_le16(boot + 19);
    uint16_t fat16_sectors = infiltratr_load_le16(boot + 22);
    uint32_t total32 = infiltratr_load_le32(boot + 32);
    uint32_t fat32_sectors = infiltratr_load_le32(boot + 36);
    geometry->total_sectors = total16 != 0 ? total16 : total32;

    if (!(geometry->bytes_per_sector == 512
          || geometry->bytes_per_sector == 1024
          || geometry->bytes_per_sector == 2048
          || geometry->bytes_per_sector == 4096)) {
        return geometry_error(error, error_size, "unsupported bytes-per-sector value");
    }
    if (geometry->sectors_per_cluster == 0
            || (geometry->sectors_per_cluster
                & (geometry->sectors_per_cluster - 1)) != 0) {
        return geometry_error(error, error_size, "invalid sectors-per-cluster value");
    }
    if (geometry->reserved_sectors == 0
            || geometry->fat_count == 0
            || geometry->total_sectors == 0) {
        return geometry_error(error, error_size, "invalid FAT layout fields");
    }

    geometry->root_dir_sectors =
        ((uint32_t)geometry->root_entry_count * 32u
         + geometry->bytes_per_sector - 1u)
        / geometry->bytes_per_sector;
    geometry->sectors_per_fat =
        fat16_sectors != 0 ? fat16_sectors : fat32_sectors;
    if (geometry->sectors_per_fat == 0) {
        return geometry_error(error, error_size, "invalid FAT size");
    }
    geometry->fat_sectors_total =
        (uint64_t)geometry->fat_count * geometry->sectors_per_fat;
    uint64_t occupied = (uint64_t)geometry->reserved_sectors
        + geometry->fat_sectors_total + geometry->root_dir_sectors;
    if (occupied >= geometry->total_sectors) {
        return geometry_error(error, error_size, "invalid FAT/data layout");
    }
    uint64_t data_sectors = (uint64_t)geometry->total_sectors - occupied;
    uint64_t clusters = data_sectors / geometry->sectors_per_cluster;
    if (clusters == 0 || clusters > UINT32_MAX - 1u) {
        return geometry_error(error, error_size, "FAT cluster count is outside the supported range");
    }
    geometry->cluster_count = (uint32_t)clusters;
    geometry->fat_type = geometry->cluster_count < 4085 ? FAT_TYPE_12 :
                         geometry->cluster_count < 65525 ? FAT_TYPE_16 : FAT_TYPE_32;
    geometry->max_cluster = geometry->cluster_count + 1u;
    geometry->volume_bytes =
        (uint64_t)geometry->total_sectors * geometry->bytes_per_sector;
    if (target_bytes != UINT64_MAX && geometry->volume_bytes > target_bytes) {
        return geometry_error(error, error_size, "filesystem extends beyond target size");
    }

    if (geometry->fat_type == FAT_TYPE_32) {
        geometry->ext_flags = infiltratr_load_le16(boot + 40);
        if (infiltratr_load_le16(boot + 42) != 0) {
            return geometry_error(error, error_size, "unsupported nonzero FAT32 filesystem version");
        }
        geometry->fat_mirroring =
            (geometry->ext_flags & UINT16_C(0x0080)) == 0;
        geometry->active_fat = geometry->fat_mirroring
            ? 0
            : (uint8_t)(geometry->ext_flags & UINT16_C(0x000F));
        if (geometry->active_fat >= geometry->fat_count) {
            return geometry_error(error, error_size, "active FAT index is outside the FAT count");
        }
        geometry->root_cluster = infiltratr_load_le32(boot + 44) & FAT32_MASK;
        geometry->fsinfo_sector = infiltratr_load_le16(boot + 48);
        geometry->backup_boot_sector = infiltratr_load_le16(boot + 50);
        geometry->volume_id = infiltratr_load_le32(boot + 67);
        if (geometry->root_entry_count != 0 || fat16_sectors != 0) {
            return geometry_error(error, error_size, "FAT32 layout fields are inconsistent");
        }
        if (geometry->root_cluster < 2
                || geometry->root_cluster > geometry->max_cluster) {
            return geometry_error(error, error_size, "invalid root cluster");
        }
    } else {
        geometry->fat_mirroring = true;
        geometry->active_fat = 0;
        geometry->root_cluster = 0;
        geometry->fsinfo_sector = 0;
        geometry->backup_boot_sector = 0;
        geometry->volume_id = infiltratr_load_le32(boot + 39);
        if (geometry->root_entry_count == 0 || fat16_sectors == 0) {
            return geometry_error(error, error_size, "FAT12/FAT16 layout fields are inconsistent");
        }
    }

    uint64_t fat_bytes =
        (uint64_t)geometry->sectors_per_fat * geometry->bytes_per_sector;
    uint64_t entry_count = geometry->fat_type == FAT_TYPE_12
        ? (fat_bytes * 2u) / 3u
        : geometry->fat_type == FAT_TYPE_16 ? fat_bytes / 2u : fat_bytes / 4u;
    if (entry_count > UINT32_MAX || entry_count <= geometry->max_cluster) {
        return geometry_error(error, error_size, "FAT is too small for the data region");
    }
    geometry->fat_entry_count = (uint32_t)entry_count;
    return true;
}

bool fat_geometry_probe_path(const char *path, FatType *type_out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    uint8_t boot[512];
    ssize_t got = ld_pread_full(fd, boot, sizeof(boot), 0);
    close(fd);
    if (got != (ssize_t)sizeof(boot)) return false;
    FatGeometry geometry;
    if (!fat_geometry_parse(boot, UINT64_MAX, &geometry, NULL, 0)) return false;
    if (type_out != NULL) *type_out = geometry.fat_type;
    return true;
}

void fat32_load(
    Fat32 *fs,
    Device device,
    bool allow_mirror_mismatch
) {
    memset(fs, 0, sizeof(*fs));
    fs->dev = device;
    fs->recovery_mode = allow_mirror_mismatch;

    uint8_t boot[512];
    if (ld_pread_full(fs->dev.fd, boot, sizeof(boot), 0)
            != (ssize_t)sizeof(boot)) {
        ld_die_errno("read boot sector");
    }
    FatGeometry geometry;
    char geometry_error_message[160];
    if (!fat_geometry_parse(
            boot,
            fs->dev.size_bytes,
            &geometry,
            geometry_error_message,
            sizeof(geometry_error_message))) {
        ld_die(geometry_error_message);
    }
    fs->fat_type = geometry.fat_type;
    fs->bytes_per_sector = geometry.bytes_per_sector;
    fs->sectors_per_cluster = geometry.sectors_per_cluster;
    fs->reserved_sectors = geometry.reserved_sectors;
    fs->fat_count = geometry.fat_count;
    fs->root_entry_count = geometry.root_entry_count;
    fs->sectors_per_fat = geometry.sectors_per_fat;
    fs->total_sectors = geometry.total_sectors;
    fs->cluster_count = geometry.cluster_count;
    fs->max_cluster = geometry.max_cluster;
    fs->cluster_size =
        (uint64_t)fs->bytes_per_sector * fs->sectors_per_cluster;
    fs->fat0_offset =
        (uint64_t)fs->reserved_sectors * fs->bytes_per_sector;
    fs->root_dir_offset =
        ((uint64_t)fs->reserved_sectors + geometry.fat_sectors_total)
        * fs->bytes_per_sector;
    fs->root_dir_size =
        (uint64_t)geometry.root_dir_sectors * fs->bytes_per_sector;
    fs->root_is_fixed = fs->fat_type != FAT_TYPE_32;
    fs->data_offset = fs->root_dir_offset + fs->root_dir_size;
    fs->ext_flags = geometry.ext_flags;
    fs->fat_mirroring = geometry.fat_mirroring;
    fs->active_fat = geometry.active_fat;
    fs->root_cluster = geometry.root_cluster;
    fs->fsinfo_sector = geometry.fsinfo_sector;
    fs->backup_boot_sector = geometry.backup_boot_sector;
    fs->volume_id = geometry.volume_id;

    size_t fat_bytes =
        (size_t)fs->sectors_per_fat * fs->bytes_per_sector;
    fs->fat_entry_count = geometry.fat_entry_count;

    uint8_t *raw = ld_xmalloc(fat_bytes);
    uint64_t active_fat_offset =
        fs->fat0_offset + (uint64_t)fs->active_fat * fat_bytes;
    if (ld_pread_full(
            fs->dev.fd,
            raw,
            fat_bytes,
            active_fat_offset
        ) != (ssize_t)fat_bytes) {
        ld_die_errno("read active FAT");
    }
    fs->fat = ld_xcalloc(
        (size_t)fs->fat_entry_count,
        sizeof(*fs->fat)
    );
    for (uint32_t index = 0; index < fs->fat_entry_count; index++) {
        fs->fat[index] =
            decode_fat_entry(fs, raw, fat_bytes, index);
    }

    /*
     * Mounted FAT volumes normally advertise an unclean state while active.
     * That must block mutations, but not a read-only allocation scan.
     */
    if (fs->dev.writable) {
        if (fs->fat_type == FAT_TYPE_32) {
            if ((fs->fat[1] & UINT32_C(0x08000000)) == 0) {
                ld_die(
                    "FAT32 clean-shutdown bit is clear; repair or cleanly "
                    "unmount the volume first"
                );
            }
            if ((fs->fat[1] & UINT32_C(0x04000000)) == 0) {
                ld_die(
                    "FAT32 hard-error bit is clear; repair the volume before "
                    "defragmenting"
                );
            }
        } else if (fs->fat_type == FAT_TYPE_16) {
            if ((fs->fat[1] & UINT32_C(0x8000)) == 0) {
                ld_die(
                    "FAT16 clean-shutdown bit is clear; repair or cleanly "
                    "unmount the volume first"
                );
            }
            if ((fs->fat[1] & UINT32_C(0x4000)) == 0) {
                ld_die(
                    "FAT16 hard-error bit is clear; repair the volume before "
                    "defragmenting"
                );
            }
        }
    }

    if (fs->fat_mirroring) {
        uint8_t *other = ld_xmalloc(fat_bytes);
        for (uint8_t copy = 0; copy < fs->fat_count; copy++) {
            if (copy == fs->active_fat) {
                continue;
            }
            uint64_t offset =
                fs->fat0_offset + (uint64_t)copy * fat_bytes;
            if (ld_pread_full(
                    fs->dev.fd,
                    other,
                    fat_bytes,
                    offset
                ) != (ssize_t)fat_bytes) {
                ld_die_errno("read mirrored FAT");
            }
            for (uint32_t index = 0;
                    index <= fs->max_cluster;
                    index++) {
                if (decode_fat_entry(fs, other, fat_bytes, index)
                        == fat_value(fs, index)) {
                    continue;
                }
                if (!allow_mirror_mismatch && fs->dev.writable) {
                    ld_die(
                        "mirrored FAT copies disagree; repair the filesystem "
                        "before defragmenting"
                    );
                }
                if (fs->dev.writable) {
                    fprintf(
                        stderr,
                        "%s: warning: mirrored FAT copies disagree; recovery "
                        "will rewrite journalled entries\n",
                        PROGRAM_NAME
                    );
                }
                copy = fs->fat_count;
                break;
            }
        }
        free(other);
    }
    free(raw);
    fs->visited_dirs = ld_bitmap_calloc((uint64_t)fs->max_cluster + 1U);
    fs->claimed_clusters = ld_bitmap_calloc((uint64_t)fs->max_cluster + 1U);
    fs->chain_seen = ld_bitmap_calloc((uint64_t)fs->max_cluster + 1U);
    if (fs->visited_dirs == NULL || fs->claimed_clusters == NULL ||
        fs->chain_seen == NULL)
        ld_die("cannot allocate packed FAT traversal guards");
}

void fat32_unload(Fat32 *fs) {
    free(fs->fat);
    free(fs->visited_dirs);
    free(fs->claimed_clusters);
    free(fs->chain_seen);
    Device device = fs->dev;
    memset(fs, 0, sizeof(*fs));
    ld_device_close(&device);
}

void fat32_sync(Fat32 *fs) {
    if (fsync(fs->dev.fd) != 0) {
        ld_die_errno("fsync target");
    }
}

void fat32_write_entry(
    Fat32 *fs,
    uint32_t cluster,
    uint32_t new_value
) {
    if (!fs->dev.writable) {
        ld_die("internal error: attempted write on read-only target");
    }
    if (cluster >= fs->fat_entry_count) {
        ld_die("attempted FAT write outside table");
    }
    size_t fat_bytes =
        (size_t)fs->sectors_per_fat * fs->bytes_per_sector;
    size_t byte_offset = fat_entry_byte_offset(fs, cluster);
    size_t span = fs->fat_type == FAT_TYPE_32 ? 4u : 2u;
    uint8_t first_copy =
        fs->fat_mirroring ? 0 : fs->active_fat;
    uint8_t end_copy = fs->fat_mirroring
        ? fs->fat_count
        : (uint8_t)(fs->active_fat + 1);
    for (uint8_t copy = first_copy; copy < end_copy; copy++) {
        uint8_t raw[4] = {0};
        uint64_t offset =
            fs->fat0_offset
            + (uint64_t)copy * fat_bytes
            + byte_offset;
        if (ld_pread_full(fs->dev.fd, raw, span, offset)
                != (ssize_t)span) {
            ld_die_errno("read FAT entry before update");
        }
        if (fs->fat_type == FAT_TYPE_12) {
            uint16_t pair = infiltratr_load_le16(raw);
            uint32_t value = new_value & UINT32_C(0x0FFF);
            pair = (cluster & 1u)
                ? (uint16_t)((pair & 0x000Fu) | (value << 4))
                : (uint16_t)((pair & 0xF000u) | value);
            infiltratr_store_le16(raw, pair);
        } else if (fs->fat_type == FAT_TYPE_16) {
            infiltratr_store_le16(raw, (uint16_t)new_value);
        } else {
            uint32_t old = infiltratr_load_le32(raw);
            infiltratr_store_le32(
                raw,
                (old & UINT32_C(0xF0000000))
                | (new_value & FAT32_MASK)
            );
        }
        if (ld_pwrite_full(fs->dev.fd, raw, span, offset)
                != (ssize_t)span) {
            ld_die_errno("write FAT entry");
        }
    }
    fs->fat[cluster] = new_value & fat_mask(fs);
}

static int compare_u32_values(const void *left, const void *right) {
    uint32_t left_value = *(const uint32_t *)left;
    uint32_t right_value = *(const uint32_t *)right;
    return left_value < right_value
        ? -1
        : left_value > right_value ? 1 : 0;
}

void fat32_apply_updates(
    Fat32 *fs,
    const FatUpdate *updates,
    size_t count
) {
    if (count == 0) {
        return;
    }
    if (!fs->dev.writable) {
        ld_die(
            "internal error: attempted bulk FAT write on read-only target"
        );
    }
    if (fs->fat_type != FAT_TYPE_32
            || (fs->recovery_mode && fs->fat_mirroring)) {
        for (size_t index = 0; index < count; index++) {
            fat32_write_entry(
                fs,
                updates[index].cluster,
                updates[index].value
            );
        }
        return;
    }

    uint32_t *sectors = ld_xmalloc(count * sizeof(*sectors));
    for (size_t index = 0; index < count; index++) {
        uint32_t cluster = updates[index].cluster;
        if (cluster >= fs->fat_entry_count) {
            free(sectors);
            ld_die("attempted bulk FAT write outside table");
        }
        fs->fat[cluster] =
            (fs->fat[cluster] & UINT32_C(0xF0000000))
            | (updates[index].value & FAT32_MASK);
        sectors[index] = (uint32_t)(
            ((uint64_t)cluster * 4) / fs->bytes_per_sector
        );
    }
    qsort(
        sectors,
        count,
        sizeof(*sectors),
        compare_u32_values
    );
    size_t unique = 0;
    for (size_t index = 0; index < count; index++) {
        if (unique == 0 || sectors[index] != sectors[unique - 1]) {
            sectors[unique++] = sectors[index];
        }
    }

    size_t fat_bytes =
        (size_t)fs->sectors_per_fat * fs->bytes_per_sector;
    uint8_t first_copy =
        fs->fat_mirroring ? 0 : fs->active_fat;
    uint8_t end_copy = fs->fat_mirroring
        ? fs->fat_count
        : (uint8_t)(fs->active_fat + 1);
    size_t run_start_index = 0;
    while (run_start_index < unique) {
        size_t run_end_index = run_start_index + 1;
        while (run_end_index < unique
                && sectors[run_end_index]
                    == sectors[run_end_index - 1] + 1) {
            run_end_index++;
        }
        uint32_t first_sector = sectors[run_start_index];
        size_t sector_count =
            run_end_index - run_start_index;
        size_t bytes = sector_count * fs->bytes_per_sector;
        uint8_t *raw = ld_xmalloc(bytes);
        size_t first_entry =
            ((size_t)first_sector * fs->bytes_per_sector) / 4;
        size_t entry_count = bytes / 4;
        for (size_t entry = 0; entry < entry_count; entry++) {
            infiltratr_store_le32(
                raw + entry * 4,
                fs->fat[first_entry + entry]
            );
        }
        for (uint8_t copy = first_copy; copy < end_copy; copy++) {
            uint64_t offset =
                fs->fat0_offset
                + (uint64_t)copy * fat_bytes
                + (uint64_t)first_sector * fs->bytes_per_sector;
            if (ld_pwrite_full(fs->dev.fd, raw, bytes, offset)
                    != (ssize_t)bytes) {
                free(raw);
                free(sectors);
                ld_die_errno("write FAT sector batch");
            }
        }
        free(raw);
        run_start_index = run_end_index;
    }
    free(sectors);
}

U32Vec fat32_read_chain(Fat32 *fs, uint32_t first) {
    U32Vec chain = {0};
    if (first == 0) {
        return chain;
    }
    if (first < 2 || first > fs->max_cluster) {
        ld_die("file begins at invalid cluster");
    }
    uint32_t current = first;
    for (;;) {
        if (current < 2 || current > fs->max_cluster) {
            ld_die("cluster chain points outside data region");
        }
        if (ld_bitmap_get(fs->chain_seen, current)) {
            ld_die("cluster-chain loop detected");
        }
        ld_bitmap_set(fs->chain_seen, current, true);
        u32vec_push(&chain, current);
        uint32_t next = fat_value(fs, current);
        if (fat_is_eoc_for(fs, next)) {
            break;
        }
        if (next == 0) {
            ld_die("allocated chain terminates in a free FAT entry");
        }
        if (next == fat_bad_value(fs)
                || (next >= fat_reserved_min(fs)
                    && next < fat_eoc_min(fs))) {
            ld_die(
                "cluster chain reaches a bad or reserved FAT entry"
            );
        }
        current = next;
        if (chain.len > fs->cluster_count) {
            ld_die("cluster chain exceeds volume cluster count");
        }
    }
    for (size_t index = 0U; index < chain.len; ++index)
        ld_bitmap_set(fs->chain_seen, chain.v[index], false);
    return chain;
}
