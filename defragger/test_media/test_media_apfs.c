/* SPDX-License-Identifier: GPL-3.0-or-later
 * All APFS checkpoint, spaceman, catalog, extent and Test Media logic
 * is implemented inside trusted APFS.so. Only generic corpus callbacks live
 * in this application.
 */
#include "test_media.h"
#include "filesystem_module_host.h"
#include "ifs_apfs_module_v1.h"
#include "ifs_apfs_fixture_host_v1.h"
#include "ifs_defrag_host_services.h"
#include <string.h>
static const IfsApfsFixtureHostV1 corpus={
    IFS_APFS_FIXTURE_HOST_V1,sizeof(IfsApfsFixtureHostV1),
    ldtm_find_spec,ldtm_fragment_profile,
    ldtm_profile_file_chunks,ldtm_profile_file_bytes,ldtm_profile_payload_bytes
};
static const IfsApfsModuleV1 *inspect(void *symbol) {
    const IfsApfsModuleV1 *(*entry)(void)=NULL;
    _Static_assert(sizeof(entry)==sizeof(symbol),"module function pointer");
    memcpy(&entry,&symbol,sizeof(entry));
    const IfsApfsModuleV1 *api=entry ? entry() : NULL;
    return api && api->abi_version==IFS_APFS_MODULE_V1 &&
      api->structure_bytes>=sizeof(*api) && api->format_id &&
      strcmp(api->format_id,"apfs")==0 &&
      (api->capabilities & (IFS_APFS_CAP_RUN | IFS_APFS_CAP_FIXTURE)) ==
        (IFS_APFS_CAP_RUN | IFS_APFS_CAP_FIXTURE) &&
      api->run && api->fixture ? api : NULL;
}
static int ready(const IfsDefragHostV2 *host,void *symbol,void *unused) {
    (void)host;(void)unused;
    return inspect(symbol)?0:1;
}
int ldtm_apfs_module_available(void) {
    return ifs_filesystem_module_call("apfs","APFS.so",
      "infiltrator_filesystem_apfs_v1",ready,NULL)==0;
}
typedef struct {
    uint32_t operation;
    const char *path;
    char *detail;
    size_t capacity;
} Request;
static int call(const IfsDefragHostV2 *host,void *symbol,void *opaque) {
    (void)host;
    const IfsApfsModuleV1 *api=inspect(symbol);
    if(!api) return 1;
    const Request *r=opaque;
    return api->fixture(r->operation,r->path,NULL,r->detail,r->capacity,
                        ifs_defrag_services_v1(),&corpus);
}
static int invoke(uint32_t operation,const char *path,char *detail,size_t capacity) {
    if(!path) return -1;
    Request req={operation,path,detail,capacity};
    return ifs_filesystem_module_call("apfs","APFS.so",
      "infiltrator_filesystem_apfs_v1",call,&req);
}
int ldtm_format_apfs_volume(const char *path) {
    return invoke(IFS_APFS_FIXTURE_FORMAT,path,NULL,0);
}
int ldtm_verify_apfs_payload(const char *path,char *detail,size_t capacity) {
    return invoke(IFS_APFS_FIXTURE_VERIFY,path,detail,capacity);
}
int ldtm_verify_apfs_payload_after_defrag(const char *path,char *detail,size_t capacity) {
    return invoke(IFS_APFS_FIXTURE_VERIFY_DEFRAG,path,detail,capacity);
}
