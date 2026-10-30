/* SPDX-License-Identifier: GPL-3.0-or-later
 * Only the generic deterministic corpus host remains in Test Media.
 * Actual ZFS label/MOS/space-map formatting and verification execute
 * exclusively inside ZFS.so's versioned fixture entrypoint.
 */
#include "test_media_raw.h"
#include "filesystem_module_host.h"
#include "ifs_zfs_module_v1.h"
#include "ifs_zfs_fixture_v1.h"
#include <string.h>
static const IfsZfsFixtureHostV1 fixture_host = {
    IFS_ZFS_FIXTURE_V1, sizeof(IfsZfsFixtureHostV1),
    ldtm_raw_file_bytes, ldtm_raw_name, ldtm_raw_open,
    ldtm_raw_read, ldtm_raw_write, ldtm_raw_plan,
    ldtm_raw_write_payload, ldtm_raw_check_init, ldtm_raw_check_file,
    ldtm_raw_check_data, ldtm_raw_check_finish
};
static const IfsZfsModuleV1 *inspect(void *symbol) {
    const IfsZfsModuleV1 *(*entry)(void) = NULL;
    _Static_assert(sizeof(entry)==sizeof(symbol), "module symbol pointer mismatch");
    memcpy(&entry,&symbol,sizeof(entry));
    const IfsZfsModuleV1 *api=entry ? entry() : NULL;
    return api && api->abi_version == IFS_ZFS_MODULE_V1 &&
        api->structure_bytes >= sizeof(*api) && api->format_id &&
        strcmp(api->format_id,"zfs")==0 &&
        (api->capabilities & (IFS_ZFS_CAP_RUN | IFS_ZFS_CAP_FIXTURE)) ==
           (IFS_ZFS_CAP_RUN | IFS_ZFS_CAP_FIXTURE) &&
        api->run && api->fixture ? api : NULL;
}
static int ready(const IfsDefragHostV2 *host,void *symbol,void *data) {
    (void)host; (void)data;
    return inspect(symbol) ? 0 : 1;
}
int ldtm_zfs_module_available(const LdtmFilesystemSpec *s) {
    return s && s->creator == LDTM_CREATOR_ZFS &&
        ifs_filesystem_module_call("zfs","ZFS.so",
            "infiltrator_filesystem_zfs_v1",ready,NULL)==0;
}
typedef struct {const LdtmFilesystemSpec *spec;const char *path;bool create;} Fixture;
static int execute(const IfsDefragHostV2 *host,void *symbol,void *arg) {
    (void)host;
    const IfsZfsModuleV1 *api=inspect(symbol);
    if(!api) return 1;
    const Fixture *f=arg;
    return api->fixture(f->spec,f->path,f->create?1:0,&fixture_host);
}
int ldtm_raw_zfs(const LdtmFilesystemSpec *s,const char *path,bool create) {
    if(!s || !path || s->creator != LDTM_CREATOR_ZFS) return -1;
    Fixture f={s,path,create};
    return ifs_filesystem_module_call("zfs","ZFS.so",
               "infiltrator_filesystem_zfs_v1",execute,&f);
}
