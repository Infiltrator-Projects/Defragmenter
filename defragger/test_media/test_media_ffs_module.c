/* SPDX-License-Identifier: GPL-3.0-or-later
 * FFS Test Media: only a format-neutral trusted SO host.
 * FFS format, allocation, payload and validation live in FFS.so.
 */
#include "test_media.h"
#include "filesystem_module_host.h"
#include "ifs_ffs_fixture_v1.h"
#include "ifs_defrag_host_services.h"
#include <stdio.h>
#include <string.h>
_Static_assert(sizeof(IfsFfsFragmentProfile)==sizeof(LdtmFragmentProfile),
               "FFS test profile ABI size");
typedef enum {AVAIL=0,FORMAT,VALIDATE,POPULATE,VERIFY,VERIFY_AFTER} Operation;
typedef struct {
    Operation op;
    const char *path,*label;
    const LdtmFragmentProfile *profile;
    char *detail;
    size_t detail_capacity;
} Request;
static int invoke(const IfsDefragHostV2 *host,void *symbol,void *opaque) {
    (void)host;
    const IfsFfsFixtureV1 *(*entry)(void)=NULL;
    _Static_assert(sizeof(entry)==sizeof(symbol),"FFS ABI symbol width");
    memcpy(&entry,&symbol,sizeof(entry));
    const IfsFfsFixtureV1 *api=entry ? entry() : NULL;
    if(!api || api->abi_version!=IFS_FFS_FIXTURE_ABI_V1 ||
       api->structure_bytes<sizeof(*api) || !api->format_id ||
       strcmp(api->format_id,"ffs")!=0 ||
       (api->capabilities & 7U)!=7U ||
       !api->format || !api->validate || !api->populate ||
       !api->verify || !api->verify_after) {
        fputs("FFS.so is missing required native Test Media contract\n",stderr);
        return 1;
    }
    const Request *request=opaque;
    if(!request || request->op==AVAIL)return 0;
    const IfsDefragServicesV1 *services=ifs_defrag_services_v1();
    if(request->op==FORMAT)
        return api->format(request->path,request->label,services);
    if(request->op==VALIDATE)
        return api->validate(request->path,services);
    if(!request->profile)return 1;
    IfsFfsFragmentProfile profile;
    memcpy(&profile,request->profile,sizeof(profile));
    if(request->op==POPULATE)
        return api->populate(request->path,&profile,services);
    if(request->op==VERIFY)
        return api->verify(request->path,&profile,request->detail,request->detail_capacity,services);
    return api->verify_after(request->path,&profile,request->detail,request->detail_capacity,services);
}
static int call(Request *r) {
    return ifs_filesystem_module_call("ffs","FFS.so",
        "infiltrator_filesystem_ffs_fixture_v1",invoke,r);
}
int ldtm_ffs_module_available(void){Request r={.op=AVAIL};return call(&r)==0;}
int ldtm_format_ffs_module(const char *path,const char *label){
    Request r={.op=FORMAT,.path=path,.label=label};return call(&r)==0?0:-1;
}
int ldtm_validate_ffs_module(const char *path){
    Request r={.op=VALIDATE,.path=path};return call(&r)==0?0:-1;
}
int ldtm_populate_ffs_module(const char *path,const LdtmFragmentProfile *profile){
    Request r={.op=POPULATE,.path=path,.profile=profile};return call(&r)==0?0:-1;
}
int ldtm_verify_ffs_module(const char *path,const LdtmFragmentProfile *profile,
                          int after,char *detail,size_t detail_capacity) {
    Request r={.op=after?VERIFY_AFTER:VERIFY,.path=path,.profile=profile,
               .detail=detail,.detail_capacity=detail_capacity};
    return call(&r)==0?0:-1;
}
