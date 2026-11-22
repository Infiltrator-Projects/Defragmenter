/* SPDX-License-Identifier: GPL-3.0-or-later
 * Format-neutral Common V2 worker. The individual filesystem owns every
 * on-disk operation, capability and read-only format probe.
 *
 * Reject a missing, duplicate or incompatible V2 provider. In particular
 * a module that exposes only an old private API is indistinguishable from a
 * missing provider: there is no fallback loader or replacement operation.
 */
#ifndef IFS_DEFRAGMENTER_COMMON_V2_WORKER_H
#define IFS_DEFRAGMENTER_COMMON_V2_WORKER_H
#include "filesystem_module_host.h"
#include "version.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>

static int ifs_v2_worker_dispatch(int argc,char **argv,
                                  const char *const *formats,size_t count)
{
    if(argc==2 && argv && argv[1] &&
       !strcmp(argv[1],"--version")) {
        puts(LD_VERSION);
        return 0;
    }
    if(argc<3 || !argv || !argv[0] || !argv[1] || !argv[2] ||
       !*argv[2] || !formats || !count)return 2;

    const char *selected=NULL;
    char filename[64]={0};
    unsigned matches=0u;
    for(size_t i=0u;i<count;++i) {
        char candidate[64]={0};
        if(!formats[i] || ifs_filesystem_module_find_readonly_v2(
            formats[i],candidate,sizeof(candidate))!=0)
            continue;
        char *probe[]={argv[0],(char *)"probe-only",argv[2],NULL};
        if(ifs_filesystem_module_run(formats[i],candidate,3,probe)!=0)
            continue;
        if(++matches!=1u) {
            fputs("Ambiguous Common V2 filesystem provider; refusing operation\n",
                  stderr);
            return 1;
        }
        selected=formats[i];
        memcpy(filename,candidate,strlen(candidate)+1u);
    }
    if(matches!=1u) {
        fputs("No compatible Common V2 provider recognises this filesystem\n",
              stderr);
        return 1;
    }
    if(!strcmp(argv[1],"identify")) {
        if(argc!=3)return 2;
        printf("{\"filesystem\":\"%s\"}\n",selected);
        return 0;
    }
    return ifs_filesystem_module_run(selected,filename,argc,argv);
}
#endif
