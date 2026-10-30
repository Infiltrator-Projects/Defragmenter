/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "filesystem_module_host.h"
#include "ifs_xfs_module_v1.h"
#include "ifs_defrag_host_services.h"
#include <stdio.h>
#include <string.h>
typedef struct { int argc; char **argv; } Request;
static int invoke(const IfsDefragHostV2 *host,void *symbol,void *opaque) {
 (void)host;
 const IfsXfsModuleV1 *(*entry)(void)=NULL;
 _Static_assert(sizeof(entry)==sizeof(symbol),"function pointer width mismatch");
 memcpy(&entry,&symbol,sizeof(entry));
 const IfsXfsModuleV1 *api=entry ? entry() : NULL;
 if(!api || api->abi_version!=IFS_XFS_MODULE_V1 ||
    api->structure_bytes<sizeof(*api) || !api->format_id ||
    strcmp(api->format_id,"xfs")!=0 ||
    (api->capabilities & IFS_XFS_CAP_RUN)==0 || !api->run) {
    fputs("XFS.so: invalid or incomplete module\n",stderr); return 1;
 }
 const Request *r=opaque;
 return api->run(r->argc,r->argv,ifs_defrag_services_v1());
}
int main(int argc,char **argv) {
 Request request={argc,argv};
 return ifs_filesystem_module_call("xfs","XFS.so",
    "infiltrator_filesystem_xfs_v1",invoke,&request);
}
