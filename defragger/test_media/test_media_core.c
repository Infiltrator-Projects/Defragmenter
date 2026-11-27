// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media_raw.h"
#include "filesystem_module_host.h"

#include "infiltratr/core.h"
#include "infiltratr/posix.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <openssl/evp.h>

const LdtmFilesystemSpec *ldtm_find_spec(const char *key) {
    size_t index;
    if (key == NULL) return NULL;
    for (index = 0U; index < ldtm_spec_count(); ++index) {
        if (strcmp(ldtm_specs()[index].key, key) == 0) return &ldtm_specs()[index];
    }
    return NULL;
}

uint64_t ldtm_allocated_capacity_bytes(void) {
    size_t index;
    uint64_t total = 0U;
    for (index = 0U; index < ldtm_spec_count(); ++index) {
        total += (uint64_t)ldtm_specs()[index].size_mib * LDTM_MIB;
    }
    return total;
}

uint64_t ldtm_required_capacity_bytes(void) {
    return ldtm_allocated_capacity_bytes() + LDTM_GIB;
}

LdtmFragmentProfile ldtm_fragment_profile(const LdtmFilesystemSpec *spec)
{
    LdtmFragmentProfile profile = {
        .files = LDTM_TARGET_FILE_COUNT,
        .chunks = 30U,
        .chunk_kib = 2048U,
        .directory_initial = 0U,
        .directory_second = 0U,
        .file_chunks = {2U, 3U, 5U, 8U, 13U, 17U, 22U, 30U},
    };
    /* The generic corpus uses the same file-growth workload for every provider. */
    (void)spec;
    return profile;
}

uint32_t ldtm_profile_file_chunks(const LdtmFragmentProfile *profile,
                                  size_t file_index) {
    if (profile == NULL || file_index >= profile->files ||
        file_index >= LDTM_TARGET_FILE_COUNT)
        return 0U;
    return profile->file_chunks[file_index] != 0U
        ? profile->file_chunks[file_index] : profile->chunks;
}

uint64_t ldtm_profile_file_bytes(const LdtmFragmentProfile *profile,
                                 size_t file_index) {
    return (uint64_t)ldtm_profile_file_chunks(profile, file_index) *
           (uint64_t)profile->chunk_kib * UINT64_C(1024);
}

uint64_t ldtm_profile_payload_bytes(const LdtmFragmentProfile *profile) {
    if (profile == NULL) return 0U;
    uint64_t total = 0U;
    for (size_t file = 0U; file < profile->files; ++file)
        total += ldtm_profile_file_bytes(profile, file);
    return total;
}

uint64_t ldtm_target_payload_bytes(const LdtmFilesystemSpec *spec) {
    if (spec == NULL || spec->payload_mib == 0U) return 0U;
    const LdtmFragmentProfile profile = ldtm_fragment_profile(spec);
    return ldtm_profile_payload_bytes(&profile);
}

static int appendf(char *buffer, size_t capacity, size_t *used, const char *format, ...) {
    int count;
    va_list args;
    if (*used >= capacity) return -1;
    va_start(args, format);
    count = vsnprintf(buffer + *used, capacity - *used, format, args);
    va_end(args);
    if (count < 0 || (size_t)count >= capacity - *used) return -1;
    *used += (size_t)count;
    return 0;
}

int ldtm_build_sfdisk_script(char *buffer, size_t capacity) {
    size_t index;
    size_t used = 0U;
    if (buffer == NULL || capacity == 0U || !ldtm_spec_count()) return -1;
    if (appendf(buffer, capacity, &used, "label: gpt\n\n") != 0) return -1;
    for (index = 0U; index < ldtm_spec_count(); ++index) {
        const LdtmFilesystemSpec *spec = &ldtm_specs()[index];
        if(!spec->size_mib || spec->creator==LDTM_CREATOR_MANUAL)return -1;
        /* The ABI describes namespace semantics, not GPT type GUIDs. Use
         * the same generic test-data partition type for every provider. */
        const char *type = "linux";
        if (appendf(buffer, capacity, &used,
                    "size=%uMiB, type=%s, name=\"%s\"\n",
                    spec->size_mib, type, spec->label) != 0) return -1;
    }
    return 0;
}

int ldtm_transport_is_field_media(int removable, const char *transport) {
    /*
     * Transport class is discovery metadata, not a destructive-write safety
     * authority. Test Media already requires a whole writable disk, rejects
     * active system storage, requires sufficient capacity and a stable
     * serial/WWN identity, and binds destructive Build to an exact typed
     * device confirmation plus SHA-256 fingerprint revalidation immediately
     * before repartitioning. A fixed or virtual secondary disk is therefore a
     * valid deliberate test target; RM/TRAN must not veto that explicit choice.
     *
     * The historical function name is retained because both the GTK selector
     * and privileged worker use this shared policy point. Its result now means
     * "transport does not disqualify this test-media target" rather than
     * "physically removable medium".
     */
    (void)removable;
    (void)transport;
    return 1;
}

int ldtm_canonicalize_device(const char *input, char *output, size_t output_capacity)
{
    return infiltratr_realpath_copy(input, output, output_capacity) ? 0 : -1;
}

