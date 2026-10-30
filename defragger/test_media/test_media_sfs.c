/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SFS Test Media uses the same SFS.so as the Defrag/Recovery worker.
 * No local SFS bitmap, block, directory or payload layout implementation. */
#include "test_media.h"
#include "filesystem_module_host.h"
#include "ifs_sfs_module_v1.h"
#include "ifs_sfs_fixture_host_v1.h"
#include "ifs_defrag_host_services.h"
#include <string.h>
static const IfsSfsFixtureHostV1 corpus={
 IFS_SFS_FIXTURE_HOST_V1,sizeof(IfsSfsFixtureHostV1),
 ldtm_find_spec,ldtm_fragment_profile,
 ldtm_profile_file_chunks,ldtm_profile_file_bytes,ldtm_profile_payload_bytes
};
static const IfsSfsModuleV1 *inspect(void *symbol) {
 const IfsSfsModuleV1 *(*entry)(void)=NULL;
 _Static_assert(sizeof(entry)==sizeof(symbol),"function pointer ABI");
 memcpy(&entry,&symbol,sizeof(entry));
 const IfsSfsModuleV1 *api=entry ? entry() : NULL;
 return api && api->abi_version==IFS_SFS_MODULE_V1 &&
  api->structure_bytes>=sizeof(*api) && api->format_id &&
  strcmp(api->format_id,"sfs")==0 &&
  (api->capabilities & (IFS_SFS_CAP_RUN | IFS_SFS_CAP_FIXTURE)) ==
    (IFS_SFS_CAP_RUN | IFS_SFS_CAP_FIXTURE) && api->run && api->fixture
  ? api : NULL;
}
static int available(const IfsDefragHostV2 *host,void *symbol,void *unused) {
 (void)host;(void)unused; return inspect(symbol)?0:1;
}
int ldtm_sfs_module_available(void) {
 return ifs_filesystem_module_call("sfs","SFS.so",
     "infiltrator_filesystem_sfs_v1",available,NULL)==0;
}
typedef struct {uint32_t op;const char *path;const void *profile;
 char *detail;size_t capacity;} Request;
static int execute(const IfsDefragHostV2 *host,void *symbol,void *opaque) {
 (void)host;
 const IfsSfsModuleV1 *api=inspect(symbol);
 if(!api) return 1;
 const Request *r=opaque;
 return api->fixture(r->op,r->path,r->profile,r->detail,r->capacity,
                      ifs_defrag_services_v1(),&corpus);
}
static int invoke(uint32_t op,const char *path,const void *profile,
                  char *detail,size_t size) {
 if(!path) return -1;
 Request r={op,path,profile,detail,size};
 return ifs_filesystem_module_call("sfs","SFS.so",
     "infiltrator_filesystem_sfs_v1",execute,&r);
}
int ldtm_format_sfs_volume(const char *path) {
 return invoke(IFS_SFS_FIXTURE_FORMAT,path,NULL,NULL,0);
}
int ldtm_populate_sfs_volume(const char *path,const LdtmFragmentProfile *p) {
 return p ? invoke(IFS_SFS_FIXTURE_POPULATE,path,p,NULL,0) : -1;
}
int ldtm_verify_sfs_payload(const char *path,const LdtmFragmentProfile *p,
 char *detail,size_t size) {
 return p ? invoke(IFS_SFS_FIXTURE_VERIFY,path,p,detail,size) : -1;
}
int ldtm_verify_sfs_payload_after_defrag(const char *path,const LdtmFragmentProfile *p,
 char *detail,size_t size) {
 return p ? invoke(IFS_SFS_FIXTURE_VERIFY_DEFRAG,path,p,detail,size) : -1;
}
