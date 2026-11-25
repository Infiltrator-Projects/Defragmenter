// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media_raw.h"
#include <stdio.h>
#include <string.h>

uint64_t ldtm_raw_file_bytes(const LdtmFilesystemSpec *spec, unsigned file)
{
    static const uint32_t edges[] = {0U, 1U, 511U, 512U, 513U, 4095U, 4096U, 4097U};
    if (file < LDTM_TARGET_FILE_COUNT) {
        const LdtmFragmentProfile profile = ldtm_fragment_profile(spec);
        return ldtm_profile_file_bytes(&profile, file);
    }
    return file < LDTM_RAW_FILES ? edges[file - LDTM_TARGET_FILE_COUNT] : 0U;
}

void ldtm_raw_name(unsigned file, char output[16])
{
    (void)snprintf(output, 16U, "F%06u.BIN", file);
}

int ldtm_raw_name_index(const char *name)
{
    for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file) {
        char expected[16];
        ldtm_raw_name(file, expected);
        if (strcmp(name, expected) == 0)
            return (int)file;
    }
    return -1;
}

void ldtm_raw_payload(uint8_t *data, size_t bytes, unsigned file, uint64_t logical)
{
    /* Each 4 KiB logical page has its own seed. Verification can therefore
     * read arbitrary filesystem extents without retaining the original image
     * or restarting a volume-sized PRNG stream. The writer never owns this
     * oracle; it only relocates the opaque bytes manufactured here. */
    while (bytes != 0U) {
        const size_t within = (size_t)(logical % 4096U);
        size_t take = 4096U - within;
        if (take > bytes)
            take = bytes;
        uint64_t state = UINT64_C(0x243f6a8885a308d3) ^ ((uint64_t)file << 40U) ^
                         ((logical / 4096U) * UINT64_C(0x9e3779b97f4a7c15));
        for (size_t index = 0U; index < within + take; ++index) {
            state ^= state >> 12U;
            state ^= state << 25U;
            state ^= state >> 27U;
            state *= UINT64_C(0x2545f4914f6cdd1d);
            if (index >= within)
                data[index - within] = (uint8_t)(state >> 56U);
        }
        data += take;
        bytes -= take;
        logical += take;
    }
}

void ldtm_raw_check_init(LdtmRawCheck *check, const LdtmFilesystemSpec *spec)
{
    *check = (LdtmRawCheck){.spec = spec};
}
int ldtm_raw_check_file(LdtmRawCheck *check, unsigned file, uint64_t bytes)
{
    if (file >= LDTM_RAW_FILES || bytes != ldtm_raw_file_bytes(check->spec, file) ||
        (check->seen & (UINT32_C(1) << file)) != 0U)
        return -1;
    check->seen |= UINT32_C(1) << file;
    return 0;
}
int ldtm_raw_check_finish(const LdtmRawCheck *check)
{
    if (check->seen != (UINT32_C(1) << LDTM_RAW_FILES) - 1U)
        return -1;
    for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file)
        if (check->done[file] != ldtm_raw_file_bytes(check->spec, file))
            return -1;
    return 0;
}
/* Every destructive operation is preceded by exact Common V2 capability
 * discovery. The provider owns format, allocation and all on-disk records.
 * Nothing is erased merely because a Test Media format was selected. */
int ldtm_raw_fixture(const LdtmFilesystemSpec *spec,const char *path,
                     bool create,char *detail,size_t capacity)
{
    if(detail && capacity)detail[0]='\0';
    if(!spec || !path || !*path)return -1;
    if(create) {
        char missing[384]={0};
        if(!ldtm_common_v2_media_detail(spec,missing,sizeof(missing))) {
            if(detail && capacity)
                (void)snprintf(detail,capacity,"%s: missing %s; no fallback",
                               spec->key,missing);
            return -1;
        }
    }
    const int result=ldtm_common_v2_fixture(spec,path,create);
    if(detail && capacity)
        (void)snprintf(detail,capacity,"%s Common V2 %s %s",
            spec->key,create?"fixture creation":"corpus verification",
            result==0?"passed":"failed");
    return result;
}
