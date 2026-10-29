/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DEFRAGMENTER_FILESYSTEM_MODULE_HOST_H
#define DEFRAGMENTER_FILESYSTEM_MODULE_HOST_H
#include "filesystem_defrag_abi.h"
typedef int (*IfsFilesystemCall)(const IfsDefragHostV2 *, void *, void *);
int ifs_filesystem_module_call(const char *, const char *, const char *,
                               IfsFilesystemCall, void *);
int ifs_filesystem_module_run(const char *, const char *, int, char **);
int ifs_filesystem_journal_matches(const char *, const char *);
#endif
