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
/* SFS and SFS2 have independent on-media formats and independent SOs.
 * The respective native CORE.PROBE owns version recognition; the worker
 * must not read signatures or silently select the legacy SFS ABI. */
static int readonly_volume_info(int argc,char **argv)
{
 const char *const formats[]={"sfs","sfs2"};
 char selected_format[16]={0},selected_file[64]={0};
 unsigned matches=0u;
 if(argc<3 || !argv[2])return 2;
 for(unsigned i=0u;i<2u;++i){
  char candidate[64]={0};
  char *probe[]={argv[0],(char *)"probe-only",argv[2],NULL};
  if(ifs_filesystem_module_find_readonly_v2(formats[i],candidate,
                                            sizeof(candidate))!=0)continue;
  if(ifs_filesystem_module_run(formats[i],candidate,3,probe)!=0)continue;
  if(++matches!=1u){
   fputs("Multiple independent SFS Common V2 modules matched target\n",stderr);
   return 1;
  }
  memcpy(selected_format,formats[i],strlen(formats[i])+1u);
  memcpy(selected_file,candidate,strlen(candidate)+1u);
 }
 if(matches!=1u){
  fputs("No independently qualified SFS/SFS2 V2 module recognises target\n",stderr);
  return 1;
 }
 return ifs_filesystem_module_run(selected_format,selected_file,argc,argv);
}
int main(int argc,char **argv) {
 if(argc>=3 && argv[1] && !strcmp(argv[1],"volume-info"))
     return readonly_volume_info(argc,argv);
 Request request={argc,argv};
 return ifs_filesystem_module_call("sfs","SFS.so",
   "infiltrator_filesystem_sfs_v1",execute,&request);
}
