/* SPDX-License-Identifier: GPL-3.0-or-later
 * HFS+/HFSX format/fixture logic lives exclusively in trusted XFS.so.
 * The application provides only filesystem-neutral media/corpus callbacks.
 */
#include "test_media_raw.h"
#include "filesystem_module_host.h"
#include "ifs_xfs_module_v1.h"
#include "ifs_xfs_fixture_v1.h"
#include "ifs_defrag_host_services.h"
#include <string.h>
static const IfsXfsFixtureHostV1 corpus={
 IFS_XFS_FIXTURE_V1,sizeof(IfsXfsFixtureHostV1),
 ldtm_raw_file_bytes,ldtm_raw_name,ldtm_raw_open,
 ldtm_raw_read,ldtm_raw_write,ldtm_raw_plan,ldtm_raw_write_payload,
 ldtm_raw_check_init,ldtm_raw_check_file,ldtm_raw_check_data,
 ldtm_raw_check_finish,ldtm_raw_crc32c
};
static const IfsXfsModuleV1 *inspect(void *symbol) {
 const IfsXfsModuleV1 *(*entry)(void)=NULL;
 _Static_assert(sizeof(entry)==sizeof(symbol),"module pointer width");
 memcpy(&entry,&symbol,sizeof(entry));
 const IfsXfsModuleV1 *api=entry ? entry() : NULL;
 return api && api->abi_version==IFS_XFS_MODULE_V1 &&
  api->structure_bytes>=sizeof(*api) && api->format_id &&
  strcmp(api->format_id,"xfs")==0 &&
  (api->capabilities & (IFS_XFS_CAP_RUN | IFS_XFS_CAP_FIXTURE)) ==
    (IFS_XFS_CAP_RUN | IFS_XFS_CAP_FIXTURE) &&
  api->run && api->fixture ? api : NULL;
}
static int available(const IfsDefragHostV2 *host,void *symbol,void *unused) {
 (void)host;(void)unused;return inspect(symbol)?0:1;
}
int ldtm_xfs_module_available(const LdtmFilesystemSpec *spec) {
 return spec && spec->creator==LDTM_CREATOR_XFS &&
  ifs_filesystem_module_call("xfs","XFS.so",
    "infiltrator_filesystem_xfs_v1",available,NULL)==0;
}
typedef struct {const LdtmFilesystemSpec *spec;const char *path;bool create;} Request;
static int invoke(const IfsDefragHostV2 *host,void *symbol,void *opaque) {
 (void)host;
 const IfsXfsModuleV1 *api=inspect(symbol);
 if(!api)return 1;
 const Request *r=opaque;
 return api->fixture(r->spec,r->path,r->create?1:0,
    ifs_defrag_services_v1(),&corpus);
}
int ldtm_raw_xfs(const LdtmFilesystemSpec *spec,
                                const char *path,bool create) {
 if(!spec || spec->creator!=LDTM_CREATOR_XFS || !path)return -1;
 Request r={spec,path,create};
 return ifs_filesystem_module_call("xfs","XFS.so",
    "infiltrator_filesystem_xfs_v1",invoke,&r);
}
