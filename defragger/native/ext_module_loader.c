/* SPDX-License-Identifier: GPL-3.0-or-later
 * All EXT classification and mutation belongs in external EXT2/3/4.so.
 * Even module selection is a read-only probe provided by the engine.
 */
#include "filesystem_module_host.h"
#include "ifs_ext_module_v1.h"
#include "ifs_defrag_host_services.h"
#include <stdio.h>
#include <string.h>
#define EXT_NOT_THIS_FORMAT 253
typedef struct { int argc; char **argv; const char *path; int matched; } Request;
typedef struct { const char *key; const char *filename; } Format;
static const Format formats[]={
    {"ext2","EXT2.so"},{"ext3","EXT3.so"},{"ext4","EXT4.so"}
};
static int invoke(const IfsDefragHostV2 *host,void *symbol,void *opaque) {
    (void)host;
    const IfsExtModuleV1 *(*entry)(void)=NULL;
    _Static_assert(sizeof(entry)==sizeof(symbol),"module pointer width mismatch");
    memcpy(&entry,&symbol,sizeof(entry));
    const IfsExtModuleV1 *api=entry ? entry() : NULL;
    Request *r=opaque;
    if(!api || api->abi_version!=IFS_EXT_MODULE_V1 ||
       api->structure_bytes<sizeof(*api) || !api->format_id ||
       (api->capabilities &
        (IFS_EXT_CAP_RUN | IFS_EXT_CAP_METADATA | IFS_EXT_CAP_FIXTURE | IFS_EXT_CAP_PROBE)) !=
        (IFS_EXT_CAP_RUN | IFS_EXT_CAP_METADATA | IFS_EXT_CAP_FIXTURE | IFS_EXT_CAP_PROBE) ||
       !api->probe || !api->run || !api->metadata || !api->fixture) {
        fputs("EXT.so: incomplete module API\n",stderr); return 1;
    }
    if (r->path && api->probe(r->path,ifs_defrag_services_v1()) != 0)
        return EXT_NOT_THIS_FORMAT;
    r->matched=1;
#ifdef IFS_EXT_METADATA_MODE
    return api->metadata(r->argc,r->argv,ifs_defrag_services_v1());
#else
    return api->run(r->argc,r->argv,ifs_defrag_services_v1());
#endif
}
int main(int argc,char **argv) {
    Request r={argc,argv,argc>=3 ? argv[2] : NULL,0};
    if(!r.path) {
        return ifs_filesystem_module_call("ext4","EXT4.so",
            "infiltrator_filesystem_ext_v1",invoke,&r);
    }
    for(size_t index=0;index<sizeof(formats)/sizeof(formats[0]);++index) {
        r.matched=0;
        int result=ifs_filesystem_module_call(formats[index].key,
            formats[index].filename,"infiltrator_filesystem_ext_v1",invoke,&r);
        if(r.matched)return result;
    }
    fprintf(stderr,"EXT2/3/4.so: no available module recognised the device\n");
    return 1;
}
