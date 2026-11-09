/* SPDX-License-Identifier: GPL-3.0-or-later
 * FAT12 Test Media uses only the canonical Common filesystem ABI.
 * No historical V1/V3/V4 FAT12 selectors, fallback or FAT parser.
 * Every data operation delegates to the installed FAT12.so module.
 */
#include "test_media_raw.h"
#include "filesystem_module_host.h"
#include "infiltratr/filesystem_module.h"
#include <stdio.h>
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
static const IfsFsModuleApiV1 *canonical(void *symbol)
{
    IfsFsModuleApiEntryV1 entry=NULL;
    _Static_assert(sizeof(entry)==sizeof(symbol),"dynamic symbol width");
    memcpy(&entry,&symbol,sizeof entry);
    const IfsFsModuleApiV1 *api=entry?entry():NULL;
    const uint64_t required=IFS_FS_CAP_PROBE|IFS_FS_CAP_MOUNT|
        IFS_FS_CAP_LIST|IFS_FS_CAP_READ|IFS_FS_CAP_WRITE|
        IFS_FS_CAP_CREATE|IFS_FS_CAP_FORMAT|IFS_FS_CAP_CHECK;
    if(!ifs_fs_common_api_valid(api,"fat12") ||
       (api->capabilities & required)!=required ||
       !api->format || !api->probe || !api->mount || !api->unmount ||
       !api->create || !api->write || !api->read || !api->stat ||
       !api->directory_entry || !api->check)
        return NULL;
    return api;
}
static int call(Fat12Fixture *f,IfsFilesystemCall action)
{
    return ifs_filesystem_module_call("fat12","FAT12.so",
        IFS_FS_COMMON_EXPORT_V1,action,f);
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
    const IfsFsModuleApiV1 *api=canonical(symbol);
    if(!api)return -1;
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

    IfsFsHostFormatIoV1 io={0};
    io.host.structure_bytes=sizeof io;
    io.host.flags=IFS_FS_IO_EXCLUSIVE;
    io.host.context=f;
    io.host.volume_bytes=f->target.bytes;
    io.host.read_at=read_at;
    io.host.write_at=write_at;
    io.host.flush=flush_at;
    io.logical_sector_bytes=512u;
    /* The sole FAT12 engine owns allocation-unit selection and format.
     * No host-minted FAT tables or host-coded boot sectors exist here. */
    if(api->format(&io.host,12u,NULL)!=IFS_FS_OK)goto done;
    IfsFsVolumeInfoV1 info={0};
    info.structure_bytes=sizeof info;
    if(api->mount(&io.host,&session,&info)!=IFS_FS_OK)goto done;
    mounted=1;
    if(info.variant!=12u || info.block_bytes!=65536u)goto done;

    char path[32];
    uint8_t buffer[65536];
    result=0;
    for(unsigned file=0;file<LDTM_RAW_FILES && !result;++file) {
        if(make_name(file,path)!=0 ||
           api->create(session,path,IFS_FS_ENTRY_FILE)!=IFS_FS_OK)
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
                if(api->write(session,path,at,buffer,n)!=IFS_FS_OK)result=-1;
                at+=n;
            }
        }
    }
done:
    if(mounted && api->unmount(session)!=IFS_FS_OK)result=-1;
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
    const IfsFsModuleApiV1 *api=canonical(symbol);
    if(!api)return -1;
    f->host=host;
    if(host->open_target(host->context,f->path,0,NULL,0,&f->target)!=0)return -1;
    int result=-1,mounted=0;
    void *session=NULL;
    IfsFsHostIoV1 io={0};
    io.structure_bytes=sizeof io;
    io.flags=IFS_FS_IO_READ_ONLY;
    io.context=f;
    io.volume_bytes=f->target.bytes;
    io.read_at=read_at;
    IfsFsVolumeInfoV1 info={0};
    info.structure_bytes=sizeof info;
    if(f->target.bytes!=(uint64_t)f->spec->size_mib*LDTM_MIB ||
       api->mount(&io,&session,&info)!=IFS_FS_OK)goto done;
    mounted=1;
    if(info.variant!=12u || info.block_bytes!=65536u)goto done;
    uint8_t actual[65536],expected[65536];
    bool seen[LDTM_RAW_FILES]={false};
    for(uint32_t index=0;index<=LDTM_RAW_FILES;++index) {
        char name[768]={0};
        IfsFsEntryV1 entry={0};
        entry.structure_bytes=sizeof entry;
        entry.name=name;
        entry.name_capacity=sizeof name;
        int status=api->directory_entry(session,"/",index,&entry);
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
        IfsFsEntryV1 entry={0};
        entry.structure_bytes=sizeof entry;
        entry.name=name;
        entry.name_capacity=sizeof name;
        const uint64_t length=ldtm_raw_file_bytes(f->spec,file);
        if(make_name(file,path)!=0 ||
           api->stat(session,path,&entry)!=IFS_FS_OK ||
           entry.size_bytes!=length || entry.type!=IFS_FS_ENTRY_FILE)
            goto done;
        for(uint64_t offset=0;offset<length;) {
            const uint32_t n=(uint32_t)((length-offset)<sizeof actual?
                (length-offset):sizeof actual);
            if(host->stop_requested(host->context) ||
               api->read(session,path,offset,actual,n)!=IFS_FS_OK)goto done;
            ldtm_raw_payload(expected,n,file,offset);
            if(memcmp(actual,expected,n)!=0)goto done;
            offset+=n;
        }
    }
    uint32_t errors=UINT32_MAX;
    if(api->check(session,&errors)!=IFS_FS_OK || errors!=0u)goto done;
    result=0;
done:
    if(mounted && api->unmount(session)!=IFS_FS_OK)result=-1;
    host->close_target(host->context,&f->target);
    return result;
}
int ldtm_fat12_common_verify(const LdtmFilesystemSpec *spec,const char *path)
{
    if(!spec || spec->creator!=LDTM_CREATOR_FAT12 || !path)return -1;
    Fat12Fixture f={.spec=spec,.path=path};
    return call(&f,verify);
}
