/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DEFRAGMENTER_FILESYSTEM_MODULE_HOST_H
#define DEFRAGMENTER_FILESYSTEM_MODULE_HOST_H
#include "filesystem_defrag_abi.h"
typedef int (*IfsFilesystemCall)(const IfsDefragHostV2 *, void *, void *);
/* A non-Defrag callback must validate its requested ABI, complete structure,
 * format identity and required methods before opening a writable target. */
int ifs_filesystem_module_call(const char *, const char *, const char *,
                               IfsFilesystemCall, void *);
int ifs_filesystem_module_run(const char *, const char *, int, char **);
/* Dedicated, existing OFS native worker only; never a Common V2 fallback. */
int ifs_filesystem_module_run_ofs_native(int, char **);
/* Discover root-owned modules advertising the standard Defrag V2 ABI. */
int ifs_filesystem_module_list_v2(void);
/* Resolve a trusted installed SO by its exported format_id, not filename. */
int ifs_filesystem_module_find_v2(const char *, char *, size_t);
int ifs_filesystem_journal_matches(const char *, const char *);
#endif
