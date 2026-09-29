// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Durable FAT relocation mechanics and recovery.
 *
 * Placement policy remains in the writer/growth planner.  This module owns
 * the one authoritative procedure that copies data, switches every metadata
 * reference, frees the old allocation and advances a durable journal.
 */

#include "fat_relocation.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "ld_io.h"
#include "ld_runtime.h"

#include "infiltratr/endian.h"

typedef struct {
    uint64_t sector_offset;
    uint64_t entry_offset;
    uint32_t new_target;
} DirentSectorPatch;

static int compare_dirent_sector_patch(const void *a, const void *b) {
    const DirentSectorPatch *left = a;
    const DirentSectorPatch *right = b;
    if (left->sector_offset < right->sector_offset) return -1;
    if (left->sector_offset > right->sector_offset) return 1;
    if (left->entry_offset < right->entry_offset) return -1;
    if (left->entry_offset > right->entry_offset) return 1;
    if (left->new_target < right->new_target) return -1;
    if (left->new_target > right->new_target) return 1;
    return 0;
}

static size_t write_dirent_first_clusters_batched(
    Fat32 *filesystem,
    const RelocationDirPatch *patches,
    size_t count
) {
    if (count == 0) return 0;
    size_t bytes_per_sector = filesystem->bytes_per_sector;
    if (bytes_per_sector < 32 || bytes_per_sector % 32 != 0) {
        ld_die("FAT sector size cannot contain aligned directory entries");
    }
    DirentSectorPatch *ordered = ld_xmalloc(count * sizeof(*ordered));
    for (size_t index = 0; index < count; index++) {
        uint64_t offset = patches[index].offset;
        uint64_t sector_offset = offset - (offset % bytes_per_sector);
        uint64_t within = offset - sector_offset;
        if (within % 32 != 0 || within + 32 > bytes_per_sector) {
            free(ordered);
            ld_die("directory entry is not aligned inside a FAT sector");
        }
        ordered[index] = (DirentSectorPatch){
            .sector_offset = sector_offset,
            .entry_offset = offset,
            .new_target = patches[index].new_target,
        };
    }
    qsort(ordered, count, sizeof(*ordered), compare_dirent_sector_patch);

    uint8_t *sector = ld_xmalloc(bytes_per_sector);
    size_t sector_writes = 0;
    size_t index = 0;
    while (index < count) {
        uint64_t sector_offset = ordered[index].sector_offset;
        if (ld_pread_full(
                filesystem->dev.fd,
                sector,
                bytes_per_sector,
                sector_offset) != (ssize_t)bytes_per_sector) {
            free(sector);
            free(ordered);
            ld_die_errno("read directory sector batch");
        }
        size_t end = index;
        while (end < count && ordered[end].sector_offset == sector_offset) {
            size_t within = (size_t)(ordered[end].entry_offset - sector_offset);
            if (end > index
                    && ordered[end].entry_offset == ordered[end - 1].entry_offset
                    && ordered[end].new_target != ordered[end - 1].new_target) {
                free(sector);
                free(ordered);
                ld_die("conflicting directory-entry patches in one transaction");
            }
            infiltratr_store_le16(
                sector + within + 20,
                filesystem->fat_type == FAT_TYPE_32
                    ? (uint16_t)((ordered[end].new_target >> 16) & UINT32_C(0xFFFF))
                    : 0
            );
            infiltratr_store_le16(
                sector + within + 26,
                (uint16_t)(ordered[end].new_target & UINT32_C(0xFFFF))
            );
            end++;
        }
        if (ld_pwrite_full(
                filesystem->dev.fd,
                sector,
                bytes_per_sector,
                sector_offset) != (ssize_t)bytes_per_sector) {
            free(sector);
            free(ordered);
            ld_die_errno("write directory sector batch");
        }
        sector_writes++;
        index = end;
    }
    free(sector);
    free(ordered);
    return sector_writes;
}

static uint32_t read_dirent_first_cluster(Fat32 *filesystem, uint64_t offset) {
    uint8_t entry[32];
    if (ld_pread_full(
            filesystem->dev.fd,
            entry,
            sizeof(entry),
            offset) != (ssize_t)sizeof(entry)) {
        ld_die_errno("read directory entry");
    }
    uint32_t first = infiltratr_load_le16(entry + 26);
    if (filesystem->fat_type == FAT_TYPE_32) {
        first |= (uint32_t)infiltratr_load_le16(entry + 20) << 16;
    }
    return first & fat_mask(filesystem);
}

