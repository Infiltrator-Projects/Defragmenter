/* SPDX-License-Identifier: GPL-3.0-or-later
 * Test the production catalogue implementation, not the fixed CI corpus. */
#include "test_media.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"catalogue line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(int argc,char **argv)
{
    CHECK(argc>=2);
    size_t n=(size_t)strtoul(argv[1],NULL,10);
    CHECK(ldtm_spec_count()==n);
    char script[16384],digest[65],detail[256];
    if(!n){
        CHECK(ldtm_catalogue_fingerprint(digest)!=0);
        CHECK(ldtm_build_sfdisk_script(script,sizeof script)!=0);
        CHECK(!ldtm_required_modules_available(detail,sizeof detail));
        return 0;
    }
    const LdtmFilesystemSpec *s=ldtm_find_spec("future-native-provider");
    CHECK(s&&s->size_mib==512u&&s->payload_mib==0u&&s->creator==LDTM_CREATOR_NO_NAMESPACE);
    CHECK(!strcmp(s->label,"LD_FUTURE-NATIVE-PROVIDER"));
    CHECK(!ldtm_find_spec("fat12"));
    CHECK(ldtm_catalogue_fingerprint(digest)==0&&strlen(digest)==64u);
    if(argc>2&&!strcmp(argv[2],"manual")){
        const LdtmFilesystemSpec *manual=ldtm_find_spec("testv2");
        CHECK(manual&&manual->creator==LDTM_CREATOR_MANUAL);
        CHECK(!ldtm_required_modules_available(detail,sizeof detail));
        CHECK(ldtm_build_sfdisk_script(script,sizeof script)!=0);
    }else{
        CHECK(ldtm_required_modules_available(detail,sizeof detail));
        CHECK(ldtm_build_sfdisk_script(script,sizeof script)==0);
        CHECK(strstr(script,"size=512MiB, type=linux, name=\"LD_FUTURE-NATIVE-PROVIDER\""));
        CHECK(ldtm_allocated_capacity_bytes()==n*512u*LDTM_MIB);
        if(n==2u){
            const LdtmFilesystemSpec *long_id=ldtm_find_spec("a-very-long-future-native-provider-identifier-for-gpt");
            CHECK(long_id&&strlen(long_id->label)==36u&&!strncmp(long_id->label,"LD_H",4));
            CHECK(!strcmp(ldtm_specs()[0].key,long_id->key));
        }
    }
    puts(digest);return 0;
}
