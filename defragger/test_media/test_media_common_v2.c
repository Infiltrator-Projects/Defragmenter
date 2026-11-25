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
    int bfs_layout; /* 1: compact, 2: fragmented free space, 3: growth */
    unsigned fragmented;
    char *detail;
    size_t detail_capacity;
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
static int is_bfs(const LdtmFilesystemSpec *s)
{ return s && !strcmp(s->key,"bfs"); }

/* These values are test-profile parameters, not filesystem algorithms.
 * Format, allocation, extent decoding and integrity checking are performed
 * only by the selected Common V2 provider. */
static uint32_t profile_fat_width(const LdtmFilesystemSpec *s)
{
    if(s->creator==LDTM_CREATOR_FAT12)return 12u;
    if(s->creator==LDTM_CREATOR_FAT16)return 16u;
    if(s->creator==LDTM_CREATOR_FAT32)return 32u;
    return 0u;
}
static int profile_geometry_valid(const LdtmFilesystemSpec *s,
                                  const IfsFsVolumeInfoV2 *info)
{
    uint32_t width=profile_fat_width(s);
    return info->block_bytes>=512u &&
           !(info->block_bytes&(info->block_bytes-1u)) &&
           (!width || info->variant==width) &&
           (width!=12u || info->block_bytes==65536u);
}

/* Report every absent operation in the same qualification path used by
 * the destructive worker. Do not infer capabilities from a filename or
 * from the presence of an unadvertised function pointer. */