static uint32_t data_offset_to_cluster(
    const Fat32 *filesystem,
    uint64_t offset
) {
    if (offset < filesystem->data_offset) {
        ld_die("directory reference lies outside the data region");
    }
    uint64_t relative = offset - filesystem->data_offset;
    uint64_t index = relative / filesystem->cluster_size;
    if (index >= filesystem->cluster_count) {
        ld_die("directory reference lies beyond the data region");
    }
    return (uint32_t)index + 2;
}

static uint64_t move_offset_between_clusters(
    const Fat32 *filesystem,
    uint64_t offset,
    uint32_t source,
    uint32_t destination
) {
    uint64_t source_offset = cluster_offset(filesystem, source);
    if (offset < source_offset
            || offset >= source_offset + filesystem->cluster_size) {
        ld_die("internal error: reference offset is not inside its source cluster");
    }
    return cluster_offset(filesystem, destination) + (offset - source_offset);
}

static void write_boot_root_cluster_one(
    Fat32 *filesystem,
    uint16_t sector_number,
    uint32_t root_cluster
) {
    if (sector_number == UINT16_C(0xFFFF)
            || sector_number >= filesystem->reserved_sectors) {
        return;
    }
    size_t bytes_per_sector = filesystem->bytes_per_sector;
    uint8_t *sector = ld_xmalloc(bytes_per_sector);
    uint64_t offset = (uint64_t)sector_number * bytes_per_sector;
    if (ld_pread_full(
            filesystem->dev.fd,
            sector,
            bytes_per_sector,
            offset) != (ssize_t)bytes_per_sector) {
        ld_die_errno("read boot sector");
    }
    if (bytes_per_sector < 512 || sector[510] != 0x55 || sector[511] != 0xAA) {
        free(sector);
        ld_die("invalid FAT32 boot-sector copy while updating root cluster");
    }
    infiltratr_store_le32(sector + 44, root_cluster & FAT32_MASK);
    if (ld_pwrite_full(
            filesystem->dev.fd,
            sector,
            bytes_per_sector,
            offset) != (ssize_t)bytes_per_sector) {
        free(sector);
        ld_die_errno("write boot sector");
    }
    free(sector);
}

static void write_root_cluster(Fat32 *filesystem, uint32_t root_cluster) {
    if (filesystem->root_is_fixed) {
        ld_die("internal error: FAT12/FAT16 root directory cannot be relocated");
    }
    write_boot_root_cluster_one(filesystem, 0, root_cluster);
    if (filesystem->backup_boot_sector != 0
            && filesystem->backup_boot_sector != UINT16_C(0xFFFF)) {
        write_boot_root_cluster_one(
            filesystem,
            filesystem->backup_boot_sector,
            root_cluster
        );
    }
    filesystem->root_cluster = root_cluster;
}

static void update_fsinfo_one(
    Fat32 *filesystem,
    uint16_t sector_number,
    uint32_t next_free
) {
    if (sector_number == 0 || sector_number == UINT16_C(0xFFFF)
            || sector_number >= filesystem->reserved_sectors) {
        return;
    }
    size_t bytes_per_sector = filesystem->bytes_per_sector;
    uint8_t *sector = ld_xmalloc(bytes_per_sector);
    uint64_t offset = (uint64_t)sector_number * bytes_per_sector;
    if (ld_pread_full(
            filesystem->dev.fd,
            sector,
            bytes_per_sector,
            offset) != (ssize_t)bytes_per_sector) {
        ld_die_errno("read FSInfo");
    }
    if (bytes_per_sector >= 512
            && infiltratr_load_le32(sector) == UINT32_C(0x41615252)
            && infiltratr_load_le32(sector + 484) == UINT32_C(0x61417272)
            && infiltratr_load_le32(sector + 508) == UINT32_C(0xAA550000)) {
        infiltratr_store_le32(sector + 492, next_free);
        if (ld_pwrite_full(
                filesystem->dev.fd,
                sector,
                bytes_per_sector,
                offset) != (ssize_t)bytes_per_sector) {
            free(sector);
            ld_die_errno("write FSInfo");
        }
    }
    free(sector);
}

void fat_relocation_update_fsinfo(
    Fat32 *filesystem,
    uint32_t next_free
) {
    if (filesystem->fat_type != FAT_TYPE_32) return;
    update_fsinfo_one(filesystem, filesystem->fsinfo_sector, next_free);
    if (filesystem->backup_boot_sector != 0
            && filesystem->backup_boot_sector != UINT16_C(0xFFFF)) {
        uint32_t backup =
            (uint32_t)filesystem->backup_boot_sector + filesystem->fsinfo_sector;
        if (backup < filesystem->reserved_sectors) {
            update_fsinfo_one(filesystem, (uint16_t)backup, next_free);
        }
    }
}

