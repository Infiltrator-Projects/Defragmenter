/* SPDX-License-Identifier: GPL-3.0-or-later
 * The trusted HFS.so exclusively implements classic HFS analysis, Defrag and
 * durable recovery. Defragmenter hosts only generic device/security services.
 */
#include "filesystem_module_host.h"
#include "ifs_hfs_module_v1.h"
#include "ifs_defrag_host_services.h"
#include <stdio.h>
#include <string.h>
typedef struct {int argc;char **argv;} Request;
static int call(const IfsDefragHostV2 *h,void *symbol,void *opaque) {
 (void)h;
 const IfsHfsModuleV1 *(*entry)(void)=NULL;
 _Static_assert(sizeof(entry)==sizeof(symbol),"pointer width");
 memcpy(&entry,&symbol,sizeof(entry));
 const IfsHfsModuleV1 *api=entry?entry():NULL;
 if(!api || api->abi_version!=IFS_HFS_MODULE_V1 ||
    api->structure_bytes<sizeof(*api) || !api->format_id ||
    strcmp(api->format_id,"hfs") ||
    (api->capabilities & IFS_HFS_CAP_RUN)==0 || !api->run) {
    fputs("HFS.so: missing or incompatible module\n",stderr);return 1;
 }
 const Request *r=opaque;
 return api->run(r->argc,r->argv,ifs_defrag_services_v1());
}
int main(int argc,char **argv) {
 Request r={argc,argv};
 return ifs_filesystem_module_call("hfs","HFS.so",
  "infiltrator_filesystem_hfs_v1",call,&r);
}
