// SPDX-License-Identifier: GPL-3.0-or-later
/* Exercise the real state parser and corpus verifier. This translation unit
 * deliberately includes the worker so no permissive test-only parser exists. */
#include "../test_media/test_media_worker.c"

#define CHECK(test)                                                                                \
    do {                                                                                           \
        if (!(test)) {                                                                             \
            fprintf(stderr, "line %d: %s\n", __LINE__, #test);                                     \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int parse_case(const char *headers, const char *suffix, int omit, int duplicate)
{
    FILE *stream = tmpfile();
    if (stream == NULL)
        return -2;
    fputs(headers, stream);
    for (size_t n = 0U; n < LDTM_SPEC_COUNT; ++n) {
        if ((int)n == omit)
            continue;
        fprintf(stream, "fs\t%s\tpopulated\tfully created\n", ldtm_specs()[n].key);
        if ((int)n == duplicate)
            fprintf(stream, "fs\t%s\tpopulated\tduplicate\n", ldtm_specs()[n].key);
    }
    fputs(suffix, stream);
    rewind(stream);
    LdtmVerifyFilesystem state[LDTM_SPEC_COUNT];
    char hash[LDTM_HASH_HEX];
    const int result = parse_verify_state(stream, "/dev/qualification", state, hash);
    fclose(stream);
    return result;
}
static int contracts(void)
{
    /* Regression: only current mounts of the selected block device may be
     * unmounted. Old /media paths from a past lsblk snapshot are not
     * authority, and a different disk must never be targeted. */
    {
        const LdtmMountedDevice selected[] = {
            {179U, 0U}, {179U, 3U}, {179U, 4U}
        };
        char mountpoint[PATH_MAX];
        CHECK(ldtm_mountinfo_target(
            "41 21 179:4 / /media/ssmith/LD_EXT4 rw,relatime - ext4 /dev/mmcblk0p4 rw\n",
            selected, 3U, mountpoint) == 1);
        CHECK(strcmp(mountpoint, "/media/ssmith/LD_EXT4") == 0);
        /* Btrfs may report a pseudo-device superblock number; its actual
         * mountinfo source still belongs to the selected physical disk.
         * This synthetic positive case covers the ordinary MAJ:MIN parser;
         * resolving a real block source is exercised on mounted hardware. */
        CHECK(ldtm_mountinfo_target(
            "42 21 179:3 / /media/ssmith/LD\\040EXT3 rw,relatime - ext3 /dev/mmcblk0p3 rw\n",
            selected, 3U, mountpoint) == 1);
        CHECK(strcmp(mountpoint, "/media/ssmith/LD EXT3") == 0);
        CHECK(ldtm_mountinfo_target(
            "43 21 8:1 / /media/other rw,relatime - ext4 /dev/sda1 rw\n",
            selected, 3U, mountpoint) == 0);
        CHECK(mountpoint[0] == '\0');
        CHECK(ldtm_mountinfo_target(
            "not valid mountinfo", selected, 3U, mountpoint) == -1);
        CHECK(ldtm_matches_device_number(179U, 4U, selected, 3U));
        CHECK(!ldtm_matches_device_number(8U, 1U, selected, 3U));
    }
    const char *headers = "schema\t3\ncorpus\t1\ndevice\t/dev/"
                          "qualification\nfingerprint\t0123456789abcdef0123456789abcdef0123456789ab"
                          "cdef0123456789abcdef\n";
    CHECK(parse_case(headers, "", -1, -1) == 0);
    CHECK(parse_case(headers, "", 3, -1) != 0);
    CHECK(parse_case(headers, "", -1, 4) != 0);
    CHECK(parse_case(headers, "schema\t3\n", -1, -1) != 0);
    CHECK(parse_case(headers, "file\tbtrfs\tmissing.bin\t0\t00\n", -1, -1) != 0);
    CHECK(parse_case(headers, "dircount\tbtrfs\t0\n", -1, -1) != 0);
    CHECK(parse_case(headers, "fs\tunknown\tpopulated\tbad\n", -1, -1) != 0);
    CHECK(parse_case(headers, "truncated", -1, -1) != 0);
    CHECK(parse_case("schema\t2\nfingerprint\t0123456789abcdef0123456789abcdef0123456789abcdef01234"
                     "56789abcdef\n",
                     "", -1, -1) != 0);
    CHECK(parse_case("schema\t3\ncorpus\t1\ndevice\t/dev/"
                     "different\nfingerprint\t0123456789abcdef0123456789abcdef0123456789abcdef01234"
                     "56789abcdef\n",
                     "", -1, -1) != 0);
    CHECK(parse_case("schema\t3\ncorpus\t1\ndevice\t/dev/"
                     "qualification\nfingerprint\tzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"
                     "zzzzzzzzzzzzzzz\n",
                     "", -1, -1) != 0);
    char long_line[8192];
    memset(long_line, 'x', sizeof(long_line));
    long_line[sizeof(long_line) - 2U] = '\n';
    long_line[sizeof(long_line) - 1U] = '\0';
    CHECK(parse_case(headers, long_line, -1, -1) != 0);
    LdtmRawCheck check;
    const LdtmFilesystemSpec *spec = ldtm_find_spec("btrfs");
    ldtm_raw_check_init(&check, spec);
    CHECK(ldtm_raw_check_finish(&check) != 0); /* The original empty-list false pass. */
    for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file) {
        CHECK(ldtm_raw_check_file(&check, file, ldtm_raw_file_bytes(spec, file)) == 0);
        CHECK(ldtm_raw_check_file(&check, file, ldtm_raw_file_bytes(spec, file)) != 0);
    }
    CHECK(ldtm_raw_check_finish(&check) != 0); /* Names without payloads cannot pass. */
    for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file)
        check.done[file] = ldtm_raw_file_bytes(spec, file);
    CHECK(ldtm_raw_check_finish(&check) == 0);
    for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file) {
        check.seen ^= UINT32_C(1) << file;
        CHECK(ldtm_raw_check_finish(&check) != 0);
        check.seen ^= UINT32_C(1) << file;
    }
    for (size_t n = 0U; n < LDTM_SPEC_COUNT; ++n) {
        const LdtmFilesystemSpec *s = &ldtm_specs()[n];
        char detail[256];
        CHECK(strcmp(ldtm_creator_display_name(s), "Filesystem driver") == 0);
        /* Unit tests are independent of the installed module inventory.
         * Real released providers must pass the separate corpus matrix;
         * the missing-provider test proves no writes on refusal. */
        int ready=ldtm_spec_creator_available(s,detail,sizeof(detail));
        CHECK(ready==0 || ready==1);
        CHECK(detail[0]!='\0');
    }
    puts("strict state and complete native corpus contracts passed");
    return 0;
}
/* Independent negative regression: obtain the retained payload location
 * from the installed provider's Common V2 extent map, then damage exactly
 * one byte on a disposable regular image. This code is linked only into
 * the test executable. No raw writer is introduced into Test Media itself. */
