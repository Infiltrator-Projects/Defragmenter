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
    int require_contiguous; /* Post-mutation proof covers every nonempty file. */
    int contiguous_layout; /* 1: compact, 2: fragmented free space, 3: growth */
    IfsFsFormatProfileV2 profile;
    int verify_growth;
    unsigned fragmented;
    char *detail;
    size_t detail_capacity;
    const IfsDefragHostV2 *services;
    IfsFilesystemTargetV2 binding;
} MediaV2;

static const char *provider(const LdtmFilesystemSpec *s)
{
    if(!s || !s->key)return NULL;
    return s->key;
}
static int no_namespace(const MediaV2 *m)
{return m->profile.layout==IFS_FS_LAYOUT_NO_NAMESPACE;}
static int contiguous_layout(const MediaV2 *m)
{return m->profile.layout==IFS_FS_LAYOUT_CONTIGUOUS_FILE;}
static int blocked_growth(const MediaV2 *m)
{return m->profile.layout==IFS_FS_LAYOUT_BLOCKED_GROWTH;}
static int profile_geometry_valid(const MediaV2 *m,const IfsFsVolumeInfoV2 *info)
{
    return info->block_bytes>=512u &&
           !(info->block_bytes&(info->block_bytes-1u)) &&
           info->variant==m->profile.probe_variant &&
           (!m->profile.exact_block_bytes ||
            info->block_bytes==m->profile.exact_block_bytes);
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
    const IfsFsFormatProfileOpsV2 *format=
        (const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_FORMAT_PROFILE);
    m->profile=(IfsFsFormatProfileV2){.structure_bytes=sizeof(m->profile)};
    if(!format || !(format->header.capabilities&IFS_FS_FORMAT_PROFILE_DEFAULT) ||
       !format->get_default || format->get_default(&m->profile)!=IFS_FS_OK ||
       !ifs_fs_format_profile_v2_valid(&m->profile)) {
        missing(m,"valid provider FORMAT_PROFILE.DEFAULT");return NULL;
    }
    const uint64_t capacity=(uint64_t)m->spec->size_mib*LDTM_MIB;
    if(capacity<m->profile.minimum_bytes ||
       (m->profile.maximum_bytes && capacity>m->profile.maximum_bytes)) {
        missing(m,"supported disposable geometry");return NULL;
    }
    int ok=1;
#define NEED(test, name) do { if(!(test)) { missing(m,name); ok=0; } } while(0)
#define CORE(cap, member) (core && (core->header.capabilities & (cap)) && core->member)
    NEED(CORE(IFS_FS_CORE_PROBE,probe),"CORE.PROBE");
    if(m->create)
        NEED(volume && (volume->header.capabilities & IFS_FS_VOLUME_FORMAT) &&
             volume->format,"VOLUME.FORMAT");
    if((m->profile.flags&IFS_FS_FORMAT_REQUIRE_CHECK) || m->require_contiguous)
        NEED(volume && (volume->header.capabilities&IFS_FS_VOLUME_CHECK) &&
             volume->check,"VOLUME.CHECK (integrity qualification)");
    if(!no_namespace(m)) {
        NEED(CORE(IFS_FS_CORE_MOUNT,mount) && core->unmount,"CORE.MOUNT/UNMOUNT");
        NEED(CORE(IFS_FS_CORE_STAT,stat),"CORE.STAT");
        NEED(CORE(IFS_FS_CORE_LIST,directory_entry),"CORE.LIST");
        NEED(CORE(IFS_FS_CORE_READ,read),"CORE.READ");
        if(m->create) {
            NEED(CORE(IFS_FS_CORE_CREATE,create),"CORE.CREATE");
            NEED(CORE(IFS_FS_CORE_WRITE,write),"CORE.WRITE");
            if(contiguous_layout(m) || blocked_growth(m))
                NEED(CORE(IFS_FS_CORE_REMOVE,remove),"CORE.REMOVE (sacrificial allocation corpus)");
            if(blocked_growth(m))
                NEED(volume && (volume->header.capabilities&IFS_FS_VOLUME_QUERY) &&
                     volume->query,"VOLUME.QUERY (blocked-growth sacrificial allocation budget)");
        }
        if(m->require_map || (m->create && blocked_growth(m))) {
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
    return ifs_filesystem_module_call(id,filename,
                                     IFS_FS_COMMON_EXPORT_V2,callback,m);
}
static int available(const IfsDefragHostV2 *unused,void *symbol,void *arg)
{
    (void)unused;
    return qualify(symbol,arg)?0:-1;
}
int ldtm_common_v2_layout(const LdtmFilesystemSpec *s)
{
    MediaV2 m={.spec=s};
    return s && module_call(&m,available)==0?(int)m.profile.layout:-1;
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

/* Workload-only blocked-growth fragmentation. The provider alone allocates blocks;
 * these helpers use verified physical extents to decide when enough real
 * sacrificial file allocations have been created. No allocator hint,
 * metadata parsing, raw placement write or synthetic extent is used.
 */
typedef struct {
    uint64_t tail[LDTM_TARGET_FILE_COUNT];
    unsigned blocked[LDTM_TARGET_FILE_COUNT];
} BlockedCorpusTails;

static int blocked_corpus_scan(const MediaV2 *m,const IfsFsDefragOpsV2 *map,
    void *session,const char *path,uint64_t bytes,uint64_t capacity,
    unsigned own,BlockedCorpusTails *tails,int record_tail)
{
    uint64_t first=0u,logical=0u,last=0u;
    unsigned displaced=0u;
    for(unsigned page=0u;page<131072u;++page) {
        IfsFsExtentV2 ranges[32]={{0}};
        uint32_t count=0u;
        if(m->services->stop_requested(m->services->context))return -1;
        const int rc=map->map(session,path,first,ranges,32u,&count);
        if(rc==IFS_FS_END) {
            if(count || logical!=bytes)return -1;
            if(record_tail && own<LDTM_TARGET_FILE_COUNT) {
                tails->tail[own]=last;
                tails->blocked[own]=displaced || last==capacity;
            }
            return 0;
        }
        if(rc!=IFS_FS_OK || !count || count>32u || first>UINT64_MAX-count)
            return -1;
        for(uint32_t i=0u;i<count;++i) {
            const IfsFsExtentV2 *e=&ranges[i];
            if(e->flags || e->reserved || !e->length ||
               e->logical_offset!=logical || logical>bytes ||
               e->length>bytes-logical || e->physical_offset>capacity ||
               e->length>capacity-e->physical_offset)return -1;
            if(logical && e->physical_offset!=last)displaced=1u;
            last=e->physical_offset+e->length;
            if(!record_tail)
                for(unsigned k=0u;k<LDTM_TARGET_FILE_COUNT;++k)
                    if(k!=own && e->physical_offset<=tails->tail[k] &&
                       tails->tail[k]<last)tails->blocked[k]=1u;
            logical+=e->length;
        }
        first+=count;
    }
    return -1;
}
static unsigned blocked_corpus_blocked(const BlockedCorpusTails *tails)
{
    unsigned n=0u;
    for(unsigned k=0u;k<LDTM_TARGET_FILE_COUNT;++k)n+=tails->blocked[k]!=0u;
    return n;
}
static int blocked_corpus_write(const MediaV2 *m,const IfsFsCoreOpsV2 *core,
    void *session,const char *path,unsigned file,uint64_t start,uint64_t end,
    int sacrificial)
{
    uint8_t buf[65536];
    if(sacrificial)memset(buf,0xa5,sizeof(buf));
    for(uint64_t at=start;at<end;) {
        const uint32_t n=(uint32_t)(end-at<sizeof(buf)?end-at:sizeof(buf));
        if(m->services->stop_requested(m->services->context))return -1;
        if(!sacrificial)ldtm_raw_payload(buf,n,file,at);
        const int rc=core->write(session,path,at,buf,n);
        if(rc!=IFS_FS_OK) {
            fprintf(stderr,"blocked-growth corpus write %s at %llu (%u bytes) failed: %d\n",
                path,(unsigned long long)at,n,rc);
            return -1;
        }
        at+=n;
    }
    return 0;
}
static int create_blocked_corpus(const MediaV2 *m,const IfsFsCoreOpsV2 *core,
    const IfsFsVolumeOpsV2 *volume,const IfsFsDefragOpsV2 *map,
    void *session,const IfsFsVolumeInfoV2 *initial,uint64_t capacity)
{
    const uint64_t unit=initial->block_bytes;
    const uint64_t quantum=unit>LDTM_MIB?unit:LDTM_MIB;
    const uint64_t blocker_max=32u*LDTM_MIB;
    const char *temporary="/LDTM-TEMP";
    char path[32];
    BlockedCorpusTails tails={0};
    uint64_t remaining=0u;
    unsigned blockers=0u;
    if(!unit || !core->remove || !volume->query || !map || !map->map)
        return -1;
    /* Establish one real allocated prefix in each target. Every other
     * payload byte is deferred until the blocking workload is established. */
    for(unsigned file=0u;file<LDTM_RAW_FILES;++file) {
        const uint64_t bytes=ldtm_raw_file_bytes(m->spec,file);
        uint64_t prefix=file<LDTM_TARGET_FILE_COUNT?quantum:0u;
        if(file<LDTM_TARGET_FILE_COUNT && (bytes<=prefix ||
           make_path(file,path) ||
           blocked_corpus_write(m,core,session,path,file,0u,prefix,0)))return -1;
        if(bytes-prefix>UINT64_MAX-remaining)return -1;
        remaining+=bytes-prefix;
    }
    for(unsigned file=0u;file<LDTM_TARGET_FILE_COUNT;++file)
        if(make_path(file,path) ||
           blocked_corpus_scan(m,map,session,path,quantum,capacity,file,&tails,1))
            return -1;
    for(unsigned file=0u;file<LDTM_TARGET_FILE_COUNT;++file)
        if(make_path(file,path) ||
           blocked_corpus_scan(m,map,session,path,quantum,capacity,file,&tails,0))
            return -1;
    if(core->create(session,temporary,IFS_FS_ENTRY_DIRECTORY)!=IFS_FS_OK)return -1;
    /* The native allocator may place files in different locality zones.
     * Keep temporary files allocated until the real payloads have grown.
     * Their physical maps, not creation order, establish whether a target's
     * next cluster is occupied. Reserve all remaining payload capacity plus
     * a conservative 64 MiB for native metadata growth; never fill to ENOSPC.
     */
    const uint64_t reserve=64u*LDTM_MIB;
    if(remaining>UINT64_MAX-reserve)return -1;
    while(blocked_corpus_blocked(&tails)<LDTM_TARGET_FILE_COUNT && blockers<128u) {
        IfsFsVolumeInfoV2 current={.structure_bytes=sizeof(current)};
        if(volume->query(session,&current)!=IFS_FS_OK || current.block_bytes!=unit ||
           current.free_bytes>current.total_bytes)return -1;
        if(current.free_bytes<=remaining+reserve)break;
        uint64_t bytes=current.free_bytes-remaining-reserve;
        if(bytes>blocker_max)bytes=blocker_max;
        bytes-=bytes%unit;
        if(!bytes)break;
        const int n=snprintf(path,sizeof(path),"%s/B%03u.TMP",temporary,blockers);
        if(n<=0 || (size_t)n>=sizeof(path) ||
           core->create(session,path,IFS_FS_ENTRY_FILE)!=IFS_FS_OK ||
           blocked_corpus_write(m,core,session,path,0u,0u,bytes,1) ||
           blocked_corpus_scan(m,map,session,path,bytes,capacity,
                            LDTM_TARGET_FILE_COUNT,&tails,0))return -1;
        ++blockers;
    }
    fprintf(stderr,"blocked-growth corpus: %u target prefixes physically blocked or already fragmented; %u temporary files\n",
            blocked_corpus_blocked(&tails),blockers);
    /* Interleave remaining growth while the blockers are still allocated.
     * Exact final fragmentation is always re-proved after their removal and
     * a fresh read-only mount; a scheduling hint is not a qualification. */
    for(uint64_t base=0u;;base+=quantum) {
        int any=0;
        for(unsigned file=0u;file<LDTM_RAW_FILES;++file) {
            const uint64_t bytes=ldtm_raw_file_bytes(m->spec,file);
            if(file<LDTM_TARGET_FILE_COUNT && base==0u){any=1;continue;}
            if(base>=bytes)continue;
            any=1;
            const uint64_t end=bytes-base<quantum?bytes:base+quantum;
            if(make_path(file,path) ||
               blocked_corpus_write(m,core,session,path,file,base,end,0))return -1;
        }
        if(!any)break;
    }
    for(unsigned i=0u;i<blockers;++i) {
        const int n=snprintf(path,sizeof(path),"%s/B%03u.TMP",temporary,i);
        if(n<=0 || (size_t)n>=sizeof(path) ||
           core->remove(session,path)!=IFS_FS_OK)return -1;
    }
    return core->remove(session,temporary)==IFS_FS_OK?0:-1;
}

/* A contiguous-file layout intrinsically requires one contiguous extent per file. Build
 * genuine fragmented free space by alternating sacrificial files and
 * complete target files, then remove only the sacrificial files. All
 * formatting, namespace changes and payload writes remain provider-owned. */
static int create_contiguous_corpus(const MediaV2 *m,const IfsFsCoreOpsV2 *core,
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
/* contiguous-file root and every file are separately mapped; compare their rounded
 * allocation sum to the provider's volume accounting. The native check
 * rejects unreferenced allocations. Consequently all remaining space
 * is unowned, not merely absent from a partial file map. */
typedef struct {uint64_t start,end;unsigned index;} ContiguousRun;
static int single_run(const IfsFsDefragOpsV2 *map,void *session,
                          const char *path,uint64_t length,uint64_t unit,
                          uint64_t capacity,ContiguousRun *out)
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
static int verify_contiguous_layout(const MediaV2 *m,const IfsFsCoreOpsV2 *core,
    const IfsFsVolumeOpsV2 *volume,const IfsFsDefragOpsV2 *map,void *session,
    const IfsFsVolumeInfoV2 *info,uint64_t capacity)
{
    const uint64_t unit=info->block_bytes;
    if(!unit || !volume || !volume->check ||
       !(volume->header.capabilities&IFS_FS_VOLUME_CHECK))return -1;
    uint64_t errors=UINT64_MAX;
    if(volume->check(session,0,&errors)!=IFS_FS_OK || errors)return -1;
    char root_name[32],path[32];
    IfsFsEntryV2 e={.structure_bytes=sizeof(e),.name=root_name,.name_capacity=sizeof(root_name)};
    ContiguousRun root={0},runs[LDTM_RAW_FILES]={{0}};
    if(core->stat(session,"/",&e)!=IFS_FS_OK || e.type!=IFS_FS_ENTRY_DIRECTORY ||
       !e.size_bytes || single_run(map,session,"/",e.size_bytes,unit,capacity,&root))return -1;
    if(info->total_bytes>capacity-root.start || info->free_bytes>info->total_bytes)return -1;
    uint64_t sum=root.end-root.start,limit=root.start+info->total_bytes;
    unsigned count=0;
    for(unsigned i=0;i<LDTM_RAW_FILES;++i) {
        uint64_t length=ldtm_raw_file_bytes(m->spec,i);ContiguousRun r={.index=i};
        if(make_path(i,path) || single_run(map,session,path,length,unit,capacity,&r))return -1;
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
        const ContiguousRun *r=&runs[i];
        if(r->start<end)return -1;
        if(m->contiguous_layout==1 && r->start!=end)return -1;
        if(m->contiguous_layout==2 && r->index<LDTM_TARGET_FILE_COUNT) {
            if(r->start-end<unit)return -1;
            ++gap_targets;
        }
        if(m->contiguous_layout==3) {
            uint64_t blocks=(r->end-r->start)/unit;
            uint64_t reserve=(blocks/10+(blocks%10!=0))*unit;
            uint64_t next=i+1<count?runs[i+1].start:limit;
            if(next<r->end || next-r->end<reserve)return -1;
        }
        end=r->end;
    }
    if(m->contiguous_layout==2 && gap_targets!=LDTM_TARGET_FILE_COUNT)return -1;
    return 0;
}

/* The native file maps prove logical coverage and the last payload byte;
 * the separate whole-volume allocation map proves adjacent allocator space.
 * Never infer free space from gaps in a file map or from VOLUME.QUERY alone.
 * Each physical allocation block is indivisible. Less than one allocation
 * unit of padding may follow the last payload byte (including format-owned
 * per-block overhead); an additional occupied block may not intervene.
 */
typedef struct {
    uint64_t end,required,start,proved;
    int tail_owned,started;
} NativeGrowthGap;

static int verify_native_growth(const MediaV2 *m,const IfsFsModuleApiV2 *api,
                               void *session,const IfsFsVolumeInfoV2 *info,
                               uint64_t capacity)
{
    const IfsFsDefragOpsV2 *map=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    const IfsFsAllocationOpsV2 *allocation=(const void*)
        ifs_fs_group_v2(api,IFS_FS_GROUP_ALLOCATION);
    if(!map || !map->map || !allocation || !allocation->map ||
       !(allocation->header.capabilities&IFS_FS_ALLOCATION_MAP)) {
        fprintf(stderr,"%s: canonical ALLOCATION.MAP required for growth verification\n",provider(m->spec));
        return -1;
    }
    const uint64_t unit=info->block_bytes;
    if(!unit || info->free_bytes>capacity)return -1;
    NativeGrowthGap gaps[LDTM_RAW_FILES]={{0}};
    for(unsigned file=0u;file<LDTM_RAW_FILES;++file) {
        char path[32];if(make_path(file,path))return -1;
        const uint64_t length=ldtm_raw_file_bytes(m->spec,file);
        uint64_t logical=0u,first=0u,last=0u,blocks=0u;
        int ended=0;
        for(unsigned page=0u;page<131072u;++page) {
            IfsFsExtentV2 ranges[32]={{0}};uint32_t n=0u;
            if(m->services->stop_requested(m->services->context))return -1;
            int rc=map->map(session,path,first,ranges,32u,&n);
            if(rc==IFS_FS_END){if(n || logical!=length)return -1;ended=1;break;}
            if(rc!=IFS_FS_OK || !n || n>32u || first>UINT64_MAX-n)return -1;
            for(uint32_t j=0u;j<n;++j) {
                const IfsFsExtentV2 *e=&ranges[j];
                if(e->flags || e->reserved || !e->length || e->logical_offset!=logical ||
                   e->length>length-logical || e->physical_offset>capacity ||
                   e->length>capacity-e->physical_offset ||
                   (logical && (e->physical_offset<last || e->physical_offset-last>=unit)))
                    return -1;
                uint64_t add=e->length/unit+(e->length%unit!=0u);
                if(add>UINT64_MAX-blocks)return -1;
                blocks+=add;logical+=e->length;last=e->physical_offset+e->length;
            }
            first+=n;
        }
        /* An empty file has no data extent and no growth requirement.
         * Every nonempty corpus file, including boundary-size payloads,
         * must independently prove its real persistent native allocation reservation. */
        if(!ended)return -1;
        if(!length) {
            if(blocks || last)return -1;
            continue;
        }
        if(!blocks || !last)return -1;
        uint64_t reserve=blocks/10u+(blocks%10u!=0u);
        if(reserve>UINT64_MAX/unit || reserve*unit>capacity-last)return -1;
        gaps[file].end=last;gaps[file].required=reserve*unit;
    }
    uint64_t at=0u,unknown=0u,free_bytes=0u;
    int ended=0;
    for(unsigned page=0u;page<1048576u;++page) {
        IfsFsAllocationRangeV2 ranges[64]={{0}};
        uint32_t n=0u;uint64_t next=at;
        if(m->services->stop_requested(m->services->context))return -1;
        const int rc=allocation->map(session,at,ranges,64u,&n);
        if(!ifs_fs_allocation_page_v2_valid(capacity,at,rc,ranges,64u,n,&next,&unknown) || unknown) {
            fprintf(stderr,"%s: invalid growth allocation page at byte %llu (status %d, entries %u, unknown %llu)\n",
                    provider(m->spec),(unsigned long long)at,rc,n,(unsigned long long)unknown);
            return -1;
        }
        if(rc==IFS_FS_END){ended=1;break;}
        for(uint32_t j=0u;j<n;++j) {
            const IfsFsAllocationRangeV2 *r=&ranges[j];
            const uint64_t end=r->physical_offset+r->length;
            if(r->state==IFS_FS_SPACE_FREE)free_bytes+=r->length;
            for(unsigned i=0u;i<LDTM_RAW_FILES;++i) {
                NativeGrowthGap *g=&gaps[i];
                if(!g->required)continue;
                if(r->physical_offset<g->end && end>=g->end) {
                    if(r->state!=IFS_FS_SPACE_ALLOCATED)return -1;
                    g->tail_owned=1;
                }
                if(!g->tail_owned || g->proved>=g->required || end<=g->end)continue;
                if(g->started) {
                    if(r->physical_offset!=g->start+g->proved || r->state!=IFS_FS_SPACE_RESERVED)
                        return -1;
                } else {
                    if(r->state!=IFS_FS_SPACE_RESERVED) {
                        if(end-g->end>=unit)return -1;
                        continue;
                    }
                    if(r->physical_offset<g->end || r->physical_offset-g->end>=unit)
                        return -1;
                    g->start=r->physical_offset;g->started=1;
                }
                const uint64_t need=g->required-g->proved;
                g->proved+=r->length<need?r->length:need;
            }
        }
        if(next<=at)return -1;
        at=next;
    }
    if(!ended || at!=capacity || free_bytes!=info->free_bytes) {
        fprintf(stderr,"%s: growth allocation totals disagree: scanned %llu/%llu bytes, mapped free %llu, volume free %llu, END %d\n",
                provider(m->spec),(unsigned long long)at,(unsigned long long)capacity,
                (unsigned long long)free_bytes,(unsigned long long)info->free_bytes,ended);
        return -1;
    }
    for(unsigned i=0u;i<LDTM_RAW_FILES;++i) {
        NativeGrowthGap *g=&gaps[i];
        if(!g->required)continue;
        if(!g->tail_owned || !g->started || g->proved!=g->required ||
           g->required>capacity-g->start) {
            fprintf(stderr,"%s: growth target %u tail %llu reserved %llu/%llu bytes (owned %d, started %d)\n",
                    provider(m->spec),i,(unsigned long long)g->end,
                    (unsigned long long)g->proved,(unsigned long long)g->required,g->tail_owned,g->started);
            return -1;
        }
        for(unsigned j=0u;j<i;++j)
            if(gaps[j].required && g->start<gaps[j].start+gaps[j].required &&
               gaps[j].start<g->start+g->required)return -1;
    }
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
        const uint32_t variant=m->profile.format_variant;
        /* A short portable label is used only if the provider accepts one.
         * GPT partition identity is independent of the filesystem label. */
        const char *label=(m->profile.flags&IFS_FS_FORMAT_ACCEPTS_LABEL)?"LDTM":NULL;
        stage="VOLUME.FORMAT";
        const int rc=volume->format(&runtime,variant,label,0u);
        if(rc!=IFS_FS_OK) {
            fprintf(stderr,"%s Common V2 FORMAT failed (variant %u, error %d)\n",
                    provider(m->spec),(unsigned)variant,rc);
            goto done;
        }
        {
            uint32_t detected=0u;
            stage="CORE.PROBE exact format variant";
            if(core->probe(&runtime,&detected)!=IFS_FS_OK || detected!=m->profile.probe_variant) {
                fprintf(stderr,"%s: expected variant %u, detected %u\n",
                        provider(m->spec),(unsigned)m->profile.probe_variant,(unsigned)detected);
                goto done;
            }
        }
    }
    if(no_namespace(m)) {
        uint32_t variant=0u;
        result=core->probe(&runtime,&variant)==IFS_FS_OK && variant==m->profile.probe_variant?0:-1;
        goto done;
    }
    stage="CORE.MOUNT";
    if(core->mount(&runtime,&session,&info)!=IFS_FS_OK || !session)goto done;
    mounted=1;
    stage="mounted allocation geometry";
    if(!profile_geometry_valid(m,&info))goto done;
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
        if(contiguous_layout(m)) {
            if(create_contiguous_corpus(m,core,session,info.block_bytes)!=0)goto done;
        } else if(blocked_growth(m)) {
            const IfsFsDefragOpsV2 *map=(const IfsFsDefragOpsV2 *)
                ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
            if(create_blocked_corpus(m,core,volume,map,session,&info,
                                  runtime.volume_bytes)!=0)goto done;
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
        if(!profile_geometry_valid(m,&info))goto done;
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
            if(contiguous_layout(m))goto done;
            continue;
        }
        if((seen&(1u<<(unsigned)number)) ||
           e.type!=IFS_FS_ENTRY_FILE ||
           e.size_bytes!=ldtm_raw_file_bytes(m->spec,(unsigned)number))
            goto done;
        seen|=1u<<(unsigned)number;
    }
    if(!directory_complete || seen!=((1u<<LDTM_RAW_FILES)-1u))goto done;
    if(blocked_growth(m)) {
        IfsFsEntryV2 temporary={.structure_bytes=sizeof(temporary),
            .name=entry_name,.name_capacity=sizeof(entry_name)};
        stage="blocked-growth sacrificial directory removal";
        if(core->stat(session,"/LDTM-TEMP",&temporary)!=IFS_FS_NOT_FOUND)
            goto done;
    }
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
            /* The eight target files establish the initial workload. After
             * relocation, every mapped file, including boundary-sized files,
             * must be contiguous; do not discard their fragmentation result. */
            if(m->require_contiguous && displaced) {
                fprintf(stderr,"%s: post-mutation file remains fragmented: %s\n",
                        provider(m->spec),path);
                goto done;
            }
            if(file<LDTM_TARGET_FILE_COUNT) {
                m->fragmented+=displaced;
                if(blocked_growth(m))
                    fprintf(stderr,"blocked-growth corpus %s: %s\n",path,
                            displaced?"physically fragmented":"contiguous");
            }
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
    if(m->contiguous_layout) {
        stage="contiguous-file independently verified allocation layout";
        const IfsFsDefragOpsV2 *map=(const IfsFsDefragOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
        if(m->fragmented || verify_contiguous_layout(m,core,volume,map,session,&info,runtime.volume_bytes))goto done;
    }
    if(m->verify_growth) {
        stage="canonical whole-volume 10% growth reserve verification";
        if(m->fragmented || verify_native_growth(m,api,session,&info,runtime.volume_bytes))
            goto done;
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
    if(!s || !path || !fragmented || ldtm_common_v2_layout(s)==IFS_FS_LAYOUT_NO_NAMESPACE)return -1;
    *fragmented=0u;
    MediaV2 m={.spec=s,.path=path,.require_map=1};
    const int rc=module_call(&m,execute);
    if(!rc)*fragmented=m.fragmented;
    return rc;
}

/* A fresh provider mount must prove all payloads, all file maps and native
 * integrity before an ordinary Defragment operation can qualify. */
int ldtm_common_v2_verify_contiguous(const LdtmFilesystemSpec *s,const char *path)
{
    if(!s || !path || ldtm_common_v2_layout(s)==IFS_FS_LAYOUT_NO_NAMESPACE)return -1;
    MediaV2 m={.spec=s,.path=path,.require_map=1,.require_contiguous=1};
    return module_call(&m,execute);
}

int ldtm_common_v2_verify_growth(const LdtmFilesystemSpec *s,const char *path)
{
    if(!s || !path || ldtm_common_v2_layout(s)==IFS_FS_LAYOUT_NO_NAMESPACE)return -1;
    MediaV2 m={.spec=s,.path=path,.require_map=1,.require_contiguous=1,.verify_growth=1};
    return module_call(&m,execute);
}
int ldtm_common_v2_verify_contiguous_layout(const LdtmFilesystemSpec *s,const char *path,int mode)
{
    if(ldtm_common_v2_layout(s)!=IFS_FS_LAYOUT_CONTIGUOUS_FILE || !path || mode<0 || mode>2)return -1;
    MediaV2 m={.spec=s,.path=path,.require_map=1,.require_contiguous=mode!=1,.contiguous_layout=mode+1};
    return module_call(&m,execute);
}
