/* SPDX-License-Identifier: GPL-3.0-or-later
 * Test-only unknown provider: real format profile and minimal namespace-free
 * test signature. Never installed or used as a production filesystem. */
#include <infiltratr/filesystem_module_v2.h>
#include <string.h>
#ifndef TEST_CATALOGUE_ID
#define TEST_CATALOGUE_ID "future-native-provider"
#endif
static const char magic[8]={'C','A','T','V','2','T','S','T'};
static int probe(const IfsFsHostRuntimeV2 *h,uint32_t *variant)
{
    char got[8];
    if(!variant)return IFS_FS_INVALID;
    *variant=0;
    int rc=ifs_fs_host_read_bytes_v2(h,0,got,sizeof got);
    if(rc)return rc;
    if(memcmp(got,magic,sizeof got))return IFS_FS_NOT_FOUND;
    *variant=77;return IFS_FS_OK;
}
static int format(const IfsFsHostRuntimeV2 *h,uint32_t variant,const char *label,uint32_t flags)
{
    if(variant!=77u||label||flags)return IFS_FS_INVALID;
    int rc=ifs_fs_host_write_bytes_v2(h,0,magic,sizeof magic);
    return rc?rc:h->flush(h->context)?IFS_FS_IO_ERROR:IFS_FS_OK;
}
static int profile(IfsFsFormatProfileV2 *p)
{
    if(!p||p->structure_bytes<sizeof *p)return IFS_FS_INVALID;
    *p=(IfsFsFormatProfileV2){.structure_bytes=sizeof *p,.format_variant=77,.probe_variant=77,
        .layout=IFS_FS_LAYOUT_NO_NAMESPACE,.exact_block_bytes=512,
        .minimum_bytes=UINT64_C(300)*1048576,.recommended_bytes=UINT64_C(384)*1048576,
        .maximum_bytes=UINT64_C(512)*1048576};
    return IFS_FS_OK;
}
static const IfsFsCoreOpsV2 core={.header={2,sizeof core,IFS_FS_GROUP_CORE,0,IFS_FS_CORE_PROBE},.probe=probe};
static const IfsFsVolumeOpsV2 volume={.header={2,sizeof volume,IFS_FS_GROUP_VOLUME,0,IFS_FS_VOLUME_FORMAT},.format=format};
static const IfsFsFormatProfileOpsV2 profiles={.header={2,sizeof profiles,IFS_FS_GROUP_FORMAT_PROFILE,0,IFS_FS_FORMAT_PROFILE_DEFAULT},.get_default=profile};
static const IfsFsGroupHeaderV2 *group(uint32_t id,uint32_t lo,uint32_t hi)
{
    if(lo>2||hi<2)return NULL;
    if(id==IFS_FS_GROUP_CORE)return &core.header;
    if(id==IFS_FS_GROUP_VOLUME)return &volume.header;
    if(id==IFS_FS_GROUP_FORMAT_PROFILE)return &profiles.header;
    return NULL;
}
static const IfsFsModuleApiV2 api={.abi_version=2,.structure_bytes=sizeof api,.format_id=TEST_CATALOGUE_ID,
    .runtime_profiles=IFS_FS_PROFILE_HOSTED,.group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)|IFS_FS_GROUP_BIT(IFS_FS_GROUP_FORMAT_PROFILE),.query_group=group};
__attribute__((visibility("default")))const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void){return &api;}