uint32_t fat_relocation_first_free_hint(const Fat32 *filesystem) {
    for (uint32_t cluster = 2; cluster <= filesystem->max_cluster; cluster++) {
        if (fat_is_free(filesystem, cluster)) return cluster;
    }
    return UINT32_C(0xFFFFFFFF);
}

static void free_cluster_list(
    Fat32 *filesystem,
    const uint32_t *clusters,
    size_t count
) {
    FatUpdate *updates = ld_xmalloc(count * sizeof(*updates));
    for (size_t index = 0; index < count; index++) {
        updates[index] = (FatUpdate){.cluster = clusters[index], .value = 0};
    }
    fat32_apply_updates(filesystem, updates, count);
    free(updates);
}

static void free_contiguous_run(
    Fat32 *filesystem,
    uint32_t start,
    size_t count
) {
    FatUpdate *updates = ld_xmalloc(count * sizeof(*updates));
    for (size_t index = 0; index < count; index++) {
        updates[index] = (FatUpdate){
            .cluster = start + (uint32_t)index,
            .value = 0,
        };
    }
    fat32_apply_updates(filesystem, updates, count);
    free(updates);
}

void fat_relocation_recover_legacy(
    Fat32 *filesystem,
    const char *journal_path
) {
    Journal journal = journal_read(journal_path);
    if (strcmp(journal.device_path, filesystem->dev.path) != 0) {
        ld_die("journal belongs to a different device path");
    }
    if (journal.volume_id != filesystem->volume_id) {
        ld_die("journal volume ID does not match target");
    }
    if (journal.dest_start < 2 || journal.source.len > filesystem->cluster_count
            || journal.dest_start
                > filesystem->max_cluster - (uint32_t)(journal.source.len - 1)) {
        ld_die("journal destination range is invalid");
    }

    uint32_t current = read_dirent_first_cluster(
        filesystem,
        journal.dirent_offset
    );
    fprintf(
        stderr,
        "Recovering interrupted move at journal stage %d...\n",
        (int)journal.stage
    );
    if (current == journal.dest_start) {
        for (size_t index = 0; index < journal.source.len; index++) {
            uint32_t destination = journal.dest_start + (uint32_t)index;
            uint32_t next = index + 1 == journal.source.len
                ? fat_eoc_value(filesystem)
                : destination + 1;
            fat32_write_entry(filesystem, destination, next);
        }
        free_cluster_list(filesystem, journal.source.v, journal.source.len);
        fat_relocation_update_fsinfo(filesystem, journal.source.v[0]);
        fat32_sync(filesystem);
        fprintf(stderr, "Recovery completed by keeping the new contiguous chain.\n");
    } else if (current == journal.old_first) {
        free_contiguous_run(
            filesystem,
            journal.dest_start,
            journal.source.len
        );
        fat32_sync(filesystem);
        fprintf(stderr, "Recovery completed by rolling back the destination chain.\n");
    } else {
        journal_free(&journal);
        ld_die("directory entry points to neither old nor new chain; manual recovery required");
    }
    journal_remove(journal_path);
    journal_free(&journal);
}

static bool fat_value_is_cluster(const Fat32 *filesystem, uint32_t value) {
    value &= fat_mask(filesystem);
    return value >= 2 && value <= filesystem->max_cluster;
}

static uint32_t *build_relocation_map(
    const Fat32 *filesystem,
    const RelocationJournal *journal
) {
    uint32_t *map = ld_xcalloc(
        (size_t)filesystem->max_cluster + 1,
        sizeof(*map)
    );
    uint8_t *destinations = ld_xcalloc(
        (size_t)filesystem->max_cluster + 1,
        1
    );
    for (size_t index = 0; index < journal->move_count; index++) {
        const RelocationMove *move = &journal->moves[index];
        if (move->source < 2 || move->source > filesystem->max_cluster
                || move->destination < 2
                || move->destination > filesystem->max_cluster
                || move->destination == move->source) {
            free(destinations);
            free(map);
            ld_die("relocation journal contains an invalid source/destination pair");
        }
        if (map[move->source] != 0 || destinations[move->destination] != 0) {
            free(destinations);
            free(map);
            ld_die("relocation journal contains duplicate source or destination clusters");
        }
        map[move->source] = move->destination;
        destinations[move->destination] = 1;
    }
    for (size_t index = 0; index < journal->move_count; index++) {
        if (destinations[journal->moves[index].source] != 0) {
            free(destinations);
            free(map);
            ld_die("relocation journal source and destination sets overlap");
        }
    }
    free(destinations);
    return map;
}

