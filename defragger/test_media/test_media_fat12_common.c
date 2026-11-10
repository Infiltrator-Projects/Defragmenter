/* SPDX-License-Identifier: GPL-3.0-or-later
 * FAT12 Test Media uses only the canonical Common filesystem ABI.
 * No historical V1/V3/V4 FAT12 selectors, fallback or FAT parser.
 * Every data operation delegates to the installed FAT12.so module.
 */
#include "test_media_raw.h"
#include "filesystem_module_host.h"
#include "infiltratr/filesystem_module_v2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const LdtmFilesystemSpec *spec;
    const char *path;
    const IfsDefragHostV2 *host;
    IfsDefragTarget target;
} Fat12Fixture;

static int read_at(void *user,uint64_t at,void *out,uint32_t bytes)
{
    Fat12Fixture *f=user;
    return f->host->read_at(f->host->context,&f->target,at,out,bytes);
}
static int write_at(void *user,uint64_t at,const void *data,uint32_t bytes)
{
    Fat12Fixture *f=user;
    return f->host->write_at(f->host->context,&f->target,at,data,bytes);
}
static int flush_at(void *user)
{
    Fat12Fixture *f=user;
    return f->host->sync_target(f->host->context,&f->target);
}
static void *allocate_at(void *ctx,size_t n) {(void)ctx;return malloc(n);}
static void release_at(void *ctx,void *p) {(void)ctx;free(p);}
static IfsFsHostRuntimeV2 make_host(Fat12Fixture *f,int writable)
{
    IfsFsHostRuntimeV2 io={0};
    io.abi_version=IFS_FS_HOST_ABI_V2;
    io.structure_bytes=sizeof(io);
    io.capabilities=writable ? IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE :
                               IFS_FS_HOST_READ_ONLY;
    io.context=f;
    io.volume_bytes=f->target.bytes;
    io.logical_sector_bytes=512u;
    io.physical_sector_bytes=512u;
    io.maximum_transfer_bytes=65536u;
    io.read_at=read_at;
    io.write_at=writable?write_at:NULL;
    io.flush=writable?flush_at:NULL;
    io.allocate=allocate_at;
    io.release=release_at;
    return io;
}
static const IfsFsModuleApiV2 *canonical(void *symbol)
{
    IfsFsModuleApiEntryV2 entry=NULL;
    _Static_assert(sizeof(entry)==sizeof(symbol),"dynamic symbol width");
    memcpy(&entry,&symbol,sizeof(entry));
    const IfsFsModuleApiV2 *api=entry?entry():NULL;
    if(!ifs_fs_api_v2_valid(api,"fat12"))return NULL;
    const IfsFsCoreOpsV2 *core=
        (const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *volume=
        (const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    const uint64_t required=IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|
        IFS_FS_CORE_LIST|IFS_FS_CORE_STAT|IFS_FS_CORE_READ|
        IFS_FS_CORE_WRITE|IFS_FS_CORE_CREATE;
    if(!core || (core->header.capabilities & required)!=required ||
       !core->mount || !core->unmount || !core->create ||
       !core->write || !core->read || !core->stat || !core->directory_entry ||
       !volume || !(volume->header.capabilities & IFS_FS_VOLUME_FORMAT) ||
       !(volume->header.capabilities & IFS_FS_VOLUME_CHECK) ||
       !volume->format || !volume->check)
        return NULL;
    return api;
}
static int call(Fat12Fixture *f,IfsFilesystemCall action)
{
    return ifs_filesystem_module_call("fat12","FAT12.so",
        IFS_FS_COMMON_EXPORT_V2,action,f);
}
static int available(const IfsDefragHostV2 *host,void *symbol,void *opaque)
{
    (void)host;(void)opaque;
    return canonical(symbol)?0:-1;
}
int ldtm_fat12_common_available(const LdtmFilesystemSpec *spec)
{
    if(!spec || spec->creator!=LDTM_CREATOR_FAT12)return -1;
    Fat12Fixture f={.spec=spec};
    return call(&f,available);
}
static int make_name(unsigned file,char path[32])
{
    char name[16];
    ldtm_raw_name(file,name);
    int n=snprintf(path,32,"/%s",name);
    return n>1 && n<32?0:-1;
}
static int create(const IfsDefragHostV2 *host,void *symbol,void *opaque)
{
    Fat12Fixture *f=opaque;
    const IfsFsModuleApiV2 *api=canonical(symbol);
    if(!api)return -1;
    const IfsFsCoreOpsV2 *core=(const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *volume=(const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    f->host=host;
    if(host->open_target(host->context,f->path,1,NULL,0,&f->target)!=0)return -1;
    int result=-1,mounted=0;
    void *session=NULL;
    if(f->target.bytes!=(uint64_t)f->spec->size_mib*LDTM_MIB ||
       f->target.bytes<2u*LDTM_MIB ||
       host->stop_requested(host->context))goto done;
    /* Preserve the existing Test Media signature-neutralisation step.
     * This touches only the exclusive, prequalified sacrificial target;
     * no filesystem records are invented by the host. */
    uint8_t zeroes[65536]={0};
    for(uint64_t at=0;at<LDTM_MIB;at+=sizeof zeroes) {
        if(host->stop_requested(host->context) ||
           write_at(f,at,zeroes,sizeof zeroes)!=0 ||
           write_at(f,f->target.bytes-LDTM_MIB+at,zeroes,sizeof zeroes)!=0)
            goto done;
    }

    IfsFsHostRuntimeV2 io=make_host(f,1);
    if(!ifs_fs_host_v2_valid(&io) ||
       volume->format(&io,12u,NULL,0u)!=IFS_FS_OK)
        goto done;
    IfsFsVolumeInfoV2 info={0};
    info.structure_bytes=sizeof(info);
    if(core->mount(&io,&session,&info)!=IFS_FS_OK)goto done;
    mounted=1;
    if(info.variant!=12u || info.block_bytes!=65536u)goto done;

    char path[32];
    uint8_t buffer[65536];
    result=0;
    for(unsigned file=0;file<LDTM_RAW_FILES && !result;++file) {
        if(make_name(file,path)!=0 ||
           core->create(session,path,IFS_FS_ENTRY_FILE)!=IFS_FS_OK)
            result=-1;
    }
    /* Interleave half-file allocations to deliberately fragment the corpus. */
    for(unsigned phase=0;phase<2u && !result;++phase) {
        for(unsigned file=0;file<LDTM_RAW_FILES && !result;++file) {
            uint64_t bytes=ldtm_raw_file_bytes(f->spec,file);
            if(bytes>UINT32_MAX) {result=-1;break;}
            uint64_t clusters=(bytes+65535u)/65536u;
            uint64_t split=(clusters>1u?clusters/2u:clusters)*65536u;
            if(split>bytes)split=bytes;
            uint64_t at=phase==0u?0u:split;
            const uint64_t end=phase==0u?split:bytes;
            if(make_name(file,path)!=0) {result=-1;break;}
            while(!result && at<end) {
                const uint32_t n=(uint32_t)((end-at)<sizeof buffer?(end-at):sizeof buffer);
                if(host->stop_requested(host->context)) {result=-1;break;}
                ldtm_raw_payload(buffer,n,file,at);
                if(core->write(session,path,at,buffer,n)!=IFS_FS_OK)result=-1;
                at+=n;
            }
        }
    }
done:
    if(mounted && core->unmount(session)!=IFS_FS_OK)result=-1;
    if(host->sync_target(host->context,&f->target)!=0)result=-1;
    host->close_target(host->context,&f->target);
    return result;
}
int ldtm_fat12_common_create(const LdtmFilesystemSpec *spec,const char *path)
{
    if(!spec || spec->creator!=LDTM_CREATOR_FAT12 || !path)return -1;
    Fat12Fixture f={.spec=spec,.path=path};
    return call(&f,create);
}
static int verify(const IfsDefragHostV2 *host,void *symbol,void *opaque)
{
    Fat12Fixture *f=opaque;
    const IfsFsModuleApiV2 *api=canonical(symbol);
    if(!api)return -1;
    const IfsFsCoreOpsV2 *core=(const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *volume=(const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    f->host=host;
    if(host->open_target(host->context,f->path,0,NULL,0,&f->target)!=0)return -1;
    int result=-1,mounted=0;
    void *session=NULL;
    IfsFsHostRuntimeV2 io=make_host(f,0);
    IfsFsVolumeInfoV2 info={0};
    info.structure_bytes=sizeof(info);
    if(f->target.bytes!=(uint64_t)f->spec->size_mib*LDTM_MIB ||
       !ifs_fs_host_v2_valid(&io) ||
       core->mount(&io,&session,&info)!=IFS_FS_OK)goto done;
    mounted=1;
    if(info.variant!=12u || info.block_bytes!=65536u)goto done;
    uint8_t actual[65536],expected[65536];
    bool seen[LDTM_RAW_FILES]={false};
    for(uint32_t index=0;index<=LDTM_RAW_FILES;++index) {
        char name[768]={0};
        IfsFsEntryV2 entry={0};
        entry.structure_bytes=sizeof entry;
        entry.name=name;
        entry.name_capacity=sizeof name;
        int status=core->directory_entry(session,"/",index,&entry);
        if(index==LDTM_RAW_FILES) {
            if(status!=IFS_FS_END)goto done;
            break;
        }
        if(status!=IFS_FS_OK)goto done;
        int file=ldtm_raw_name_index(entry.name);
        if(file<0 || (unsigned)file>=LDTM_RAW_FILES || seen[file] ||
           entry.type!=IFS_FS_ENTRY_FILE ||
           entry.size_bytes!=ldtm_raw_file_bytes(f->spec,(unsigned)file))
            goto done;
        seen[file]=true;
    }
    for(unsigned file=0;file<LDTM_RAW_FILES;++file) {
        if(!seen[file])goto done;
        char path[32],name[768]={0};
        IfsFsEntryV2 entry={0};
        entry.structure_bytes=sizeof entry;
        entry.name=name;
        entry.name_capacity=sizeof name;
        const uint64_t length=ldtm_raw_file_bytes(f->spec,file);
        if(make_name(file,path)!=0 ||
           core->stat(session,path,&entry)!=IFS_FS_OK ||
           entry.size_bytes!=length || entry.type!=IFS_FS_ENTRY_FILE)
            goto done;
        for(uint64_t offset=0;offset<length;) {
            const uint32_t n=(uint32_t)((length-offset)<sizeof actual?
                (length-offset):sizeof actual);
            if(host->stop_requested(host->context) ||
               core->read(session,path,offset,actual,n)!=IFS_FS_OK)goto done;
            ldtm_raw_payload(expected,n,file,offset);
            if(memcmp(actual,expected,n)!=0)goto done;
            offset+=n;
        }
    }
    uint64_t errors=UINT64_MAX;
    if(volume->check(session,0u,&errors)!=IFS_FS_OK || errors!=0u)goto done;
    result=0;
done:
    if(mounted && core->unmount(session)!=IFS_FS_OK)result=-1;
    host->close_target(host->context,&f->target);
    return result;
}
int ldtm_fat12_common_verify(const LdtmFilesystemSpec *spec,const char *path)
{
    if(!spec || spec->creator!=LDTM_CREATOR_FAT12 || !path)return -1;
    Fat12Fixture f={.spec=spec,.path=path};
    return call(&f,verify);
}
