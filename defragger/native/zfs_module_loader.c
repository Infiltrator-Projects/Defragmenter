/* SPDX-License-Identifier: GPL-3.0-or-later
 * Generic trusted ZFS.so loader. The module owns the native analysis and
 * exact-read algorithms; there is no internal fallback or system mkfs.
 */
#include "filesystem_module_host.h"
#include "ifs_zfs_module_v1.h"
#include <stdio.h>
#include <string.h>
typedef struct { int argc; char **argv; } Request;
static int call_module(const IfsDefragHostV2 *host, void *symbol, void *opaque) {
    (void)host;
    const IfsZfsModuleV1 *(*entry)(void) = NULL;
    _Static_assert(sizeof(entry)==sizeof(symbol), "module function pointer mismatch");
    memcpy(&entry,&symbol,sizeof(entry));
    const IfsZfsModuleV1 *api=entry ? entry() : NULL;
    if (!api || api->abi_version != IFS_ZFS_MODULE_V1 ||
        api->structure_bytes < sizeof(*api) || !api->format_id ||
        strcmp(api->format_id,"zfs") != 0 ||
        (api->capabilities & IFS_ZFS_CAP_RUN) == 0 || !api->run) {
        fputs("ZFS.so: unrecognised ZFS worker API\n",stderr);
        return 1;
    }
    const Request *r=opaque;
    return api->run(r->argc,r->argv);
}
int main(int argc,char **argv) {
    Request request={argc,argv};
    return ifs_filesystem_module_call("zfs","ZFS.so",
       "infiltrator_filesystem_zfs_v1",call_module,&request);
}