static uint32_t *build_predecessor_table(Fat32 *filesystem) {
    uint32_t *predecessors = ld_xcalloc(
        (size_t)filesystem->max_cluster + 1,
        sizeof(*predecessors)
    );
    for (uint32_t cluster = 2; cluster <= filesystem->max_cluster; cluster++) {
        uint32_t next = fat_value(filesystem, cluster);
        if (!fat_value_is_cluster(filesystem, next)) continue;
        if (predecessors[next] != 0) {
            free(predecessors);
            ld_die("multiple FAT predecessors detected while building relocation map");
        }
        predecessors[next] = cluster;
    }
    return predecessors;
}

static RelocationJournal make_relocation_journal(
    Fat32 *filesystem,
    const DirRefList *directory_references,
    const uint32_t *predecessors,
    const RelocationMove *moves,
    size_t move_count
) {
    char target_identity[160];
    if (ld_device_format_identity(&filesystem->dev, target_identity,
                                  sizeof(target_identity)) != 0) {
        ld_die_errno("capture FAT transaction target identity");
    }
    RelocationJournal journal = {
        .device_path = ld_xstrdup(filesystem->dev.path),
        .target_identity = ld_xstrdup(target_identity),
        .physical_bytes = filesystem->dev.size_bytes,
        .volume_id = filesystem->volume_id,
        .stage = J_PREPARED,
        .root_old = filesystem->root_cluster,
        .root_new = filesystem->root_cluster,
    };
    for (size_t index = 0; index < move_count; index++) {
        RelocationMove move = moves[index];
        move.next = fat_value(filesystem, move.source);
        move.predecessor = predecessors[move.source];
        relocation_journal_add_move(&journal, move);
    }

    uint32_t *map = build_relocation_map(filesystem, &journal);
    if (map[journal.root_old] != 0) journal.root_new = map[journal.root_old];
    uint8_t *first_has_reference = ld_xcalloc(
        (size_t)filesystem->max_cluster + 1,
        1
    );
    for (size_t index = 0; index < directory_references->len; index++) {
        const DirRef *reference = &directory_references->v[index];
        uint32_t new_target = map[reference->target_cluster];
        if (new_target == 0) continue;
        uint64_t patch_offset = reference->offset;
        bool in_fixed_root = filesystem->root_is_fixed
            && reference->offset >= filesystem->root_dir_offset
            && reference->offset
                < filesystem->root_dir_offset + filesystem->root_dir_size;
        if (!in_fixed_root) {
            uint32_t container = data_offset_to_cluster(
                filesystem,
                reference->offset
            );
            if (map[container] != 0) {
                patch_offset = move_offset_between_clusters(
                    filesystem,
                    reference->offset,
                    container,
                    map[container]
                );
            }
        }
        relocation_journal_add_dir_patch(
            &journal,
            (RelocationDirPatch){
                .offset = patch_offset,
                .old_target = reference->target_cluster,
                .new_target = new_target,
            }
        );
        first_has_reference[reference->target_cluster] = 1;
    }

    for (size_t index = 0; index < journal.move_count; index++) {
        const RelocationMove *move = &journal.moves[index];
        if (move->predecessor == 0 && move->source != journal.root_old
                && first_has_reference[move->source] == 0) {
            free(first_has_reference);
            free(map);
            relocation_journal_free(&journal);
            ld_die("first cluster has no directory reference during packed defragmentation");
        }
    }
    free(first_has_reference);
    free(map);
    return journal;
}

