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
 Request r={argc,argv};
 return ifs_filesystem_module_call("minix","MINIX.so",
  "infiltrator_filesystem_minix_defrag_v1",execute,&r);
}
