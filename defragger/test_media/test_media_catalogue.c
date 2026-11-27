/* SPDX-License-Identifier: GPL-3.0-or-later
 * Production Test Media discovers only the trusted Common V2 SO inventory.
 * An immutable process-local snapshot is sorted by exported format ID.
 * The privileged worker independently reconstructs it, and rejects a
 * differing GUI-confirmed layout before any destructive operation.
 */
#include "test_media.h"
#include "filesystem_module_host.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>

static LdtmFilesystemSpec specs[LDTM_SPEC_LIMIT];
static char ids[LDTM_SPEC_LIMIT][IFS_FS_MAX_FORMAT_ID+1u];
static char labels[LDTM_SPEC_LIMIT][37]; /* GPT name: 36 UTF-16 code units */
static size_t count;
static pthread_once_t once=PTHREAD_ONCE_INIT;

static int profile_call(const IfsDefragHostV2 *unused,void *symbol,void *arg)
{
    (void)unused;
    IfsFsModuleApiEntryV2 entry=NULL;
    _Static_assert(sizeof(entry)==sizeof(symbol),"Common V2 entry width");
    memcpy(&entry,&symbol,sizeof entry);
    const IfsFsModuleApiV2 *api=entry?entry():NULL;
    if(!api||!ifs_fs_api_v2_valid(api,api->format_id)||
       !(api->runtime_profiles&IFS_FS_PROFILE_HOSTED))return -1;
    const IfsFsFormatProfileOpsV2 *ops=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_FORMAT_PROFILE);
    IfsFsFormatProfileV2 *p=arg;
    return ops&&(ops->header.capabilities&IFS_FS_FORMAT_PROFILE_DEFAULT)&&
        ops->get_default&&ops->get_default(p)==IFS_FS_OK&&
        ifs_fs_format_profile_v2_valid(p)?0:-1;
}
static int label_for(const char *id,char out[37])
{
    size_t n=strlen(id);
    if(n<=33u){
        memcpy(out,"LD_",3);
        for(size_t i=0;i<n;i++)out[i+3u]=id[i]>='a'&&id[i]<='z'?(char)(id[i]-'a'+'A'):id[i];
        out[n+3u]=0;return 0;
    }
    /* Long valid IDs must not truncate into colliding GPT labels. */
    unsigned char hash[32];unsigned bytes=0;
    if(EVP_Digest(id,n,hash,&bytes,EVP_sha256(),NULL)!=1||bytes!=sizeof hash)return -1;
    static const char hex[]="0123456789ABCDEF";
    memcpy(out,"LD_H",4);
    for(unsigned i=0;i<16;i++){out[4u+2u*i]=hex[hash[i]>>4];out[5u+2u*i]=hex[hash[i]&15u];}
    out[36]=0;return 0;
}
static int collect(void *unused,const char *id,uint64_t caps,uint64_t groups)
{
    (void)unused;(void)caps;(void)groups;
    if(!ifs_fs_format_id_valid(id)||count==LDTM_SPEC_LIMIT)return -1;
    for(size_t i=0;i<count;i++)if(!strcmp(specs[i].key,id))return -1;
    char filename[64];
    if(ifs_filesystem_module_find_readonly_v2(id,filename,sizeof filename)!=0)return -1;
    size_t i=count;
    memcpy(ids[i],id,strlen(id)+1u);
    if(label_for(id,labels[i]))return -1;
    for(size_t j=0;j<count;j++)if(!strcmp(specs[j].label,labels[i]))return -1;
    LdtmFilesystemSpec s={.key=ids[i],.label=labels[i],.creator=LDTM_CREATOR_MANUAL,
        .note="Installed provider has no usable Common V2 format profile"};
    IfsFsFormatProfileV2 p={.structure_bytes=sizeof p};
    if(ifs_filesystem_module_call(id,filename,IFS_FS_COMMON_EXPORT_V2,profile_call,&p)==0){
        /* Fixed application workload, variable provider geometry. Prefer 2
         * GiB, but obey the provider's minimum, recommendation and maximum.
         * MiB-aligned GPT slots must fit the exact advertised geometry. */
        uint64_t min=p.minimum_bytes/LDTM_MIB+(p.minimum_bytes%LDTM_MIB!=0);
        uint64_t rec=p.recommended_bytes/LDTM_MIB+(p.recommended_bytes%LDTM_MIB!=0);
        uint64_t max=p.maximum_bytes?p.maximum_bytes/LDTM_MIB:UINT32_MAX;
        uint64_t size=rec>2048u?rec:2048u;
        if(size>max)size=max;
        if(size<min)size=min;
        if(size&&size<=max&&size<=UINT32_MAX&&
           (!p.exact_block_bytes||!(size*LDTM_MIB%p.exact_block_bytes))){
            s.size_mib=(uint32_t)size;
            s.payload_mib=p.layout==IFS_FS_LAYOUT_NO_NAMESPACE?0u:200u;
            s.creator=p.layout==IFS_FS_LAYOUT_NO_NAMESPACE?LDTM_CREATOR_NO_NAMESPACE:LDTM_CREATOR_GENERIC_V2;
            s.note="Geometry and namespace from installed Common V2 format profile";
        }else s.note="Provider geometry cannot be represented by MiB-aligned GPT test slots";
    }
    specs[count++]=s;
    return 0;
}
static int compare(const void *a,const void *b)
{return strcmp(((const LdtmFilesystemSpec*)a)->key,((const LdtmFilesystemSpec*)b)->key);}
static void discover(void)
{
    /* Duplicate ownership, overflow or inaccessible authority invalidates
     * the entire snapshot instead of silently constructing a partial disk. */
    if(ifs_filesystem_module_visit_v2(collect,NULL)!=0){count=0;return;}
    qsort(specs,count,sizeof specs[0],compare);
}
const LdtmFilesystemSpec *ldtm_specs(void)
{(void)pthread_once(&once,discover);return specs;}
size_t ldtm_spec_count(void)
{(void)pthread_once(&once,discover);return count;}
