// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LINUX_DEFRAGGER_EXT_WORKSPACE_H
#define LINUX_DEFRAGGER_EXT_WORKSPACE_H

#include "ext_native.h"

#include <stdint.h>

#define EXT_WORKSPACE_UNAVAILABLE 1

typedef struct {
    uint64_t start;
    uint64_t blocks;
    uint64_t batch_blocks;
    uint32_t block_size;
} ExtWorkspace;

int ext_workspace_prepare(ExtFs *fs, sqlite3 *db,
                          const ExtGeometry *geometry,
                          uint64_t requested_batch_blocks,
                          ExtWorkspace *workspace, char **error);
int ext_workspace_load(sqlite3 *db, ExtWorkspace *workspace, char **error);
int ext_workspace_stage(int fd, sqlite3 *db, const ExtWorkspace *workspace,
                        char **error);
int ext_workspace_place(int fd, sqlite3 *db, const ExtWorkspace *workspace,
                        char **error);
int ext_workspace_restore(int fd, sqlite3 *db, const ExtWorkspace *workspace,
                          char **error);

#endif
