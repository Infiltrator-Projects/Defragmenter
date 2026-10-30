/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SFS geometry, allocation, relocation, journaling and recovery are
 * exclusively implemented by the trusted SFS.so module. */
#include "filesystem_module_host.h"
#include "ifs_sfs_module_v1.h"
#include "ifs_defrag_host_services.h"
#include <stdio.h>
#include <string.h>
typedef struct {int argc;char **argv;} Request;
static const IfsSfsModuleV1 *inspect(void *symbol) {
 const IfsSfsModuleV1 *(*entry)(void)=NULL;
 _Static_assert(sizeof(entry)==sizeof(symbol),"function pointer ABI");
 memcpy(&entry,&symbol,sizeof(entry));
 const IfsSfsModuleV1 *api=entry ? entry() : NULL;
 return api && api->abi_version==IFS_SFS_MODULE_V1 &&
  api->structure_bytes>=sizeof(*api) && api->format_id &&
  strcmp(api->format_id,"sfs")==0 &&
  (api->capabilities & (IFS_SFS_CAP_RUN | IFS_SFS_CAP_FIXTURE)) ==
    (IFS_SFS_CAP_RUN | IFS_SFS_CAP_FIXTURE) && api->run && api->fixture
  ? api : NULL;
}
static int execute(const IfsDefragHostV2 *host,void *symbol,void *opaque) {
 (void)host;
 const IfsSfsModuleV1 *api=inspect(symbol);
 if(!api) { fputs("SFS.so: unsupported or incomplete module ABI\n",stderr); return 1; }
 const Request *request=opaque;
 return api->run(request->argc,request->argv,ifs_defrag_services_v1());
}
int main(int argc,char **argv) {
 Request request={argc,argv};
 return ifs_filesystem_module_call("sfs","SFS.so",
   "infiltrator_filesystem_sfs_v1",execute,&request);
}
