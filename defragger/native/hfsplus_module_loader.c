/* SPDX-License-Identifier: GPL-3.0-or-later
 * Trusted generic host dispatch: HFS+ layouts and recovery exist in HFSPLUS.so.
 */
#include "filesystem_module_host.h"
#include "ifs_hfsplus_module_v1.h"
#include "ifs_defrag_host_services.h"
#include <stdio.h>
#include <string.h>
typedef struct {int argc; char **argv;} Request;
static int execute(const IfsDefragHostV2 *host, void *symbol, void *opaque) {
    (void)host;
    const IfsHfsplusModuleV1 *(*entry)(void)=NULL;
    _Static_assert(sizeof(entry)==sizeof(symbol),"function pointer width");
    memcpy(&entry,&symbol,sizeof(entry));
    const IfsHfsplusModuleV1 *api=entry ? entry() : NULL;
    if (!api || api->abi_version!=IFS_HFSPLUS_MODULE_V1 ||
        api->structure_bytes<sizeof(*api) || !api->format_id ||
        strcmp(api->format_id,"hfsplus") != 0 ||
        (api->capabilities & IFS_HFSPLUS_CAP_RUN) == 0 || !api->run) {
        fputs("HFSPLUS.so: incomplete or incompatible filesystem module\n",stderr);
        return 1;
    }
    const Request *request=opaque;
    return api->run(request->argc,request->argv,ifs_defrag_services_v1());
}
int main(int argc,char **argv) {
    Request request={argc,argv};
    return ifs_filesystem_module_call("hfsplus","HFSPLUS.so",
        "infiltrator_filesystem_hfsplus_v1",execute,&request);
}
