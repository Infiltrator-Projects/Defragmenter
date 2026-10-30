/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PFS3 Test Media uses the same PFS3.so as the Defrag/Recovery worker.
 * No local PFS3 bitmap, block, directory or payload layout implementation. */
#include "test_media.h"
#include "filesystem_module_host.h"
#include "ifs_pfs3_module_v1.h"
#include "ifs_pfs3_fixture_host_v1.h"
#include "ifs_defrag_host_services.h"
#include <string.h>
static const IfsPfs3FixtureHostV1 corpus={
 IFS_PFS3_FIXTURE_HOST_V1,sizeof(IfsPfs3FixtureHostV1),
 ldtm_find_spec,ldtm_fragment_profile,
 ldtm_profile_file_chunks,ldtm_profile_file_bytes,ldtm_profile_payload_bytes
};
static const IfsPfs3ModuleV1 *inspect(void *symbol) {
 const IfsPfs3ModuleV1 *(*entry)(void)=NULL;
 _Static_assert(sizeof(entry)==sizeof(symbol),"function pointer ABI");
 memcpy(&entry,&symbol,sizeof(entry));
 const IfsPfs3ModuleV1 *api=entry ? entry() : NULL;
 return api && api->abi_version==IFS_PFS3_MODULE_V1 &&
  api->structure_bytes>=sizeof(*api) && api->format_id &&
  strcmp(api->format_id,"pfs3")==0 &&
  (api->capabilities & (IFS_PFS3_CAP_RUN | IFS_PFS3_CAP_FIXTURE)) ==
    (IFS_PFS3_CAP_RUN | IFS_PFS3_CAP_FIXTURE) && api->run && api->fixture
  ? api : NULL;
}
static int available(const IfsDefragHostV2 *host,void *symbol,void *unused) {
 (void)host;(void)unused; return inspect(symbol)?0:1;
}
int ldtm_pfs3_module_available(void) {
 return ifs_filesystem_module_call("pfs3","PFS3.so",
     "infiltrator_filesystem_pfs3_v1",available,NULL)==0;
}
typedef struct {uint32_t op;const char *path;const void *profile;
 char *detail;size_t capacity;} Request;
static int execute(const IfsDefragHostV2 *host,void *symbol,void *opaque) {
 (void)host;
 const IfsPfs3ModuleV1 *api=inspect(symbol);
 if(!api) return 1;
 const Request *r=opaque;
 return api->fixture(r->op,r->path,r->profile,r->detail,r->capacity,
                      ifs_defrag_services_v1(),&corpus);
}
static int invoke(uint32_t op,const char *path,const void *profile,
                  char *detail,size_t size) {
 if(!path) return -1;
 Request r={op,path,profile,detail,size};
 return ifs_filesystem_module_call("pfs3","PFS3.so",
     "infiltrator_filesystem_pfs3_v1",execute,&r);
}
int ldtm_format_pfs3_volume(const char *path) {
 return invoke(IFS_PFS3_FIXTURE_FORMAT,path,NULL,NULL,0);
}
int ldtm_populate_pfs3_volume(const char *path,const LdtmFragmentProfile *p) {
 return p ? invoke(IFS_PFS3_FIXTURE_POPULATE,path,p,NULL,0) : -1;
}
int ldtm_verify_pfs3_payload(const char *path,const LdtmFragmentProfile *p,
 char *detail,size_t size) {
 return p ? invoke(IFS_PFS3_FIXTURE_VERIFY,path,p,detail,size) : -1;
}
int ldtm_verify_pfs3_payload_after_defrag(const char *path,const LdtmFragmentProfile *p,
 char *detail,size_t size) {
 return p ? invoke(IFS_PFS3_FIXTURE_VERIFY_DEFRAG,path,p,detail,size) : -1;
}
