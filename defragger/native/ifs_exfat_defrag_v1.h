/* SPDX-License-Identifier: GPL-3.0-or-later
 * Linux exFAT defragmentation entry point; V5 file API remains unchanged.
 */
#ifndef IFS_EXFAT_DEFRAG_V1_H
#define IFS_EXFAT_DEFRAG_V1_H
#include "ifs_defrag_services_v1.h"
#define IFS_EXFAT_DEFRAG_ABI_V1 1u
#define IFS_EXFAT_DEFRAG_CAP_RUN UINT64_C(1)
typedef struct {
  uint32_t abi_version,structure_bytes;
  const char *format_id;
  uint64_t capabilities;
  int (*run)(int argc,char **argv,const IfsDefragServicesV1 *host);
} IfsExfatDefragV1;
const IfsExfatDefragV1 *infiltrator_filesystem_exfat_defrag_v1(void);
#endif
