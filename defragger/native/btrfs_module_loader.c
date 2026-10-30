/* SPDX-License-Identifier: GPL-3.0-or-later
 * Generic BTRFS.so loader. The module owns all filesystem-specific behavior.
 */
#include "filesystem_module_host.h"
#include "ifs_btrfs_module_v1.h"
#include "ifs_defrag_host_services.h"
#include <stdio.h>
#include <string.h>
typedef struct {int argc;char **argv;} Request;
static int execute(const IfsDefragHostV2 *host,void *symbol,void *opaque) {
 (void)host;
 const IfsBtrfsModuleV1 *(*entry)(void)=NULL;
 _Static_assert(sizeof(entry)==sizeof(symbol),"function pointer size");
 memcpy(&entry,&symbol,sizeof(entry));
 const IfsBtrfsModuleV1 *api=entry?entry():NULL;
 if(!api || api->abi_version!=IFS_BTRFS_MODULE_V1 ||
   api->structure_bytes<sizeof(*api) || !api->format_id ||
   strcmp(api->format_id,"btrfs") ||
   (api->capabilities&IFS_BTRFS_CAP_RUN)==0 || !api->run) {
  fputs("BTRFS.so: missing or incompatible driver\n",stderr);return 1;
 }
 const Request *r=opaque;
 return api->run(r->argc,r->argv,ifs_defrag_services_v1());
}
int main(int argc,char **argv) {
 Request r={argc,argv};
 return ifs_filesystem_module_call("btrfs","BTRFS.so",
   "infiltrator_filesystem_btrfs_v1",execute,&r);
}
