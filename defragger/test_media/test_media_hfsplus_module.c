/* SPDX-License-Identifier: GPL-3.0-or-later
 * HFS+/HFSX format/fixture logic lives exclusively in trusted HFSPLUS.so.
 * The application provides only filesystem-neutral media/corpus callbacks.
 */
#include "test_media_raw.h"
#include "filesystem_module_host.h"
#include "ifs_hfsplus_module_v1.h"
#include "ifs_hfsplus_fixture_v1.h"
#include "ifs_defrag_host_services.h"
#include <string.h>
static const IfsHfsplusFixtureHostV1 corpus={
 IFS_HFSPLUS_FIXTURE_V1,sizeof(IfsHfsplusFixtureHostV1),
 ldtm_raw_file_bytes,ldtm_raw_name,ldtm_raw_open,
 ldtm_raw_read,ldtm_raw_write,ldtm_raw_plan,ldtm_raw_write_payload,
 ldtm_raw_check_init,ldtm_raw_check_file,ldtm_raw_check_data,
 ldtm_raw_check_finish
};
static const IfsHfsplusModuleV1 *inspect(void *symbol) {
 const IfsHfsplusModuleV1 *(*entry)(void)=NULL;
 _Static_assert(sizeof(entry)==sizeof(symbol),"module pointer width");
 memcpy(&entry,&symbol,sizeof(entry));
 const IfsHfsplusModuleV1 *api=entry ? entry() : NULL;
 return api && api->abi_version==IFS_HFSPLUS_MODULE_V1 &&
  api->structure_bytes>=sizeof(*api) && api->format_id &&
  strcmp(api->format_id,"hfsplus")==0 &&
  (api->capabilities & (IFS_HFSPLUS_CAP_RUN | IFS_HFSPLUS_CAP_FIXTURE)) ==
    (IFS_HFSPLUS_CAP_RUN | IFS_HFSPLUS_CAP_FIXTURE) &&
  api->run && api->fixture ? api : NULL;
}
static int available(const IfsDefragHostV2 *host,void *symbol,void *unused) {
 (void)host;(void)unused;return inspect(symbol)?0:1;
}
int ldtm_hfsplus_module_available(const LdtmFilesystemSpec *spec) {
 return spec && spec->creator==LDTM_CREATOR_HFSPLUS &&
  ifs_filesystem_module_call("hfsplus","HFSPLUS.so",
    "infiltrator_filesystem_hfsplus_v1",available,NULL)==0;
}
typedef struct {const LdtmFilesystemSpec *spec;const char *path;bool create;} Request;
static int invoke(const IfsDefragHostV2 *host,void *symbol,void *opaque) {
 (void)host;
 const IfsHfsplusModuleV1 *api=inspect(symbol);
 if(!api)return 1;
 const Request *r=opaque;
 return api->fixture(r->spec,r->path,r->create?1:0,
    ifs_defrag_services_v1(),&corpus);
}
int ldtm_hfsplus_module_fixture(const LdtmFilesystemSpec *spec,
                                const char *path,bool create) {
 if(!spec || spec->creator!=LDTM_CREATOR_HFSPLUS || !path)return -1;
 Request r={spec,path,create};
 return ifs_filesystem_module_call("hfsplus","HFSPLUS.so",
    "infiltrator_filesystem_hfsplus_v1",invoke,&r);
}
