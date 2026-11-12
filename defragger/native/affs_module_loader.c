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
    /* Native strict Common V2 identification for read-only requests.
     * An explicit per-file --path map is NOT the legacy --cells allocation
     * grid; do not parse the Amiga DOS boot bytes to select that provider.
     * Both independent SOs perform their own checked CORE probe. Reject
     * absence or ambiguous matches without calling any V1 fallback. */
    int file_map=0;
    if(!strcmp(argv[1],"map")) {
        for(int i=3;i<argc;++i) {
            if(!argv[i])return 2;
            if(!strcmp(argv[i],"--path") ||
               !strncmp(argv[i],"--path=",7u)) {
                file_map=1;
                break;
            }
        }
    }
    if(file_map || !strcmp(argv[1],"volume-info")) {
        const char *const formats[]={"ofs","ffs"};
        char selected_format[16]={0};
        char selected_name[64]={0};
        unsigned matches=0u;
        for(unsigned i=0u;i<2u;++i) {
            char candidate[64]={0};
            if(ifs_filesystem_module_find_readonly_v2(
                    formats[i],candidate,sizeof(candidate))!=0)
                continue;
            char *probe[]={(char *)argv[0],(char *)"probe-only",argv[2],NULL};
            if(ifs_filesystem_module_run(formats[i],candidate,3,probe)!=0)
                continue;
            if(++matches!=1u) {
                fputs("Multiple trusted Amiga Common V2 providers matched the target\n",stderr);
                return 1;
            }
            memcpy(selected_format,formats[i],strlen(formats[i])+1u);
            memcpy(selected_name,candidate,strlen(candidate)+1u);
        }
        if(matches!=1u) {
            fputs("No trusted Amiga Common V2 provider recognised the target\n",stderr);
            return 1;
        }
        return ifs_filesystem_module_run(selected_format,selected_name,
                                         argc,argv);
    }
    /* A recovery journal takes precedence over a possibly damaged boot
     * signature. All remaining FFS identity checks occur inside FFS.so. */
    if(recover && ifs_filesystem_journal_matches(journal,"ofs"))
        return ifs_filesystem_module_run_ofs_native(argc, argv);
    unsigned char signature[4]={0};
    int fd=open(argv[2],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd>=0) {
        if(pread(fd,signature,sizeof(signature),0)!=(ssize_t)sizeof(signature))
            memset(signature,0,sizeof(signature));
        close(fd);
    }
    if(signature[0]=='D'&&signature[1]=='O'&&signature[2]=='S'&&
       signature[3]<=6U && (signature[3]&1U)==0U)
        return ifs_filesystem_module_run_ofs_native(argc, argv);
    if(!recover && !(signature[0]=='D'&&signature[1]=='O'&&
       signature[2]=='S'&&(signature[3]==1U||signature[3]==3U||signature[3]==5U))) {
        fputs("No supported OFS/FFS filesystem signature\n",stderr);
        return 1;
    }
    Request r={argc,argv};
    return ifs_filesystem_module_call("ffs","FFS.so",
       "infiltrator_filesystem_ffs_defrag_v1",run_ffs,&r);
}
