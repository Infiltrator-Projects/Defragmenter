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
        CHECK(strcmp(ldtm_creator_display_name(s), "Built-in native") == 0);
        if (s->creator != LDTM_CREATOR_UFS)
            CHECK(ldtm_spec_creator_available(s, detail, sizeof(detail)) == 1);
    }
    puts("strict state and complete native corpus contracts passed");
    return 0;
}
int main(int argc, char **argv)
{
    if (argc == 1)
        return contracts();
    if (argc == 3 && strcmp(argv[1], "ufs-source") == 0)
        return generate_ufs_source(argv[2]) != 0;
    if (argc != 4)
        return 2;
    const LdtmFilesystemSpec *spec = ldtm_find_spec(argv[2]);
    if (spec == NULL)
        return 2;
    char detail[512] = {0};
    int result;
    if (strcmp(argv[1], "verify") == 0 && spec->creator == LDTM_CREATOR_UFS)
        result = verify_ufs2_raw_payload(argv[3], detail, sizeof(detail));
    else {
        const bool create = strcmp(argv[1], "create") == 0;
        if (!create && strcmp(argv[1], "verify") != 0)
            return 2;
        if (create) {
            const int fd = open(argv[3], O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
            CHECK(fd >= 0);
            CHECK(ftruncate(fd, (off_t)((uint64_t)spec->size_mib * LDTM_MIB)) == 0);
            CHECK(close(fd) == 0);
            CHECK(ldtm_raw_fixture(spec, argv[3], false, detail, sizeof(detail)) != 0);
        }
        result = ldtm_raw_fixture(spec, argv[3], create, detail, sizeof(detail));
    }
    fprintf(result == 0 ? stdout : stderr, "%s\n", detail);
    return result != 0;
}