typedef struct {
    const LdtmFilesystemSpec *spec;
    const char *path;
    uint64_t offset;
} CorruptionTarget;

static int locate_payload(const IfsDefragHostV2 *host, void *symbol, void *opaque)
{
    CorruptionTarget *t=opaque;
    const char *id=!strcmp(t->spec->key,"ufs")?"ufs2":t->spec->key;
    IfsFsModuleApiEntryV2 entry=NULL;
    _Static_assert(sizeof(entry)==sizeof(symbol),"Common V2 entry width");
    memcpy(&entry,&symbol,sizeof(entry));
    const IfsFsModuleApiV2 *api=entry?entry():NULL;
    if(!api || !ifs_fs_api_v2_valid(api,id) ||
       !(api->runtime_profiles&IFS_FS_PROFILE_HOSTED))return -1;
    const IfsFsCoreOpsV2 *core=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsDefragOpsV2 *map=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    if(!core || !core->mount || !core->unmount || !map ||
       !(map->header.capabilities&IFS_FS_DEFRAG_MAP) || !map->map)return -1;
    IfsFilesystemTargetV2 binding={.services=host};
    if(host->open_target(host->context,t->path,0,NULL,0u,&binding.target)!=0)
        return -1;
    IfsFsHostRuntimeV2 runtime={0};
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    void *session=NULL;
    int rc=-1;
    if(ifs_filesystem_target_runtime_v2(&binding,0,0u,&runtime)!=0 ||
       core->mount(&runtime,&session,&info)!=IFS_FS_OK || !session)
        goto done;
    char name[16],path[32];
    ldtm_raw_name(0u,name);
    int len=snprintf(path,sizeof(path),"/%s",name);
    IfsFsExtentV2 extent={0};
    uint32_t count=0u;
    if(len>0 && (size_t)len<sizeof(path) &&
       map->map(session,path,0u,&extent,1u,&count)==IFS_FS_OK &&
       count==1u && !extent.flags && !extent.reserved &&
       extent.logical_offset==0u && extent.length>=64u &&
       extent.physical_offset<=runtime.volume_bytes &&
       64u<=runtime.volume_bytes-extent.physical_offset) {
        t->offset=extent.physical_offset;
        rc=0;
    }
    if(core->unmount(session)!=IFS_FS_OK)rc=-1;
done:
    host->close_target(host->context,&binding.target);
    return rc;
}

