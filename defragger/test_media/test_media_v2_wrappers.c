/* SPDX-License-Identifier: GPL-3.0-or-later
 * Stable application-facing Test Media functions, implemented exclusively by
 * Common V2 providers. No private filesystem fixtures are accepted. A format
 * missing actual V2 capabilities is unavailable and never writes to a target.
 */
#include "test_media_raw.h"
#include <stdio.h>
#include <string.h>

static const LdtmFilesystemSpec *spec(const char *name)
{ return ldtm_find_spec(name); }
static int ready(const char *name)
{ return ldtm_common_v2_available(spec(name)); }
static int fixture(const char *name,const char *path,int create)
{ return ldtm_common_v2_fixture(spec(name),path,create!=0); }
static int reply(const char *name,const char *path,char *detail,size_t capacity)
{
    int result=fixture(name,path,0);
    if(detail && capacity)
        (void)snprintf(detail,capacity,"%s Common V2 corpus %s",
                       name,result==0?"verified":"unavailable or invalid");
    return result;
}
int ldtm_fat_module_available(const LdtmFilesystemSpec *s)
{
    if(!s)return 0;
    if(s->creator==LDTM_CREATOR_FAT12 ||
       s->creator==LDTM_CREATOR_FAT16 ||
       s->creator==LDTM_CREATOR_FAT32)
        return ldtm_fat_common_available(s)==0;
    return ldtm_common_v2_available(s);
}
int ldtm_fat_module_create(const LdtmFilesystemSpec *s,const char *path)
{ return ldtm_common_v2_fixture(s,path,true); }
int ldtm_raw_fat(const LdtmFilesystemSpec *s,const char *path,bool create)
{
    if(!s)return -1;
    if(s->creator==LDTM_CREATOR_FAT12 ||
       s->creator==LDTM_CREATOR_FAT16 ||
       s->creator==LDTM_CREATOR_FAT32)
        return create?ldtm_fat_common_create(s,path):
                      ldtm_fat_common_verify(s,path);
    return ldtm_common_v2_fixture(s,path,create);
}
int ldtm_raw_exfat(const LdtmFilesystemSpec *s,const char *path,bool create)
{ return s && s->creator==LDTM_CREATOR_EXFAT?
         ldtm_common_v2_fixture(s,path,create):-1; }
int ldtm_ext_module_available(const LdtmFilesystemSpec *s)
{ return s && (s->creator==LDTM_CREATOR_EXT2 ||
                s->creator==LDTM_CREATOR_EXT3 ||
                s->creator==LDTM_CREATOR_EXT4) && ldtm_common_v2_available(s); }
int ldtm_raw_ext(const LdtmFilesystemSpec *s,const char *path,bool create)
{ return ldtm_common_v2_fixture(s,path,create); }
int ldtm_minix_module_available(const LdtmFilesystemSpec *s)
{ return s && s->creator==LDTM_CREATOR_MINIX && ldtm_common_v2_available(s); }
int ldtm_minix_module_fixture(const LdtmFilesystemSpec *s,const char *path,bool create)
{ return ldtm_common_v2_fixture(s,path,create); }
int ldtm_raw_minix(const LdtmFilesystemSpec *s,const char *path,bool create)
{ return ldtm_common_v2_fixture(s,path,create); }
int ldtm_hfs_module_available(const LdtmFilesystemSpec *s)
{ return s && s->creator==LDTM_CREATOR_HFS && ldtm_common_v2_available(s); }
int ldtm_hfsplus_module_available(const LdtmFilesystemSpec *s)
{ return s && s->creator==LDTM_CREATOR_HFSPLUS && ldtm_common_v2_available(s); }
int ldtm_hfs_module_fixture(const LdtmFilesystemSpec *s,const char *path,bool create)
{ return ldtm_common_v2_fixture(s,path,create); }
int ldtm_hfsplus_module_fixture(const LdtmFilesystemSpec *s,const char *path,bool create)
{ return ldtm_common_v2_fixture(s,path,create); }
int ldtm_raw_mac(const LdtmFilesystemSpec *s,const char *path,bool create)
{ return ldtm_common_v2_fixture(s,path,create); }
int ldtm_ntfs_module_available(const LdtmFilesystemSpec *s)
{ return s && s->creator==LDTM_CREATOR_NTFS && ldtm_common_v2_available(s); }
int ldtm_raw_ntfs(const LdtmFilesystemSpec *s,const char *path,bool create)
{ return ldtm_common_v2_fixture(s,path,create); }
int ldtm_xfs_module_available(const LdtmFilesystemSpec *s)
{ return s && s->creator==LDTM_CREATOR_XFS && ldtm_common_v2_available(s); }
int ldtm_raw_xfs(const LdtmFilesystemSpec *s,const char *path,bool create)
{ return ldtm_common_v2_fixture(s,path,create); }
int ldtm_btrfs_module_available(const LdtmFilesystemSpec *s)
{ return s && s->creator==LDTM_CREATOR_BTRFS && ldtm_common_v2_available(s); }
int ldtm_btrfs_module_fixture(const LdtmFilesystemSpec *s,const char *path,bool create)
{ return ldtm_common_v2_fixture(s,path,create); }
int ldtm_raw_btrfs(const LdtmFilesystemSpec *s,const char *path,bool create)
{ return ldtm_common_v2_fixture(s,path,create); }
int ldtm_zfs_module_available(const LdtmFilesystemSpec *s)
{ return s && s->creator==LDTM_CREATOR_ZFS && ldtm_common_v2_available(s); }
int ldtm_raw_zfs(const LdtmFilesystemSpec *s,const char *path,bool create)
{ return ldtm_common_v2_fixture(s,path,create); }
/* Amiga and APFS Test Media operations are also subject to the exact same
 * real V2 format/create/read capabilities. Former V1 fixture symbols are not
 * resolved by the application or its subordinate worker. */
