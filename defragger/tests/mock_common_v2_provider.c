/* SPDX-License-Identifier: GPL-3.0-or-later
 * Test-only miniature storage provider. Never installed or shipped.
 * Exercises the complete Common V2 loader and exclusive host callbacks.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <string.h>
#include <stdint.h>
typedef struct { const IfsFsHostRuntimeV2 *host; } MockVolume;
typedef struct { uint32_t marker; } MockPlan;
static int probe(const IfsFsHostRuntimeV2 *h,uint32_t *variant) {
    char magic[8];
    if(!ifs_fs_host_v2_valid(h) || !variant)return IFS_FS_INVALID;
    *variant=0u;
    if(h->volume_bytes<96u || h->read_at(h->context,0u,magic,8u))
        return IFS_FS_IO_ERROR;
    if(memcmp(magic,"TESTV2FS",8u))return IFS_FS_NOT_FOUND;
    *variant=1u;return IFS_FS_OK;
}
static int mount(const IfsFsHostRuntimeV2 *h,void **out,IfsFsVolumeInfoV2 *info) {
    uint32_t variant=0u;
    if(!out||!info||info->structure_bytes<sizeof(*info) ||
       probe(h,&variant)!=IFS_FS_OK)return IFS_FS_INVALID;
    MockVolume *v=h->allocate(h->context,sizeof(*v));
    if(!v)return IFS_FS_NO_SPACE;
    v->host=h;*out=v;
    info->variant=1u;info->block_bytes=h->logical_sector_bytes;
    info->total_bytes=h->volume_bytes;info->free_bytes=0u;
    info->max_name_bytes=8u;
    return IFS_FS_OK;
}
static int unmount(void *m) {
    if(!m)return IFS_FS_INVALID;
    MockVolume *v=m;
    v->host->release(v->host->context,v);
    return IFS_FS_OK;
}
static int map(void *m,const char *path,uint64_t first,IfsFsExtentV2 *out,
               uint32_t cap,uint32_t *written) {
    if(!m || !path || strcmp(path,"/FILE") || first || !out || !cap || !written)
        return IFS_FS_INVALID;
    out[0]=(IfsFsExtentV2){0u,64u,4u,0u,0u};
    *written=1u;
    return IFS_FS_OK;
}
static int plan(void *m,const char *path,uint32_t policy,void **out) {
    if(!m || !path || strcmp(path,"/FILE") || (policy!=0u && policy!=10u) ||
       !out)return IFS_FS_INVALID;
    MockVolume *v=m;
    MockPlan *p=v->host->allocate(v->host->context,sizeof(*p));
    if(!p)return IFS_FS_NO_SPACE;
    p->marker=0x32465349u;
    *out=p;
    return IFS_FS_OK;
}
static int execute(void *m,void *plan,uint64_t *moved) {
    MockVolume *v=m;
    MockPlan *p=plan;
    if(!v||!p||p->marker!=0x32465349u||!moved ||
       !v->host->write_at)return IFS_FS_INVALID;
    if(v->host->write_at(v->host->context,64u,"MOVE",4u))
        return IFS_FS_IO_ERROR;
    *moved=4u;
    return IFS_FS_OK;
}
static int verify(void *m,void *plan) {
    MockVolume *v=m;MockPlan *p=plan;char got[4];
    if(!v||!p||p->marker!=0x32465349u)return IFS_FS_INVALID;
    return v->host->read_at(v->host->context,64u,got,4u) ||
        memcmp(got,"MOVE",4u) ? IFS_FS_IO_ERROR:IFS_FS_OK;
}
static int recover(void *m,uint32_t flags,uint64_t *repaired) {
    MockVolume *v=m;
    if(!v||!repaired||flags||!v->host->write_at)return IFS_FS_INVALID;
    if(v->host->write_at(v->host->context,80u,"REST",4u))
        return IFS_FS_IO_ERROR;
    *repaired=1u;
    return IFS_FS_OK;
}
static void release_plan(void *m,void *plan) {
    MockVolume *v=m;
    if(v && plan)v->host->release(v->host->context,plan);
}
static const IfsFsCoreOpsV2 core={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),
             IFS_FS_GROUP_CORE,0u,IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT},
    .probe=probe,.mount=mount,.unmount=unmount
};
static const IfsFsDefragOpsV2 defrag={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsDefragOpsV2),
             IFS_FS_GROUP_DEFRAG,0u,IFS_FS_DEFRAG_ALL},
    .map=map,.plan=plan,.execute=execute,.verify=verify,
    .recover=recover,.release_plan=release_plan
};
static const IfsFsGroupHeaderV2 *query(uint32_t id,uint32_t min,uint32_t max) {
    if(min>IFS_FS_GROUP_ABI_V2 || max<IFS_FS_GROUP_ABI_V2)return NULL;
    if(id==IFS_FS_GROUP_CORE)return &core.header;
    if(id==IFS_FS_GROUP_DEFRAG)return &defrag.header;
    return NULL;
}
static const IfsFsModuleApiV2 api={
    .abi_version=IFS_FS_COMMON_ABI_V2,
    .structure_bytes=sizeof(IfsFsModuleApiV2),
    .format_id="testv2",
    .runtime_profiles=IFS_FS_PROFILE_HOSTED,
    .group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|
                IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG),
    .query_group=query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void) {
    return &api;
}