static int verify_corruption(const LdtmFilesystemSpec *spec,const char *path)
{
    if(spec->creator==LDTM_CREATOR_SWAP)return 2;
    char detail[512]={0};
    CHECK(ldtm_raw_fixture(spec,path,false,detail,sizeof(detail))==0);
    const char *id=!strcmp(spec->key,"ufs")?"ufs2":spec->key;
    char module[64]={0};
    CorruptionTarget target={.spec=spec,.path=path};
    CHECK(ifs_filesystem_module_find_readonly_v2(id,module,sizeof(module))==0);
    CHECK(ifs_filesystem_module_call(id,module,IFS_FS_COMMON_EXPORT_V2,
                                    locate_payload,&target)==0);
    /* Never permit this negative test to target a real device. */
    struct stat st;
    CHECK(lstat(path,&st)==0 && S_ISREG(st.st_mode) && st.st_nlink==1);
    int fd=open(path,O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    CHECK(fd>=0);
    struct stat opened;
    uint8_t actual[64],expected[64];
    ldtm_raw_payload(expected,sizeof(expected),0u,0u);
    if(fstat(fd,&opened)!=0 || !S_ISREG(opened.st_mode) ||
       opened.st_dev!=st.st_dev || opened.st_ino!=st.st_ino ||
       opened.st_size!=(off_t)((uint64_t)spec->size_mib*LDTM_MIB) ||
       pread(fd,actual,sizeof(actual),(off_t)target.offset)!=(ssize_t)sizeof(actual) ||
       memcmp(actual,expected,sizeof(actual))) {
        close(fd);
        fprintf(stderr,"Common V2 extent did not identify the expected payload bytes\n");
        return 1;
    }
    const off_t offset=(off_t)(target.offset+37u);
    const uint8_t damaged=(uint8_t)(actual[37]^0x40u);
    int changed=pwrite(fd,&damaged,1u,offset)==1 && fsync(fd)==0;
    int rejected=changed && ldtm_raw_fixture(spec,path,false,detail,sizeof(detail))!=0;
    /* Always restore before returning, even if the verifier accepted damage. */
    int restored=pwrite(fd,actual+37,1u,offset)==1 && fsync(fd)==0;
    if(close(fd)!=0)restored=0;
    CHECK(changed && rejected && restored);
    CHECK(ldtm_raw_fixture(spec,path,false,detail,sizeof(detail))==0);
    puts("Common V2 verifier rejected one corrupted payload byte and accepted the restored corpus");
    return 0;
}

/* Exercise each mutation against an independently prepared, fragmented
 * regular image using the production Common V2 host. A packed image is not
 * evidence that Growth Defrag can relocate a fragmented corpus. */
static int qualify_image_operation(const LdtmFilesystemSpec *spec,
                                   const char *path,const char *operation)
{
    struct stat st;
    char detail[512]={0};
    CHECK(lstat(path,&st)==0 && S_ISREG(st.st_mode) && st.st_nlink==1);
    CHECK(spec->creator!=LDTM_CREATOR_SWAP);
    CHECK(require_fragmentation_state(spec,path,1,detail,sizeof(detail))==0);
    CHECK(run_production_defrag(spec,path,operation)==0);
    CHECK(require_fragmentation_state(spec,path,0,detail,sizeof(detail))==0);
    printf("%s: %s verified all corpus bytes and zero fragmented large files\n",
           spec->key,operation);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 1)
        return contracts();
    if (argc == 3 && strcmp(argv[1],"preflight")==0) {
        const LdtmFilesystemSpec *spec=ldtm_find_spec(argv[2]);
        if(!spec)return 2;
        char detail[512]={0};
        int ready=ldtm_spec_creator_available(spec,detail,sizeof(detail));
        fprintf(ready?stdout:stderr,"%s\n",detail);
        return ready?0:1;
    }
    if (argc == 3 && strcmp(argv[1], "preflight-corpus") == 0) {
        const LdtmFilesystemSpec *spec = ldtm_find_spec(argv[2]);
        if (!spec) return 2;
        char detail[512] = {0};
        int result = preflight_corpus(spec, detail, sizeof(detail));
        fprintf(result == 0 ? stdout : stderr, "%s\n", detail);
        return result != 0;
    }
    if (argc == 3 && strcmp(argv[1], "preflight-mutations") == 0) {
        const LdtmFilesystemSpec *spec = ldtm_find_spec(argv[2]);
        if (!spec) return 2;
        char detail[512] = {0};
        int result = preflight_mutations(spec, detail, sizeof(detail));
        fprintf(result == 0 ? stdout : stderr, "%s\n", detail);
        return result != 0;
    }
    if (argc != 4)
        return 2;
    const LdtmFilesystemSpec *spec = ldtm_find_spec(argv[2]);
    if (spec == NULL)
        return 2;
    if (strcmp(argv[1], "verify-corruption") == 0)
        return verify_corruption(spec, argv[3]);
    if (strcmp(argv[1], "qualify-defrag") == 0)
        return qualify_image_operation(spec, argv[3], "defrag");
    if (strcmp(argv[1], "qualify-growth") == 0)
        return qualify_image_operation(spec, argv[3], "growth-defrag");
    char detail[512] = {0};
    int result;
    {
        const bool qualified = strcmp(argv[1], "qualify-create") == 0;
        const bool fresh = qualified || strcmp(argv[1], "create") == 0;
        const bool create = fresh || strcmp(argv[1], "recreate") == 0;
        if (!create && strcmp(argv[1], "verify") != 0)
            return 2;
        if (fresh) {
            const int fd = open(argv[3], O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
            CHECK(fd >= 0);
            CHECK(ftruncate(fd, (off_t)((uint64_t)spec->size_mib * LDTM_MIB)) == 0);
            CHECK(close(fd) == 0);
            CHECK(ldtm_raw_fixture(spec, argv[3], false, detail, sizeof(detail)) != 0);
        }
        if (qualified) {
            FILE *state = tmpfile();
            CHECK(state != NULL);
            result = create_raw_and_populate(spec, argv[3], state);
            CHECK(fclose(state) == 0);
        } else {
            result = ldtm_raw_fixture(spec, argv[3], create, detail, sizeof(detail));
        }
    }
    fprintf(result == 0 ? stdout : stderr, "%s\n", detail);
    return result != 0;
}
