// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LINUX_DEFRAGGER_NTFS_TRANSACTION_H
#define LINUX_DEFRAGGER_NTFS_TRANSACTION_H

#include <stdint.h>

typedef struct {
    char *device;
    char *target_identity;
    char serial[17];
    char operation[24];
    char phase[32];
    char *stage;
    char *plan;
    uint64_t physical_bytes;
    uint64_t filesystem_bytes;
    uint64_t commit_cluster;
    uint64_t move_clusters;
    uint64_t workspace_start;
    uint64_t workspace_clusters;
} NtfsJournal;

/*
 * Filesystem-specific persistent transaction record.
 *
 * This component owns only NTFS journal representation, parsing and durable
 * publication. Placement, commit ordering, phase meaning and recovery policy
 * remain in ntfs_worker.c.
 */
void ntfs_transaction_cleanup(const char *journal, const NtfsJournal *state);
void ntfs_journal_free(NtfsJournal *state);
int ntfs_journal_save(const char *path, const NtfsJournal *state, char **error);
int ntfs_journal_load(const char *path, NtfsJournal *state, char **error);
int ntfs_journal_phase(const char *path, NtfsJournal *state,
                       const char *phase, char **error);

#endif
