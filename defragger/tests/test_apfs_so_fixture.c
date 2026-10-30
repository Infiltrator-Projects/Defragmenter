/* SPDX-License-Identifier: GPL-3.0-or-later
 * Prove the real APFS.so creates and independently verifies its corpus.
 * Does not interpret any APFS on-disk records inside Defragmenter.
 */
#include "test_media.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#define CHECK(expr) do { if(!(expr)) {fprintf(stderr,"%s:%d: %s (errno=%d)\n",__FILE__,__LINE__,#expr,errno); return 1;} } while(0)
int main(int argc,char **argv) {
 if(argc!=2) return 2;
 const LdtmFilesystemSpec *spec=ldtm_find_spec("apfs");
 CHECK(spec && spec->size_mib>0);
 CHECK(ldtm_apfs_module_available());
 int fd=open(argv[1],O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);
 CHECK(fd>=0);
 CHECK(ftruncate(fd,(off_t)((uint64_t)spec->size_mib*LDTM_MIB))==0);
 CHECK(close(fd)==0);
 char detail[512]={0};
 CHECK(ldtm_format_apfs_volume(argv[1])==0);
 CHECK(ldtm_verify_apfs_payload(argv[1],detail,sizeof(detail))==0);
 printf("%s\n",detail);
 CHECK(unlink(argv[1])==0);
 return 0;
}