int ldtm_decode_hex_byte(char high, char low, unsigned char *value) {
    char text[3] = {high, low, '\0'};
    uint64_t parsed = 0U;
    if (value == NULL || !infiltratr_parse_u64(text, 16U, &parsed) ||
        parsed > UINT8_MAX)
        return 0;
    *value = (unsigned char)parsed;
    return 1;
}

const char *ldtm_creator_display_name(const LdtmFilesystemSpec *spec) {
    if (spec == NULL) return "Unavailable";
    return spec->creator == LDTM_CREATOR_MANUAL ?
        "Manual" : "Filesystem driver";
}

static int copy_resolved_program(const char *program, char *output,
                                 size_t output_capacity) {
    char resolved[PATH_MAX];
    if (realpath(program, resolved) == NULL || access(resolved, X_OK) != 0)
        return -1;
    if (snprintf(output, output_capacity, "%s", resolved) < 0 ||
        strlen(resolved) + 1U > output_capacity)
        return -1;
    return 0;
}

int ldtm_resolve_program(const char *program, char *output,
                         size_t output_capacity) {
    static const char *const directories[] = {
        "/usr/sbin", "/usr/bin", "/sbin", "/bin",
        "/usr/local/sbin", "/usr/local/bin"
    };
    char candidate[PATH_MAX];
    size_t index;

    if (program == NULL || *program == '\0' ||
        output == NULL || output_capacity == 0U)
        return -1;

    if (strchr(program, '/') != NULL) {
        if (program[0] != '/')
            return -1;
        return copy_resolved_program(program, output, output_capacity);
    }

    for (index = 0U; index < sizeof(directories) / sizeof(directories[0]); ++index) {
        const int count = snprintf(candidate, sizeof(candidate), "%s/%s",
                                   directories[index], program);
        if (count <= 0 || (size_t)count >= sizeof(candidate) ||
            access(candidate, X_OK) != 0)
            continue;
        if (copy_resolved_program(candidate, output, output_capacity) == 0)
            return 0;
    }
    return -1;
}

int ldtm_program_available(const char *program) {
    char resolved[PATH_MAX];
    return ldtm_resolve_program(program, resolved, sizeof(resolved)) == 0;
}

/* Only the canonical Common V2 provider capabilities grant Test Media
 * availability. Read-only proof of FORMAT and writable CORE methods occurs
 * before any image or physical medium can be altered. */
int ldtm_spec_creator_available(const LdtmFilesystemSpec *spec,
                                char *detail,size_t capacity)
{
    if(!spec || !detail || !capacity)return 0;
    if(spec->creator==LDTM_CREATOR_MANUAL) {
        (void)snprintf(detail,capacity,"Reserved: %s",spec->note);
        return 0;
    }
    char missing[384]={0};
    const int ready=ldtm_common_v2_media_detail(spec,missing,sizeof(missing));
    if(ready)
        (void)snprintf(detail,capacity,"%s: required Common V2 operations available",spec->key);
    else
        (void)snprintf(detail,capacity,"%s: missing %s; operation refused",spec->key,missing);
    return ready;
}

int ldtm_required_modules_available(char *detail, size_t detail_capacity) {
    if (!detail || detail_capacity == 0U) return 0;
    if(!ldtm_spec_count()){
        (void)snprintf(detail,detail_capacity,"No unique trusted Common V2 providers discovered; refusing operation");
        return 0;
    }
    /*
     * Never curate a secondary subset of filesystem types here: omitted
     * entries used to let Test Media erase/repartition a disk before finding
     * out FAT32, NTFS, XFS or UFS2 was unavailable. Every configured creator
     * must pass its own read-only module/ABI preflight.
     */
    for (size_t i = 0U; i < ldtm_spec_count(); ++i) {
        const LdtmFilesystemSpec *spec = &ldtm_specs()[i];
        if (!ldtm_spec_creator_available(spec, detail, detail_capacity))
            return 0;
    }
    return 1;
}

/* Bind the exact sorted provider layout to the GUI confirmation. A change
 * in installed format identities, capacities or namespace roles changes
 * this hash. It is not a claim about provider binary authenticity. */
int ldtm_catalogue_fingerprint(char output[65])
{
    if(!output)return -1;
    output[0]=0;
    if(!ldtm_spec_count())return -1;
    EVP_MD_CTX *ctx=EVP_MD_CTX_new();
    if(!ctx)return -1;
    int ok=EVP_DigestInit_ex(ctx,EVP_sha256(),NULL)==1;
    for(size_t i=0;ok&&i<ldtm_spec_count();i++){
        const LdtmFilesystemSpec *s=&ldtm_specs()[i];
        char row[256];
        int n=snprintf(row,sizeof row,"%s\t%s\t%u\t%u\t%u\n",
                       s->key,s->label,s->size_mib,s->payload_mib,(unsigned)s->creator);
        ok=n>0&&(size_t)n<sizeof row&&EVP_DigestUpdate(ctx,row,(size_t)n)==1;
    }
    unsigned char hash[32];unsigned n=0;
    if(ok)ok=EVP_DigestFinal_ex(ctx,hash,&n)==1&&n==sizeof hash;
    EVP_MD_CTX_free(ctx);
    if(!ok)return -1;
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<sizeof hash;i++){
        output[2u*i]=hex[hash[i]>>4];output[2u*i+1u]=hex[hash[i]&15u];
    }
    output[64]=0;return 0;
}
