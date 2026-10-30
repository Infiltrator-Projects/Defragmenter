/* SPDX-License-Identifier: GPL-3.0-or-later
 * Amiga filesystem dispatch only: all on-disk algorithms and journals
 * are exclusively provided by OFS.so or FFS.so.
 */
#include "filesystem_module_host.h"
#include "ifs_ffs_defrag_v1.h"
#include "ifs_defrag_host_services.h"
#include "version.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
typedef struct {int argc;char **argv;} Request;
static int run_ffs(const IfsDefragHostV2 *host,void *symbol,void *user) {
    (void)host;
    const IfsFfsDefragV1 *(*entry)(void)=NULL;
    _Static_assert(sizeof(entry)==sizeof(symbol),"function-pointer ABI");
    memcpy(&entry,&symbol,sizeof(entry));
    const IfsFfsDefragV1 *api=entry ? entry() : NULL;
    if(!api || api->abi_version!=IFS_FFS_DEFRAG_ABI_V1 ||
       api->structure_bytes<sizeof(*api) || !api->format_id ||
       strcmp(api->format_id,"ffs")!=0 ||
       (api->capabilities & IFS_FFS_DEFRAG_CAP_RUN)!=IFS_FFS_DEFRAG_CAP_RUN ||
       !api->run) {
        fputs("FFS.so: missing native defragmentation interface\n",stderr);
        return 1;
    }
    const Request *request=user;
    return api->run(request->argc,request->argv,ifs_defrag_services_v1());
}
int main(int argc,char **argv) {
    if(argc==2 && strcmp(argv[1],"--version")==0) {puts(LD_VERSION);return 0;}
    if(argc<3 || !argv[2]) {fputs("Usage: affs-worker MODE DEVICE [OPTIONS]\n",stderr);return 2;}
    const int recover=strcmp(argv[1],"recover")==0;
    const char *journal=NULL;
    for(int i=3;i+1<argc;i++)
        if(strcmp(argv[i],"--journal")==0)journal=argv[i+1];
    /* A recovery journal takes precedence over a possibly damaged boot
     * signature. All remaining FFS identity checks occur inside FFS.so. */
    if(recover && ifs_filesystem_journal_matches(journal,"ofs"))
        return ifs_filesystem_module_run("ofs","OFS.so",argc,argv);
    unsigned char signature[4]={0};
    int fd=open(argv[2],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd>=0) {
        if(pread(fd,signature,sizeof(signature),0)!=(ssize_t)sizeof(signature))
            memset(signature,0,sizeof(signature));
        close(fd);
    }
    if(signature[0]=='D'&&signature[1]=='O'&&signature[2]=='S'&&
       signature[3]<=6U && (signature[3]&1U)==0U)
        return ifs_filesystem_module_run("ofs","OFS.so",argc,argv);
    if(!recover && !(signature[0]=='D'&&signature[1]=='O'&&
       signature[2]=='S'&&(signature[3]==1U||signature[3]==3U||signature[3]==5U))) {
        fputs("No supported OFS/FFS filesystem signature\n",stderr);
        return 1;
    }
    Request r={argc,argv};
    return ifs_filesystem_module_call("ffs","FFS.so",
       "infiltrator_filesystem_ffs_defrag_v1",run_ffs,&r);
}
