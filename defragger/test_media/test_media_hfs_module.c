/* SPDX-License-Identifier: GPL-3.0-or-later
 * HFS+/HFSX format/fixture logic lives exclusively in trusted HFS.so.
 * The application provides only filesystem-neutral media/corpus callbacks.
 */
#include "test_media_raw.h"
#include "filesystem_module_host.h"
#include "ifs_hfs_module_v1.h"
#include "ifs_hfs_fixture_v1.h"
#include "ifs_defrag_host_services.h"
#include <string.h>
static const IfsHfsFixtureHostV1 corpus={
 IFS_HFS_FIXTURE_V1,sizeof(IfsHfsFixtureHostV1),
 ldtm_raw_file_bytes,ldtm_raw_name,ldtm_raw_open,
 ldtm_raw_read,ldtm_raw_write,ldtm_raw_plan,ldtm_raw_write_payload,
 ldtm_raw_check_init,ldtm_raw_check_file,ldtm_raw_check_data,
 ldtm_raw_check_finish
};
static const IfsHfsModuleV1 *inspect(void *symbol) {
 const IfsHfsModuleV1 *(*entry)(void)=NULL;
 _Static_assert(sizeof(entry)==sizeof(symbol),"module pointer width");
 memcpy(&entry,&symbol,sizeof(entry));
 const IfsHfsModuleV1 *api=entry ? entry() : NULL;
 return api && api->abi_version==IFS_HFS_MODULE_V1 &&
  api->structure_bytes>=sizeof(*api) && api->format_id &&
  strcmp(api->format_id,"hfs")==0 &&
  (api->capabilities & (IFS_HFS_CAP_RUN | IFS_HFS_CAP_FIXTURE)) ==
    (IFS_HFS_CAP_RUN | IFS_HFS_CAP_FIXTURE) &&
  api->run && api->fixture ? api : NULL;
}
static int available(const IfsDefragHostV2 *host,void *symbol,void *unused) {
 (void)host;(void)unused;return inspect(symbol)?0:1;
}
int ldtm_hfs_module_available(const LdtmFilesystemSpec *spec) {
 return spec && spec->creator==LDTM_CREATOR_HFS &&
  ifs_filesystem_module_call("hfs","HFS.so",
    "infiltrator_filesystem_hfs_v1",available,NULL)==0;
}
typedef struct {const LdtmFilesystemSpec *spec;const char *path;bool create;} Request;
static int invoke(const IfsDefragHostV2 *host,void *symbol,void *opaque) {
 (void)host;
 const IfsHfsModuleV1 *api=inspect(symbol);
 if(!api)return 1;
 const Request *r=opaque;
 return api->fixture(r->spec,r->path,r->create?1:0,
    ifs_defrag_services_v1(),&corpus);
}
int ldtm_hfs_module_fixture(const LdtmFilesystemSpec *spec,
                                const char *path,bool create) {
 if(!spec || spec->creator!=LDTM_CREATOR_HFS || !path)return -1;
 Request r={spec,path,create};
 return ifs_filesystem_module_call("hfs","HFS.so",
    "infiltrator_filesystem_hfs_v1",invoke,&r);
}

int ldtm_raw_mac(const LdtmFilesystemSpec *spec,const char *path,bool create) {
 if(!spec)return -1;
 if(spec->creator==LDTM_CREATOR_HFSPLUS)
   return ldtm_hfsplus_module_fixture(spec,path,create);
 return ldtm_hfs_module_fixture(spec,path,create);
}
