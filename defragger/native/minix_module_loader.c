/* SPDX-License-Identifier: GPL-3.0-or-later
 * Minix filesystem parsing, relocation, formatting and durable recovery
 * are exclusively implemented in independently installed trusted MINIX.so.
 */
#include "filesystem_module_host.h"
#include "ifs_minix_defrag_v1.h"
#include "ifs_defrag_host_services.h"
#include <stdio.h>
#include <string.h>
typedef struct {int argc;char **argv;} Request;
static int execute(const IfsDefragHostV2 *host,void *symbol,void *opaque) {
 (void)host;
 const IfsMinixDefragV1 *(*entry)(void)=NULL;
 _Static_assert(sizeof(entry)==sizeof(symbol),"module pointer size");
 memcpy(&entry,&symbol,sizeof(entry));
 const IfsMinixDefragV1 *api=entry?entry():NULL;
 if(!api || api->abi_version!=IFS_MINIX_DEFRAG_ABI_V1 ||
    api->structure_bytes<sizeof(*api) || !api->format_id ||
    strcmp(api->format_id,"minix") ||
    (api->capabilities&IFS_MINIX_DEFRAG_CAP_RUN)==0 || !api->run) {
  fputs("MINIX.so: missing or incompatible defrag module\n",stderr);return 1;
 }
 const Request *r=opaque;
 return api->run(r->argc,r->argv,ifs_defrag_services_v1());
}
int main(int argc,char **argv) {
 /* Exact Common V2 volume metadata; never fall back to the legacy private
  * MINIX defrag worker when the read-only provider is absent. */
 if(argc>=3 && argv[1] && !strcmp(argv[1],"volume-info"))
     return ifs_filesystem_module_run("minix","MINIX.so",argc,argv);
 /* Explicit per-file Minix mapping is now owned by MINIX.so's verified
  * Common V2 DEFRAG_MAP group. Never call the retired V1 module for this
  * operation, even when the matching V2 capability is unavailable. The
  * legacy whole-volume analyser and transactional writer remain separate
  * until their complete native V2 equivalents are qualified. */
 if(argc>=3 && argv[1] && strcmp(argv[1],"map")==0) {
  for(int i=3;i<argc;++i) {
   if(!argv[i])return 2;
   if(strcmp(argv[i],"--path")==0 ||
      strncmp(argv[i],"--path=",7u)==0)
    return ifs_filesystem_module_run("minix","MINIX.so",argc,argv);
  }
 }
 Request r={argc,argv};
 return ifs_filesystem_module_call("minix","MINIX.so",
  "infiltrator_filesystem_minix_defrag_v1",execute,&r);
}
