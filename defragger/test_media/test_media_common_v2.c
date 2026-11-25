/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native formatting and full file-corpus qualification are separate Common
 * V2 contracts. A formatter-only provider may create and identify media,
 * but is never credited with file creation, writing or defragmentation.
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
    int format_only;
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
       !core->probe)return NULL;
    if(m->create && (!volume || !(volume->header.capabilities & IFS_FS_VOLUME_FORMAT) ||
                     !volume->format))return NULL;
    if(m->format_only || is_swap(m->spec))return api;
    if(!volume)return NULL;
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
int ldtm_common_v2_format_available(const LdtmFilesystemSpec *s)
{
    MediaV2 m={.spec=s,.create=1,.format_only=1};
    return module_call(&m,available)==0;
}
/* Provider-defined format parameters, not filesystem implementations.
 * A zero variant is meaningful (Amiga DOS0), not a generic default. */
static int format_parameters(const LdtmFilesystemSpec *s,uint32_t *variant,
                             const char **label)
{
    *label=s->label;
    switch(s->creator) {
    case LDTM_CREATOR_FAT12: *variant=12u; *label=NULL; break;
    case LDTM_CREATOR_FAT16: *variant=16u; *label=NULL; break;
    case LDTM_CREATOR_FAT32: *variant=32u; *label=NULL; break;
    case LDTM_CREATOR_EXFAT: *variant=64u; *label=NULL; break;
    case LDTM_CREATOR_NTFS: *variant=1u; break;
    case LDTM_CREATOR_EXT2: *variant=2u; break;
    case LDTM_CREATOR_EXT3: *variant=3u; break;
    case LDTM_CREATOR_EXT4: *variant=4u; break;
    case LDTM_CREATOR_XFS: *variant=5u; break;
    case LDTM_CREATOR_BTRFS: *variant=1u; break;
    case LDTM_CREATOR_AFFS:
        if(!strcmp(s->key,"ofs"))*variant=0u;
        else if(!strcmp(s->key,"ffs"))*variant=1u;
        else if(!strcmp(s->key,"sfs"))*variant=3u;
        else if(!strcmp(s->key,"sfs2"))*variant=4096u;
        else return -1;
        break;
    case LDTM_CREATOR_PFS3: *variant=1u; break;
    case LDTM_CREATOR_HFS: *variant=1u; break;
    case LDTM_CREATOR_HFSPLUS: *variant=4u; break;
    case LDTM_CREATOR_MINIX: *variant=3u; *label=NULL; break;
    case LDTM_CREATOR_UFS:
        if(!strcmp(s->key,"ufs1"))*variant=1u;
        else if(!strcmp(s->key,"ufs2"))*variant=3u;
        else return -1;
        break;
    case LDTM_CREATOR_ZFS: *variant=1u; break;
    case LDTM_CREATOR_APFS: *variant=1u; break;
    case LDTM_CREATOR_SWAP: *variant=4096u; break;
    default: return -1;
    }
    return 0;
}
static int make_path(unsigned number,char name[32])
{
    char short_name[16];
    ldtm_raw_name(number,short_name);
    int length=snprintf(name,32,"/%s",short_name);
    return length>1 && length<32?0:-1;
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
        /* This is a prequalified, exclusively leased Test Media image.
         * Neutralise stale signatures only AFTER the Common V2 provider has
         * authenticated and advertised its actual format capability. */
        uint8_t empty[65536]={0};
        for(uint64_t at=0u;at<LDTM_MIB;at+=sizeof(empty)) {
            if(host->stop_requested(host->context) ||
               runtime.write_at(runtime.context,at,empty,sizeof(empty))!=0 ||
               runtime.write_at(runtime.context,
                   runtime.volume_bytes-LDTM_MIB+at,empty,sizeof(empty))!=0)
                goto done;
        }
        uint32_t variant=0u;
        const char *label=NULL;
        if(format_parameters(m->spec,&variant,&label)!=0)goto done;
        const int rc=volume->format(&runtime,variant,label,0u);
        if(rc!=IFS_FS_OK) {
            fprintf(stderr,"%s Common V2 FORMAT failed (variant %u, error %d)\n",
                    provider(m->spec),(unsigned)variant,rc);
            goto done;
        }
        if(runtime.flush(runtime.context)!=0)goto done;
        uint32_t detected=0u;
        if(core->probe(&runtime,&detected)!=IFS_FS_OK || detected!=variant) {
            fprintf(stderr,"%s Common V2 post-format probe mismatch (%u != %u)\n",
                    provider(m->spec),(unsigned)detected,(unsigned)variant);
            goto done;
        }
    }
    if(m->format_only || is_swap(m->spec)) {
        uint32_t variant=0u,expected=0u;
        const char *label=NULL;
        if(format_parameters(m->spec,&expected,&label)==0 &&
           core->probe(&runtime,&variant)==IFS_FS_OK && variant==expected)result=0;
        goto done;
    }
    if(core->mount(&runtime,&session,&info)!=IFS_FS_OK)goto done;
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

int ldtm_common_v2_format_only(const LdtmFilesystemSpec *s,const char *path,bool create)
{
    if(!s || !path)return -1;
    MediaV2 m={.spec=s,.path=path,.create=create?1:0,.format_only=1};
    return module_call(&m,execute);
}
