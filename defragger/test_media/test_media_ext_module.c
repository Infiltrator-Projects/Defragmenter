/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host only supplies corpus and generic media I/O.
 * On-disk EXT2/3/4 fixture logic lives inside EXT2.so/EXT3.so/EXT4.so.
 */
#include "test_media_raw.h"
#include "filesystem_module_host.h"
#include "ifs_ext_module_v1.h"
#include "ifs_ext_fixture_v1.h"
#include "ifs_defrag_host_services.h"
#include <string.h>
static const IfsExtFixtureHostV1 corpus={
 IFS_EXT_FIXTURE_V1,sizeof(IfsExtFixtureHostV1),
 ldtm_raw_file_bytes,ldtm_raw_name,ldtm_raw_open,
 ldtm_raw_read,ldtm_raw_write,ldtm_raw_plan,ldtm_raw_write_payload,
 ldtm_raw_check_init,ldtm_raw_check_file,ldtm_raw_check_data,
 ldtm_raw_check_finish,ldtm_raw_crc32c
};
static const char *kind(const LdtmFilesystemSpec *spec) {
 if(!spec)return NULL;
 switch(spec->creator) {
 case LDTM_CREATOR_EXT2:return "ext2";
 case LDTM_CREATOR_EXT3:return "ext3";
 case LDTM_CREATOR_EXT4:return "ext4";
 default:return NULL;
 }
}
static const char *filename(const char *k) {
 return strcmp(k,"ext2")==0 ? "EXT2.so" :
        strcmp(k,"ext3")==0 ? "EXT3.so" : "EXT4.so";
}
static const IfsExtModuleV1 *inspect(void *symbol,const char *k) {
 const IfsExtModuleV1 *(*entry)(void)=NULL;
 _Static_assert(sizeof(entry)==sizeof(symbol),"module pointer width mismatch");
 memcpy(&entry,&symbol,sizeof(entry));
 const IfsExtModuleV1 *api=entry ? entry() : NULL;
 return api && api->abi_version==IFS_EXT_MODULE_V1 &&
   api->structure_bytes>=sizeof(*api) && api->format_id &&
   strcmp(api->format_id,k)==0 &&
   (api->capabilities&(IFS_EXT_CAP_RUN|IFS_EXT_CAP_FIXTURE))==
   (IFS_EXT_CAP_RUN|IFS_EXT_CAP_FIXTURE) && api->run && api->fixture
    ? api : NULL;
}
typedef struct {const LdtmFilesystemSpec *spec;const char *path;bool create;int availability;} Request;
static int invoke(const IfsDefragHostV2 *host,void *symbol,void *opaque) {
 (void)host;
 Request *r=opaque;
 const IfsExtModuleV1 *api=inspect(symbol,kind(r->spec));
 if(!api)return 1;
 if(r->availability)return 0;
 return api->fixture(r->spec,r->path,r->create?1:0,
                     ifs_defrag_services_v1(),&corpus);
}
int ldtm_ext_module_available(const LdtmFilesystemSpec *spec) {
 const char *k=kind(spec);
 if(!k)return 0;
 Request r={spec,NULL,false,1};
 return ifs_filesystem_module_call(k,filename(k),
        "infiltrator_filesystem_ext_v1",invoke,&r)==0;
}
int ldtm_raw_ext(const LdtmFilesystemSpec *spec,const char *path,bool create) {
 const char *k=kind(spec);
 if(!k || !path)return -1;
 Request r={spec,path,create,0};
 return ifs_filesystem_module_call(k,filename(k),
        "infiltrator_filesystem_ext_v1",invoke,&r);
}