static void missing(MediaV2 *m,const char *operation)
{
    if(!m->detail || !m->detail_capacity)return;
    size_t used=strlen(m->detail);
    if(used<m->detail_capacity)
        (void)snprintf(m->detail+used,m->detail_capacity-used,
            "%s%s",used?", ":"",operation);
}
static const IfsFsModuleApiV2 *qualify(void *symbol,MediaV2 *m)
{
    IfsFsModuleApiEntryV2 entry=NULL;
    _Static_assert(sizeof(entry)==sizeof(symbol),"Common V2 entry width");
    memcpy(&entry,&symbol,sizeof(entry));
    const IfsFsModuleApiV2 *api=entry?entry():NULL;
    if(!api || !ifs_fs_api_v2_valid(api,provider(m->spec)) ||
       !(api->runtime_profiles & IFS_FS_PROFILE_HOSTED)) {
        missing(m,"valid hosted Common V2 descriptor");
        return NULL;
    }
    const IfsFsCoreOpsV2 *core=
        (const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *volume=
        (const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    int ok=1;
#define NEED(test, name) do { if(!(test)) { missing(m,name); ok=0; } } while(0)
#define CORE(cap, member) (core && (core->header.capabilities & (cap)) && core->member)
    NEED(CORE(IFS_FS_CORE_PROBE,probe),"CORE.PROBE");
    if(m->create)
        NEED(volume && (volume->header.capabilities & IFS_FS_VOLUME_FORMAT) &&
             volume->format,"VOLUME.FORMAT");
    if(profile_fat_width(m->spec)==12u)
        NEED(volume && (volume->header.capabilities&IFS_FS_VOLUME_CHECK) &&
             volume->check,"VOLUME.CHECK (FAT12 qualification)");
    if(!is_swap(m->spec)) {
        NEED(CORE(IFS_FS_CORE_MOUNT,mount) && core->unmount,"CORE.MOUNT/UNMOUNT");
        NEED(CORE(IFS_FS_CORE_STAT,stat),"CORE.STAT");
        NEED(CORE(IFS_FS_CORE_LIST,directory_entry),"CORE.LIST");
        NEED(CORE(IFS_FS_CORE_READ,read),"CORE.READ");
        if(m->create) {
            NEED(CORE(IFS_FS_CORE_CREATE,create),"CORE.CREATE");
            NEED(CORE(IFS_FS_CORE_WRITE,write),"CORE.WRITE");
            if(is_bfs(m->spec))NEED(CORE(IFS_FS_CORE_REMOVE,remove),"CORE.REMOVE (BFS gap corpus)");
        }
        if(m->require_map) {
            const IfsFsDefragOpsV2 *map=(const IfsFsDefragOpsV2 *)
                ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
            NEED(map && (map->header.capabilities & IFS_FS_DEFRAG_MAP) &&
                 map->map,"DEFRAG.MAP");
        }
    }
#undef CORE
#undef NEED
    return ok?api:NULL;
}
static int module_call(MediaV2 *m,IfsFilesystemCall callback)
{
    const char *id=provider(m->spec);
    char filename[64]={0};
    if(!id || ifs_filesystem_module_find_readonly_v2(
            id,filename,sizeof(filename))!=0) {
        missing(m,"unique trusted hosted Common V2 provider");
        return -1;
    }
    const uint32_t width=profile_fat_width(m->spec);
    if(width) {
        char expected[16];
        if(snprintf(expected,sizeof(expected),"FAT%u.so",width)>=(int)sizeof(expected) ||
           strcmp(filename,expected)!=0) {
            missing(m,"canonical provider filename");
            return -1;
        }
    }
    return ifs_filesystem_module_call(id,filename,
                                     IFS_FS_COMMON_EXPORT_V2,callback,m);
}
static int available(const IfsDefragHostV2 *unused,void *symbol,void *arg)
{
    (void)unused;
    return qualify(symbol,arg)?0:-1;
}
/* Preflight the complete destructive-media workload, including the ability
 * to prove actual allocation fragmentation. ABI presence alone is not enough. */
int ldtm_common_v2_media_detail(const LdtmFilesystemSpec *s,
                                char *detail,size_t capacity)
{
    if(detail && capacity)detail[0]='\0';
    MediaV2 m={.spec=s,.create=1,.require_map=1,
               .detail=detail,.detail_capacity=capacity};
    if(module_call(&m,available)!=0) {
        if(detail && capacity && !detail[0])
            missing(&m,"trusted provider load");
        return 0;
    }
    return 1;
}
int ldtm_common_v2_media_available(const LdtmFilesystemSpec *s)
{
    return ldtm_common_v2_media_detail(s,NULL,0u);
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

/* SCO BFS intrinsically requires one contiguous extent per file. Build
 * genuine fragmented free space by alternating sacrificial files and
 * complete target files, then remove only the sacrificial files. All
 * formatting, namespace changes and payload writes remain provider-owned. */
static int create_bfs_corpus(const MediaV2 *m,const IfsFsCoreOpsV2 *core,
                             void *session,uint32_t unit)
{
    const uint32_t capacity=4u*1024u*1024u;
    uint8_t *buf=malloc(capacity);
    if(!buf)return -1;
    int rc=-1;char path[32],gap[32];
    memset(buf,0,unit);
    for(unsigned i=0;i<LDTM_RAW_FILES;++i) {
        if(i<LDTM_TARGET_FILE_COUNT) {
            (void)snprintf(gap,sizeof(gap),"/G%06u.TMP",i);
            memset(buf,0x5a,unit);
            if(core->create(session,gap,IFS_FS_ENTRY_FILE)!=IFS_FS_OK ||
               core->write(session,gap,0u,buf,unit)!=IFS_FS_OK)goto done;
        }
        if(make_path(i,path)!=0)goto done;
        uint64_t length=ldtm_raw_file_bytes(m->spec,i);
        for(uint64_t at=0;at<length;) {
            uint32_t n=(uint32_t)(length-at<capacity?length-at:capacity);
            if(m->services->stop_requested(m->services->context))goto done;
            ldtm_raw_payload(buf,n,i,at);
            if(core->write(session,path,at,buf,n)!=IFS_FS_OK)goto done;
            at+=n;
        }
    }
    for(unsigned i=0;i<LDTM_TARGET_FILE_COUNT;++i) {
        (void)snprintf(gap,sizeof(gap),"/G%06u.TMP",i);
        if(core->remove(session,gap)!=IFS_FS_OK)goto done;
    }
    rc=0;
done:
    free(buf);return rc;
}
/* BFS root and every file are separately mapped; compare their rounded
 * allocation sum to the provider's volume accounting. The native check
 * rejects unreferenced allocations. Consequently all remaining space
 * is unowned, not merely absent from a partial file map. */
typedef struct {uint64_t start,end;unsigned index;} BfsRun;
static int bfs_single_run(const IfsFsDefragOpsV2 *map,void *session,
                          const char *path,uint64_t length,uint64_t unit,
                          uint64_t capacity,BfsRun *out)
{
    IfsFsExtentV2 e={0};uint32_t n=0;
    if(!length)return map->map(session,path,0,&e,1,&n)==IFS_FS_END && !n?0:-1;
    if(map->map(session,path,0,&e,1,&n)!=IFS_FS_OK || n!=1 ||
       e.logical_offset || e.length!=length || e.flags || e.reserved ||
       e.physical_offset%unit || e.physical_offset>capacity ||
       length>UINT64_MAX-(unit-1))return -1;
    uint64_t allocated=((length+unit-1)/unit)*unit;
    if(allocated>capacity-e.physical_offset)return -1;
    out->start=e.physical_offset;out->end=e.physical_offset+allocated;
    return map->map(session,path,1,&e,1,&n)==IFS_FS_END && !n?0:-1;
}
static int verify_bfs_layout(const MediaV2 *m,const IfsFsCoreOpsV2 *core,
    const IfsFsVolumeOpsV2 *volume,const IfsFsDefragOpsV2 *map,void *session,
    const IfsFsVolumeInfoV2 *info,uint64_t capacity)
{
    const uint64_t unit=info->block_bytes;
    if(unit!=512u || !volume || !volume->check ||
       !(volume->header.capabilities&IFS_FS_VOLUME_CHECK))return -1;
    uint64_t errors=UINT64_MAX;
    if(volume->check(session,0,&errors)!=IFS_FS_OK || errors)return -1;
    char root_name[32],path[32];
    IfsFsEntryV2 e={.structure_bytes=sizeof(e),.name=root_name,.name_capacity=sizeof(root_name)};
    BfsRun root={0},runs[LDTM_RAW_FILES]={{0}};
    if(core->stat(session,"/",&e)!=IFS_FS_OK || e.type!=IFS_FS_ENTRY_DIRECTORY ||
       !e.size_bytes || bfs_single_run(map,session,"/",e.size_bytes,unit,capacity,&root))return -1;
    if(info->total_bytes>capacity-root.start || info->free_bytes>info->total_bytes)return -1;
    uint64_t sum=root.end-root.start,limit=root.start+info->total_bytes;
    unsigned count=0;
    for(unsigned i=0;i<LDTM_RAW_FILES;++i) {
        uint64_t length=ldtm_raw_file_bytes(m->spec,i);BfsRun r={.index=i};
        if(make_path(i,path) || bfs_single_run(map,session,path,length,unit,capacity,&r))return -1;
        if(!length)continue;
        if(r.start<root.end || r.end>limit || r.end-r.start>info->total_bytes-sum)return -1;
        sum+=r.end-r.start;
        unsigned j=count++;
        while(j && runs[j-1].start>r.start){runs[j]=runs[j-1];--j;}
        runs[j]=r;
    }
    if(sum!=info->total_bytes-info->free_bytes)return -1;
    uint64_t end=root.end;unsigned gap_targets=0;
    for(unsigned i=0;i<count;++i) {
        const BfsRun *r=&runs[i];
        if(r->start<end)return -1;
        if(m->bfs_layout==1 && r->start!=end)return -1;
        if(m->bfs_layout==2 && r->index<LDTM_TARGET_FILE_COUNT) {
            if(r->start-end<unit)return -1;
            ++gap_targets;
        }
        if(m->bfs_layout==3) {
            uint64_t blocks=(r->end-r->start)/unit;
            uint64_t reserve=(blocks/10+(blocks%10!=0))*unit;
            uint64_t next=i+1<count?runs[i+1].start:limit;
            if(next<r->end || next-r->end<reserve)return -1;
        }
        end=r->end;
    }
    if(m->bfs_layout==2 && gap_targets!=LDTM_TARGET_FILE_COUNT)return -1;
    return 0;
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
    m->services=host;
    if(host->open_target(host->context,m->path,m->create,NULL,0u,
                         &m->binding.target)!=0)return -1;
    int result=-1,mounted=0;
    const char *stage="target geometry/runtime";
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
        case LDTM_CREATOR_FAT12: variant=12u; label=NULL; break;
        case LDTM_CREATOR_FAT16: variant=16u; label=NULL; break;
        case LDTM_CREATOR_FAT32: variant=32u; label=NULL; break;
        case LDTM_CREATOR_EXFAT: variant=64u; label=NULL; break;
        case LDTM_CREATOR_NTFS: variant=1u; break;
        case LDTM_CREATOR_EXT3: variant=3u; break;
        case LDTM_CREATOR_EXT4: variant=4u; break;
        case LDTM_CREATOR_XFS: variant=5u; break;
        case LDTM_CREATOR_BTRFS: variant=1u; break;
        case LDTM_CREATOR_HFS: variant=1u; break;
        case LDTM_CREATOR_HFSPLUS: variant=4u; break;
        case LDTM_CREATOR_APFS: variant=1u; break;
        case LDTM_CREATOR_AFFS:
            if(!strcmp(m->spec->key,"sfs"))variant=3u;
            break;
        case LDTM_CREATOR_UFS: variant=3u; break;
        case LDTM_CREATOR_UFS1: variant=1u; break;
        case LDTM_CREATOR_SFS2: variant=4096u; break;
        case LDTM_CREATOR_SWAP: variant=4096u; break;
        default: break;
        }
        stage="VOLUME.FORMAT";
        const int rc=volume->format(&runtime,variant,label,0u);
        if(rc!=IFS_FS_OK) {
            fprintf(stderr,"%s Common V2 FORMAT failed (variant %u, error %d)\n",
                    provider(m->spec),(unsigned)variant,rc);
            goto done;
        }
        if(variant) {
            uint32_t detected=0u;
            stage="CORE.PROBE exact format variant";
            if(core->probe(&runtime,&detected)!=IFS_FS_OK || detected!=variant) {
                fprintf(stderr,"%s: expected variant %u, detected %u\n",
                        provider(m->spec),(unsigned)variant,(unsigned)detected);
                goto done;
            }
        }
    }
    if(is_swap(m->spec)) {
        uint32_t variant=0u;
        result=core->probe(&runtime,&variant)==IFS_FS_OK && variant==4096u?0:-1;
        goto done;
    }
    stage="CORE.MOUNT";
    if(core->mount(&runtime,&session,&info)!=IFS_FS_OK || !session)goto done;
    mounted=1;
    stage="mounted allocation geometry";
    if(!profile_geometry_valid(m->spec,&info))goto done;
    uint8_t actual[65536],expected[65536];
    char path[32],entry_name[768];
    if(m->create) {
        stage="CORE.CREATE";
        for(unsigned file=0u;file<LDTM_RAW_FILES;++file) {
            if(make_path(file,path)!=0 ||
               core->create(session,path,IFS_FS_ENTRY_FILE)!=IFS_FS_OK)
                goto done;
        }
        /* Alternate the first and second halves to force interleaved file
         * growth through the provider's own allocation/CoW machinery. */
        stage="CORE.WRITE";
        if(is_bfs(m->spec)) {
            if(create_bfs_corpus(m,core,session,info.block_bytes)!=0)goto done;
        } else if(m->spec->creator==LDTM_CREATOR_NTFS) {
            /* NTFS's native allocator can extend a run into adjacent free
             * space. Interleave bounded one-MiB increments rather than two
             * large passes. Keep each individual host transfer unchanged.
             * This limits runlist pressure on the existing native reader
             * while retaining exactly the same lengths and payload bytes.
             * NTFS owns placement; the independent eight-file MAP gate
             * remains the authority on whether fragmentation was achieved.
             */
            const uint64_t quantum=LDTM_MIB;
            for(uint64_t base=0u;;base+=quantum) {
                int remaining=0;
                for(unsigned file=0u;file<LDTM_RAW_FILES;++file) {
                    const uint64_t length=ldtm_raw_file_bytes(m->spec,file);
                    if(base>=length)continue;
                    remaining=1;
                    const uint64_t end=length-base<quantum?length:base+quantum;
                    if(make_path(file,path)!=0)goto done;
                    for(uint64_t at=base;at<end;) {
                        const uint32_t n=(uint32_t)(end-at<sizeof(actual)?
                                                    end-at:sizeof(actual));
                        if(host->stop_requested(host->context))goto done;
                        ldtm_raw_payload(expected,n,file,at);
                        const int rc=core->write(session,path,at,expected,n);
                        if(rc!=IFS_FS_OK) {
                            fprintf(stderr,"%s: write %s at %llu (%u bytes) failed: %d\n",
                                    provider(m->spec),path,(unsigned long long)at,n,rc);
                            goto done;
                        }
                        at+=n;
                    }
                }
                if(!remaining)break;
            }
        } else for(unsigned phase=0u;phase<2u;++phase)
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
                    const int rc=core->write(session,path,at,expected,n);
                    if(rc!=IFS_FS_OK) {
                        fprintf(stderr,"%s: write %s at %llu (%u bytes) failed: %d\n",
                                provider(m->spec),path,(unsigned long long)at,n,rc);
                        goto done;
                    }
                    at+=n;
                }
            }
        stage="CORE.UNMOUNT after write";
        const int unmount_rc=core->unmount(session);
        mounted=0;session=NULL;
        if(unmount_rc!=IFS_FS_OK)goto done;
        stage="target sync";
        if(host->sync_target(host->context,&m->binding.target)!=0)goto done;
        /* The post-format corpus check is deliberately a read-only mount,
         * even while retaining the original exclusive device lease. A
         * provider must not repair or change the just-written evidence. */
        if(ifs_filesystem_target_runtime_v2(&m->binding,0,0u,&runtime)!=0 ||
           !ifs_fs_host_v2_valid(&runtime))goto done;
        stage="read-only CORE.MOUNT";
        info=(IfsFsVolumeInfoV2){.structure_bytes=sizeof(info)};
        if(core->mount(&runtime,&session,&info)!=IFS_FS_OK || !session)goto done;
        mounted=1;
        if(!profile_geometry_valid(m->spec,&info))goto done;
    }
    stage="CORE.LIST";
    unsigned seen=0u;
    int directory_complete=0;
    for(uint32_t index=0u;index<65536u;++index) {
        memset(entry_name,0xff,sizeof(entry_name));
        IfsFsEntryV2 e={.structure_bytes=sizeof(e),
                        .name=entry_name,.name_capacity=sizeof(entry_name)};
        int rc=core->directory_entry(session,"/",index,&e);
        if(rc==IFS_FS_END){directory_complete=1;break;}
        if(rc!=IFS_FS_OK || !memchr(entry_name,'\0',sizeof(entry_name)))goto done;
        int number=ldtm_raw_name_index(entry_name);
        if(number<0) {
            /* FAT's corpus profile admits exactly its 16 files; other
             * providers may expose genuine native metadata entries. */
            if(profile_fat_width(m->spec) || is_bfs(m->spec))goto done;
            continue;
        }
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
        stage="CORE.STAT";
        if(core->stat(session,path,&e)!=IFS_FS_OK ||
           e.type!=IFS_FS_ENTRY_FILE || e.size_bytes!=length)
            goto done;
        /* Every corpus file must have a complete physical mapping. The
         * fragmentation threshold applies only to the large target files;
         * small boundary-sized files still need independently verified maps. */
        if(m->require_map) {
            stage="DEFRAG.MAP";
            const IfsFsDefragOpsV2 *map=(const IfsFsDefragOpsV2 *)
                ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
            unsigned displaced=0u;
            if(file_fragmented(map,session,path,length,runtime.volume_bytes,
                info.block_bytes,host,&displaced)!=0) {
                fprintf(stderr,"%s: incomplete Common V2 extent map for %s\n",
                        provider(m->spec),path);
                goto done;
            }
            if(file<LDTM_TARGET_FILE_COUNT)m->fragmented+=displaced;
        }
        stage="CORE.READ and payload comparison";
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
    if(m->bfs_layout) {
        stage="BFS independently verified allocation layout";
        const IfsFsDefragOpsV2 *map=(const IfsFsDefragOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
        if(m->fragmented || verify_bfs_layout(m,core,volume,map,session,&info,runtime.volume_bytes))goto done;
    }
    /* Exercise the provider's native integrity checker whenever advertised,
     * not just for FAT. FAT12 CHECK remains mandatory at preflight. An
     * unavailable checker is not evidence of filesystem integrity. */
    if(volume && volume->check &&
       (volume->header.capabilities&IFS_FS_VOLUME_CHECK)) {
        stage="VOLUME.CHECK";
        uint64_t errors=UINT64_MAX;
        if(volume->check(session,0u,&errors)!=IFS_FS_OK || errors!=0u)
            goto done;
    }
    result=0;
done:
    if(result!=0)
        fprintf(stderr,"%s Common V2 corpus failed at %s\n",provider(m->spec),stage);
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

int ldtm_common_v2_verify_bfs_layout(const LdtmFilesystemSpec *s,const char *path,int mode)
{
    if(!is_bfs(s) || !path || mode<0 || mode>2)return -1;
    MediaV2 m={.spec=s,.path=path,.require_map=1,.bfs_layout=mode+1};
    return module_call(&m,execute);
}
