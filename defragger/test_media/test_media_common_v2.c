/* SPDX-License-Identifier: GPL-3.0-or-later
 * The Test Media corpus is created and inspected only through real Common V2
 * filesystem groups. A provider that does not advertise FORMAT, CREATE,
 * WRITE or READ does not qualify for this destructive workload. No private
 * formatter, raw metadata parser, historical bridge or ABI fallback exists.
 */
#include "test_media_raw.h"
#include "filesystem_module_host.h"
#include <infiltratr/filesystem_module_v2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef struct {
    const LdtmFilesystemSpec *spec;
    const char *path;
    int create;
    int require_map;
    unsigned fragmented;
    const IfsDefragHostV2 *services;
    IfsFilesystemTargetV2 binding;
} MediaV2;

static const char *provider(const LdtmFilesystemSpec *s)
{
    if(!s || !s->key)return NULL;
    return !strcmp(s->key,"ufs")?"ufs2":s->key;
}
static int is_swap(const LdtmFilesystemSpec *s)
{ return s && s->creator==LDTM_CREATOR_SWAP; }

static const IfsFsModuleApiV2 *qualify(void *symbol,const MediaV2 *m)
{
    IfsFsModuleApiEntryV2 entry=NULL;
    _Static_assert(sizeof(entry)==sizeof(symbol),"Common V2 entry width");
    memcpy(&entry,&symbol,sizeof(entry));
    const IfsFsModuleApiV2 *api=entry?entry():NULL;
    if(!api || !ifs_fs_api_v2_valid(api,provider(m->spec)) ||
       !(api->runtime_profiles & IFS_FS_PROFILE_HOSTED))return NULL;
    const IfsFsCoreOpsV2 *core=
        (const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *volume=
        (const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    if(!core || !(core->header.capabilities & IFS_FS_CORE_PROBE) ||
       !core->probe || (m->create && !volume))return NULL;
    if(m->create && (!(volume->header.capabilities & IFS_FS_VOLUME_FORMAT) ||
                     !volume->format))return NULL;
    if(is_swap(m->spec))return api;
    if(m->require_map) {
        const IfsFsDefragOpsV2 *map=(const IfsFsDefragOpsV2 *)
            ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
        if(!map || !(map->header.capabilities&IFS_FS_DEFRAG_MAP) || !map->map)
            return NULL;
    }
    uint64_t required=IFS_FS_CORE_MOUNT|IFS_FS_CORE_STAT|
                      IFS_FS_CORE_LIST|IFS_FS_CORE_READ;
    if(m->create)required|=IFS_FS_CORE_CREATE|IFS_FS_CORE_WRITE;
    if((core->header.capabilities & required)!=required ||
       !core->mount || !core->unmount || !core->stat ||
       !core->directory_entry || !core->read ||
       (m->create && (!core->create || !core->write)))
        return NULL;
    return api;
}
static int module_call(MediaV2 *m,IfsFilesystemCall callback)
{
    const char *id=provider(m->spec);
    char filename[64]={0};
    if(!id || ifs_filesystem_module_find_readonly_v2(
            id,filename,sizeof(filename))!=0)
        return -1;
    return ifs_filesystem_module_call(id,filename,
                                     IFS_FS_COMMON_EXPORT_V2,callback,m);
}
static int available(const IfsDefragHostV2 *unused,void *symbol,void *arg)
{
    (void)unused;
    return qualify(symbol,arg)?0:-1;
}
int ldtm_common_v2_available(const LdtmFilesystemSpec *s)
{
    MediaV2 m={.spec=s,.create=1};
    return module_call(&m,available)==0;
}
/* Preflight the complete destructive-media workload, including the ability
 * to prove actual allocation fragmentation. ABI presence alone is not enough. */
int ldtm_common_v2_media_available(const LdtmFilesystemSpec *s)
{
    MediaV2 m={.spec=s,.create=1,.require_map=1};
    return module_call(&m,available)==0;
}
static int make_path(unsigned number,char name[32])
{
    char short_name[16];
    ldtm_raw_name(number,short_name);
    int length=snprintf(name,32,"/%s",short_name);
    return length>1 && length<32?0:-1;
}
/* Only provider-owned extent mappings may establish fragmentation. Every
 * canonical large file must be completely mapped, including the terminal
 * END page; a short successful page is not EOF. Extents are ordered by
 * logical byte offset, not by physical position. A gap smaller than one
 * filesystem allocation unit is not sufficient evidence of a displaced
 * allocation (some formats have per-block metadata overhead).
 */
static int file_fragmented(const IfsFsDefragOpsV2 *map,void *session,
    const char *path,uint64_t bytes,uint64_t capacity,uint32_t unit,
    const IfsDefragHostV2 *host,unsigned *fragmented)
{
    uint64_t first=0u,logical=0u,previous_end=0u;
    unsigned displaced=0u;
    for(unsigned page=0u;page<131072u;++page) {
        IfsFsExtentV2 extents[32]={{0}};
        uint32_t count=0u;
        if(host->stop_requested(host->context))return -1;
        int rc=map->map(session,path,first,extents,32u,&count);
        if(rc==IFS_FS_END) {
            if(count || logical!=bytes)return -1;
            *fragmented=displaced;
            return 0;
        }
        if(rc!=IFS_FS_OK || !count || count>32u || first>UINT64_MAX-count)
            return -1;
        for(uint32_t i=0u;i<count;++i) {
            const IfsFsExtentV2 *e=&extents[i];
            if(e->reserved || e->flags || !e->length ||
               e->logical_offset!=logical || logical>bytes ||
               e->length>bytes-logical || e->physical_offset>capacity ||
               e->length>capacity-e->physical_offset)return -1;
            if(logical && (e->physical_offset<previous_end ||
               e->physical_offset-previous_end>=unit))displaced=1u;
            previous_end=e->physical_offset+e->length;
            logical+=e->length;
        }
        first+=count;
    }
    return -1;
}

static int execute(const IfsDefragHostV2 *host,void *symbol,void *opaque)
{
    MediaV2 *m=opaque;
    const IfsFsModuleApiV2 *api=qualify(symbol,m);
    if(!api || !m->path)return -1;
    const IfsFsCoreOpsV2 *core=
        (const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *volume=
        (const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    m->binding.services=host;
    if(host->open_target(host->context,m->path,m->create,NULL,0u,
                         &m->binding.target)!=0)return -1;
    int result=-1,mounted=0;
    void *session=NULL;
    IfsFsHostRuntimeV2 runtime={0};
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    if(m->binding.target.bytes!=(uint64_t)m->spec->size_mib*LDTM_MIB ||
       ifs_filesystem_target_runtime_v2(&m->binding,m->create,0u,&runtime)!=0 ||
       !ifs_fs_host_v2_valid(&runtime))
        goto done;
    if(m->create) {
        if(host->stop_requested(host->context) ||
           runtime.volume_bytes<2u*LDTM_MIB)goto done;
        /* The provider owns all on-disk initialization, including signatures.
         * Common V2 FORMAT variants and label support are provider-defined.
         * These values select the provider's existing public contract; no
         * filesystem data structures are interpreted here. */
        uint32_t variant=0u;
        const char *label=m->spec->label;
        switch(m->spec->creator) {
        case LDTM_CREATOR_EXFAT: variant=64u; label=NULL; break;
        case LDTM_CREATOR_NTFS: variant=1u; break;
        case LDTM_CREATOR_SWAP: variant=4096u; break;
        default: break;
        }
        const int rc=volume->format(&runtime,variant,label,0u);
        if(rc!=IFS_FS_OK) {
            fprintf(stderr,"%s Common V2 FORMAT failed (variant %u, error %d)\n",
                    provider(m->spec),(unsigned)variant,rc);
            goto done;
        }
        if(variant) {
            uint32_t detected=0u;
            if(core->probe(&runtime,&detected)!=IFS_FS_OK || detected!=variant)
                goto done;
        }
    }
    if(is_swap(m->spec)) {
        uint32_t variant=0u;
        result=core->probe(&runtime,&variant)==IFS_FS_OK && variant==4096u?0:-1;
        goto done;
    }
    if(core->mount(&runtime,&session,&info)!=IFS_FS_OK || !session)goto done;
    mounted=1;
    if(info.block_bytes<512u || (info.block_bytes&(info.block_bytes-1u)))
        goto done;
    uint8_t actual[65536],expected[65536];
    char path[32],entry_name[768];
    if(m->create) {
        for(unsigned file=0u;file<LDTM_RAW_FILES;++file) {
            if(make_path(file,path)!=0 ||
               core->create(session,path,IFS_FS_ENTRY_FILE)!=IFS_FS_OK)
                goto done;
        }
        /* Alternate the first and second halves to force interleaved file
         * growth through the provider's own allocation/CoW machinery. */
        for(unsigned phase=0u;phase<2u;++phase)
            for(unsigned file=0u;file<LDTM_RAW_FILES;++file) {
                uint64_t length=ldtm_raw_file_bytes(m->spec,file);
                uint64_t blocks=(length+info.block_bytes-1u)/info.block_bytes;
                uint64_t half=(blocks>1u?blocks/2u:blocks)*info.block_bytes;
                if(half>length)half=length;
                uint64_t at=phase?half:0u,end=phase?length:half;
                if(make_path(file,path)!=0)goto done;
                while(at<end) {
                    uint32_t n=(uint32_t)((end-at)<sizeof(actual)?
                                           end-at:sizeof(actual));
                    if(host->stop_requested(host->context))goto done;
                    ldtm_raw_payload(expected,n,file,at);
                    if(core->write(session,path,at,expected,n)!=IFS_FS_OK)
                        goto done;
                    at+=n;
                }
            }
        if(core->unmount(session)!=IFS_FS_OK)goto done;
        mounted=0;session=NULL;
        if(host->sync_target(host->context,&m->binding.target)!=0)goto done;
        if(core->mount(&runtime,&session,&info)!=IFS_FS_OK)goto done;
        mounted=1;
    }
    unsigned seen=0u;
    int directory_complete=0;
    for(uint32_t index=0u;index<65536u;++index) {
        IfsFsEntryV2 e={.structure_bytes=sizeof(e),
                        .name=entry_name,.name_capacity=sizeof(entry_name)};
        int rc=core->directory_entry(session,"/",index,&e);
        if(rc==IFS_FS_END){directory_complete=1;break;}
        if(rc!=IFS_FS_OK)goto done;
        int number=ldtm_raw_name_index(entry_name);
        if(number<0)continue; /* legitimate provider metadata is not corpus */
        if((seen&(1u<<(unsigned)number)) ||
           e.type!=IFS_FS_ENTRY_FILE ||
           e.size_bytes!=ldtm_raw_file_bytes(m->spec,(unsigned)number))
            goto done;
        seen|=1u<<(unsigned)number;
    }
    if(!directory_complete || seen!=((1u<<LDTM_RAW_FILES)-1u))goto done;
    for(unsigned file=0u;file<LDTM_RAW_FILES;++file) {
        if(make_path(file,path)!=0)goto done;
        IfsFsEntryV2 e={.structure_bytes=sizeof(e),.name=entry_name,
                        .name_capacity=sizeof(entry_name)};
        uint64_t length=ldtm_raw_file_bytes(m->spec,file);
        if(core->stat(session,path,&e)!=IFS_FS_OK ||
           e.type!=IFS_FS_ENTRY_FILE || e.size_bytes!=length)
            goto done;
        if(m->require_map && file<LDTM_TARGET_FILE_COUNT) {
            const IfsFsDefragOpsV2 *map=(const IfsFsDefragOpsV2 *)
                ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
            unsigned displaced=0u;
            if(file_fragmented(map,session,path,length,runtime.volume_bytes,
                info.block_bytes,host,&displaced)!=0) {
                fprintf(stderr,"%s: incomplete Common V2 extent map for %s\n",
                        provider(m->spec),path);
                goto done;
            }
            m->fragmented+=displaced;
        }
        for(uint64_t at=0u;at<length;) {
            uint32_t n=(uint32_t)((length-at)<sizeof(actual)?
                                   length-at:sizeof(actual));
            if(host->stop_requested(host->context) ||
               core->read(session,path,at,actual,n)!=IFS_FS_OK)
                goto done;
            ldtm_raw_payload(expected,n,file,at);
            if(memcmp(expected,actual,n)!=0)goto done;
            at+=n;
        }
    }
    result=0;
done:
    if(mounted && core->unmount(session)!=IFS_FS_OK)result=-1;
    if(m->create && host->sync_target(host->context,&m->binding.target)!=0)
        result=-1;
    host->close_target(host->context,&m->binding.target);
    return result;
}
int ldtm_common_v2_fixture(const LdtmFilesystemSpec *s,
                            const char *path,bool create)
{
    if(!s || !path)return -1;
    MediaV2 m={.spec=s,.path=path,.create=create?1:0};
    return module_call(&m,execute);
}

/* Read-only independent reopen: combine exact corpus byte verification with
 * complete per-file extent proof. This is not a whole-volume display map and
 * does not claim that metadata, directory maps or relocation are qualified. */
int ldtm_common_v2_verify_fragmentation(const LdtmFilesystemSpec *s,
    const char *path,unsigned *fragmented)
{
    if(!s || !path || !fragmented || is_swap(s))return -1;
    *fragmented=0u;
    MediaV2 m={.spec=s,.path=path,.require_map=1};
    const int rc=module_call(&m,execute);
    if(!rc)*fragmented=m.fragmented;
    return rc;
}