int ldtm_ffs_module_available(void){return ready("ffs");}
int ldtm_format_ffs_module(const char *path,const char *label)
{ (void)label;return fixture("ffs",path,1); }
int ldtm_validate_ffs_module(const char *path){return fixture("ffs",path,0);}
int ldtm_populate_ffs_module(const char *path,const LdtmFragmentProfile *p)
{ (void)p;return fixture("ffs",path,0); }
int ldtm_verify_ffs_module(const char *path,const LdtmFragmentProfile *p,
                           int after,char *detail,size_t n)
{ (void)p;(void)after;return reply("ffs",path,detail,n); }
int ldtm_ofs_module_available(void){return ready("ofs");}
int ldtm_format_ofs_module(const char *path,const char *label)
{ (void)label;return fixture("ofs",path,1); }
int ldtm_populate_ofs_module(const char *path,const LdtmFragmentProfile *p)
{ (void)p;return fixture("ofs",path,0); }
int ldtm_validate_ofs_module(const char *path){return fixture("ofs",path,0);}
int ldtm_verify_ofs_module(const char *path,const LdtmFragmentProfile *p,
                           int after,char *detail,size_t n)
{ (void)p;(void)after;return reply("ofs",path,detail,n); }
int ldtm_sfs_module_available(void){return ready("sfs");}
int ldtm_format_sfs_volume(const char *path){return fixture("sfs",path,1);}
int ldtm_populate_sfs_volume(const char *path,const LdtmFragmentProfile *p)
{ (void)p;return fixture("sfs",path,0); }
int ldtm_verify_sfs_payload(const char *path,const LdtmFragmentProfile *p,
                            char *detail,size_t n)
{ (void)p;return reply("sfs",path,detail,n); }
int ldtm_verify_sfs_payload_after_defrag(const char *path,const LdtmFragmentProfile *p,
                                         char *detail,size_t n)
{ (void)p;return reply("sfs",path,detail,n); }
int ldtm_pfs3_module_available(void){return ready("pfs3");}
int ldtm_format_pfs3_volume(const char *path){return fixture("pfs3",path,1);}
int ldtm_populate_pfs3_volume(const char *path,const LdtmFragmentProfile *p)
{ (void)p;return fixture("pfs3",path,0); }
int ldtm_verify_pfs3_payload(const char *path,const LdtmFragmentProfile *p,
                             char *detail,size_t n)
{ (void)p;return reply("pfs3",path,detail,n); }
int ldtm_verify_pfs3_payload_after_defrag(const char *path,
             const LdtmFragmentProfile *p,char *detail,size_t n)
{ (void)p;return reply("pfs3",path,detail,n); }
int ldtm_apfs_module_available(void){return ready("apfs");}
int ldtm_format_apfs_volume(const char *path){return fixture("apfs",path,1);}
int ldtm_verify_apfs_payload(const char *path,char *detail,size_t n)
{ return reply("apfs",path,detail,n); }
int ldtm_verify_apfs_payload_after_defrag(const char *path,char *detail,size_t n)
{ return reply("apfs",path,detail,n); }
