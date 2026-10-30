/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real SFS.so formatter / population / integrity qualification.
 * No application-private SFS records or filesystem geometry are invented.
 */
#include "test_media.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr, "%s:%d: %s (errno=%d)\n",__FILE__,__LINE__,#x,errno); return 1; } } while(0)
int main(int argc,char **argv) {
    if(argc!=2) return 2;
    const LdtmFilesystemSpec *spec=ldtm_find_spec("sfs");
    CHECK(spec && spec->size_mib>0);
    CHECK(ldtm_sfs_module_available());
    const int fd=open(argv[1],O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC,0600);
    CHECK(fd>=0);
    CHECK(ftruncate(fd,(off_t)((uint64_t)spec->size_mib*LDTM_MIB))==0);
    CHECK(close(fd)==0);
    LdtmFragmentProfile profile=ldtm_fragment_profile(spec);
    char detail[512]={0};
    CHECK(ldtm_format_sfs_volume(argv[1])==0);
    CHECK(ldtm_populate_sfs_volume(argv[1],&profile)==0);
    CHECK(ldtm_verify_sfs_payload(argv[1],&profile,detail,sizeof(detail))==0);
    printf("%s\n",detail);
    CHECK(unlink(argv[1])==0);
    return 0;
}