static void complete_relocation_journal(
    Fat32 *filesystem,
    const char *journal_path,
    RelocationJournal *journal,
    FatIoConfig *io,
    FatRelocationLog detail_log
) {
    if (strcmp(journal->device_path, filesystem->dev.path) != 0) {
        ld_die("relocation journal belongs to a different device path");
    }
    if (!ld_device_matches_identity(&filesystem->dev,
                                    journal->target_identity,
                                    journal->physical_bytes)) {
        ld_die("relocation journal target identity or capacity does not match the opened FAT target");
    }
    if (journal->volume_id != filesystem->volume_id) {
        ld_die("relocation journal volume ID does not match target");
    }
    uint32_t *map = build_relocation_map(filesystem, journal);

    uint32_t *sources = ld_xmalloc(journal->move_count * sizeof(*sources));
    uint32_t *destinations = ld_xmalloc(
        journal->move_count * sizeof(*destinations)
    );
    for (size_t index = 0; index < journal->move_count; index++) {
        sources[index] = journal->moves[index].source;
        destinations[index] = journal->moves[index].destination;
    }
    fat_io_copy_clusters(
        filesystem,
        sources,
        destinations,
        journal->move_count,
        io
    );
    free(sources);
    free(destinations);
    fat32_sync(filesystem);
    journal->stage = J_DATA_COPIED;
    relocation_journal_write(journal_path, journal);

    FatUpdate *link_updates = ld_xmalloc(
        journal->move_count * 2 * sizeof(*link_updates)
    );
    size_t link_count = 0;
    for (size_t index = 0; index < journal->move_count; index++) {
        const RelocationMove *move = &journal->moves[index];
        uint32_t new_next = move->next;
        if (fat_value_is_cluster(filesystem, move->next)
                && map[move->next] != 0) {
            new_next = map[move->next];
        }
        link_updates[link_count++] = (FatUpdate){
            .cluster = move->destination,
            .value = new_next,
        };
    }
    for (size_t index = 0; index < journal->move_count; index++) {
        const RelocationMove *move = &journal->moves[index];
        if (move->predecessor != 0 && map[move->predecessor] == 0) {
            link_updates[link_count++] = (FatUpdate){
                .cluster = move->predecessor,
                .value = move->destination,
            };
        }
    }
    fat32_apply_updates(filesystem, link_updates, link_count);
    free(link_updates);

    size_t sector_writes = write_dirent_first_clusters_batched(
        filesystem,
        journal->dir_patches,
        journal->dir_patch_count
    );
    if (detail_log != NULL && journal->dir_patch_count != 0) {
        detail_log(
            "Directory metadata:      %zu entr%s in %zu sector write%s\n",
            journal->dir_patch_count,
            journal->dir_patch_count == 1 ? "y" : "ies",
            sector_writes,
            sector_writes == 1 ? "" : "s"
        );
    }
    if (journal->root_new != journal->root_old) {
        write_root_cluster(filesystem, journal->root_new);
    }
    fat32_sync(filesystem);
    journal->stage = J_SWITCHED;
    relocation_journal_write(journal_path, journal);

    uint32_t next_free = filesystem->max_cluster;
    uint32_t *old_sources = ld_xmalloc(
        journal->move_count * sizeof(*old_sources)
    );
    for (size_t index = 0; index < journal->move_count; index++) {
        old_sources[index] = journal->moves[index].source;
        if (journal->moves[index].source < next_free) {
            next_free = journal->moves[index].source;
        }
    }
    free_cluster_list(filesystem, old_sources, journal->move_count);
    free(old_sources);
    fat_relocation_update_fsinfo(filesystem, next_free);
    fat32_sync(filesystem);
    journal->stage = J_OLD_FREED;
    relocation_journal_write(journal_path, journal);

    journal_remove(journal_path);
    free(map);
}

void fat_relocation_execute(
    Fat32 *filesystem,
    const DirRefList *directory_references,
    const char *journal_path,
    const RelocationMove *moves,
    size_t move_count,
    FatIoConfig *io,
    FatRelocationLog detail_log
) {
    uint32_t *predecessors = build_predecessor_table(filesystem);
    RelocationJournal journal = make_relocation_journal(
        filesystem,
        directory_references,
        predecessors,
        moves,
        move_count
    );
    free(predecessors);
    relocation_journal_write(journal_path, &journal);
    complete_relocation_journal(
        filesystem,
        journal_path,
        &journal,
        io,
        detail_log
    );
    relocation_journal_free(&journal);
}

void fat_relocation_recover_mapped(
    Fat32 *filesystem,
    const char *journal_path,
    FatIoConfig *io,
    FatRelocationLog detail_log
) {
    RelocationJournal journal = relocation_journal_read(journal_path);
    fprintf(
        stderr,
        "Recovering interrupted mapped FAT relocation of %zu cluster%s...\n",
        journal.move_count,
        journal.move_count == 1 ? "" : "s"
    );
    complete_relocation_journal(
        filesystem,
        journal_path,
        &journal,
        io,
        detail_log
    );
    fprintf(
        stderr,
        "Mapped FAT recovery completed by finishing the recorded transaction.\n"
    );
    relocation_journal_free(&journal);
}
