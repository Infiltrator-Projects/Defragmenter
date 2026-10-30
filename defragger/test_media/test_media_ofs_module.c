/* SPDX-License-Identifier: GPL-3.0-or-later
 * The dataset belongs to Test Media; filesystem creation/allocation belongs to
 * OFS.so. The retained AFFS verifier is an independent disposable-image oracle.
 */
#include "test_media.h"
#include "filesystem_module_host.h"
#include "ofs_module_abi.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *path, *label;
    const LdtmFragmentProfile *profile;
    const IfsDefragHostV2 *host;
    IfsDefragTarget target;
} Fixture;
static int read_io(void *opaque, uint64_t at, void *p, uint32_t n) {
    Fixture *f = opaque; return f->host->read_at(f->host->context, &f->target, at, p, n);
}
static int write_io(void *opaque, uint64_t at, const void *p, uint32_t n) {
    Fixture *f = opaque; return f->host->write_at(f->host->context, &f->target, at, p, n);
}
static int flush_io(void *opaque) {
    Fixture *f = opaque; return f->host->sync_target(f->host->context, &f->target);
}
static void *allocate_io(void *opaque, size_t n) {
    Fixture *f = opaque; return f->host->allocate(n);
}
static void release_io(void *opaque, void *p) {
    Fixture *f = opaque; f->host->release(p);
}
static int clock_io(void *opaque, IfsOfsDate *out) {
    (void)opaque; *out = (IfsOfsDate){0, 0, 0}; return 0;
}
static void payload(unsigned char *buffer, size_t length, uint32_t file, uint32_t block) {
    uint64_t state = UINT64_C(0x6a09e667f3bcc909) ^ ((uint64_t)file << 40) ^
                     ((uint64_t)block * UINT64_C(0x9e3779b97f4a7c15));
    for (size_t i = 0; i < length; ++i) {
        state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
        state *= UINT64_C(0x2545f4914f6cdd1d); buffer[i] = (unsigned char)(state >> 56);
    }
}
static int populate(const IfsOfsModuleV3 *api, Fixture *f) {
    const LdtmFragmentProfile *p = f->profile;
    if (!p || !p->files || p->files > 8 || !p->chunks || !p->chunk_kib) return -1;
    const char *base = "/Defragmenter-TestData";
    const char *directory = "/Defragmenter-TestData/fragmented-directory";
    const char *files = "/Defragmenter-TestData/fragmented-files";
    if (api->mkdir(base) || api->mkdir(files) || api->mkdir(directory)) return -1;
    char path[160];
    for (uint32_t i = 0; i < p->directory_initial; ++i) {
        (void)snprintf(path, sizeof(path), "%s/entry-%05u.txt", directory, i);
        if (api->create(path)) return -1;
    }
    for (uint32_t i = 0; i < p->directory_initial; i += 2) {
        (void)snprintf(path, sizeof(path), "%s/entry-%05u.txt", directory, i);
        if (api->unlink(path)) return -1;
    }
    for (uint32_t i = 0; i < p->directory_second; ++i) {
        (void)snprintf(path, sizeof(path), "%s/entry-%05u.txt", directory, p->directory_initial + i);
        if (api->create(path)) return -1;
    }
    uint32_t done[8] = {0};
    for (uint32_t i = 0; i < p->files; ++i) {
        (void)snprintf(path, sizeof(path), "%s/fragmented-%02u.bin", files, i);
        if (api->create(path)) return -1;
    }
    unsigned char *buffer = f->host->allocate(488U * 2048U);
    if (!buffer) return -1;
    int result = 0;
    for (uint32_t chunk = 0; !result && chunk < p->chunks; ++chunk) {
        for (uint32_t i = 0; !result && i < p->files; ++i) {
            uint64_t bytes = ldtm_profile_file_bytes(p, i);
            uint32_t chunks = ldtm_profile_file_chunks(p, i);
            if (bytes > UINT32_MAX || chunks < 2) { result = -1; break; }
            if (chunk >= chunks) continue;
            uint32_t blocks = (uint32_t)((bytes + 487U) / 488U);
            uint32_t run = blocks / chunks + (chunk < blocks % chunks ? 1U : 0U);
            (void)snprintf(path, sizeof(path), "%s/fragmented-%02u.bin", files, i);
            while (!result && run) {
                uint32_t count = run > 2048U ? 2048U : run, n = 0;
                for (uint32_t j = 0; j < count; ++j) {
                    uint32_t at = (done[i] + j) * 488U;
                    uint32_t take = bytes - at < 488U ? (uint32_t)(bytes - at) : 488U;
                    payload(buffer + n, take, i, done[i] + j); n += take;
                }
                result = api->write(path, done[i] * 488U, buffer, n);
                done[i] += count; run -= count;
                if (f->host->stop_requested(f->host->context)) result = -1;
            }
        }
    }
    f->host->release(buffer); return result;
}
static int create_fixture(const IfsDefragHostV2 *host, void *symbol, void *opaque) {
    Fixture *f = opaque; f->host = host;
    const IfsOfsModuleV3 *(*entry)(void) = NULL;
    _Static_assert(sizeof(entry) == sizeof(symbol), "module symbol width");
    memcpy(&entry, &symbol, sizeof(entry)); const IfsOfsModuleV3 *api = entry();
    if (!api || api->abi_version != IFS_OFS_ABI_V3 || api->structure_bytes < sizeof(*api) ||
        !api->format_id || strcmp(api->format_id, "ofs") || !api->format ||
        !api->mount || !api->unmount || !api->mkdir || !api->create ||
        !api->unlink || !api->write || !api->verify) return -1;
    if (host->open_target(host->context, f->path, 1, NULL, 0, &f->target)) return -1;
    IfsOfsIoV3 io = {sizeof(io), 0, f, f->target.bytes, read_io, write_io, flush_io,
                     allocate_io, release_io, clock_io};
    int r;
    if (f->label) {
        IfsOfsFormatV3 format = {sizeof(format), 512, 2, 0, f->label};
        r = api->format(&io, &format);
    } else {
        r = api->mount(&io, NULL);
        if (!r) {
            r = populate(api, f);
            IfsOfsCheckV3 check;
            if (!r) r = api->verify(&check);
            int unmounted = api->unmount(); if (!r) r = unmounted;
        }
    }
    host->close_target(host->context, &f->target);
    return r;
}
static int available(const IfsDefragHostV2 *host, void *symbol, void *opaque) {
    (void)host; (void)opaque;
    const IfsOfsModuleV3 *(*entry)(void) = NULL;
    memcpy(&entry, &symbol, sizeof(entry)); const IfsOfsModuleV3 *api = entry();
    return api && api->abi_version == IFS_OFS_ABI_V3 && api->structure_bytes >= sizeof(*api) &&
        api->format_id && !strcmp(api->format_id, "ofs") && api->format && api->mount &&
        api->unmount && api->mkdir && api->create && api->unlink && api->write && api->verify ? 0 : -1;
}
int ldtm_ofs_module_available(void) {
    return ifs_filesystem_module_call("ofs", "OFS.so", "infiltrator_filesystem_module_v3", available, NULL) == 0;
}
int ldtm_format_ofs_module(const char *path, const char *label) {
    Fixture f = {.path = path, .label = label ? label : "LD_OFS"};
    return ifs_filesystem_module_call("ofs", "OFS.so", "infiltrator_filesystem_module_v3", create_fixture, &f);
}
int ldtm_populate_ofs_module(const char *path, const LdtmFragmentProfile *profile) {
    Fixture f = {.path = path, .profile = profile};
    return ifs_filesystem_module_call("ofs", "OFS.so", "infiltrator_filesystem_module_v3", create_fixture, &f);
}

/* Independently check the full canonical corpus via OFS.so's public file and
 * directory APIs; Test Media does not inspect OFS blocks or bitmaps itself. */
typedef struct {
    Fixture base;
    int validate_only;
    const LdtmFragmentProfile *profile;
    char *detail;
    size_t detail_capacity;
} OfsCheckRequest;
static int inspect_ofs(const IfsDefragHostV2 *host,void *symbol,void *opaque) {
    OfsCheckRequest *q=opaque;
    if(!q || !q->base.path)return -1;
    Fixture *f=&q->base; f->host=host;
    const IfsOfsModuleV3 *(*entry)(void)=NULL;
    _Static_assert(sizeof(entry)==sizeof(symbol),"OFS symbol width");
    memcpy(&entry,&symbol,sizeof(entry));
    const IfsOfsModuleV3 *api=entry ? entry() : NULL;
    if(!api || api->abi_version!=IFS_OFS_ABI_V3 ||
       api->structure_bytes<sizeof(*api) || !api->format_id ||
       strcmp(api->format_id,"ofs") || !api->mount || !api->unmount ||
       !api->read || !api->stat || !api->verify)return -1;
    if(host->open_target(host->context,f->path,0,NULL,0,&f->target))return -1;
    IfsOfsIoV3 io={sizeof(io),IFS_OFS_READ_ONLY,f,f->target.bytes,
        read_io,NULL,NULL,allocate_io,release_io,clock_io};
    IfsOfsMountV3 options={sizeof(options),0,0,0};
    int rc=api->mount(&io,&options);
    if(rc)goto close_target;
    IfsOfsCheckV3 check={.structure_bytes=sizeof(check)};
    rc=api->verify(&check);
    if(!rc && (check.errors || check.bitmap_mismatches))rc=-1;
    if(!rc && !q->validate_only) {
        if(!q->profile || !q->profile->files ||
           q->profile->files>LDTM_TARGET_FILE_COUNT)rc=-1;
        for(uint32_t i=0;!rc && i<q->profile->files;i++) {
            char path[160]; IfsOfsEntryV3 file={.structure_bytes=sizeof(file)};
            (void)snprintf(path,sizeof(path),
                "/Defragmenter-TestData/fragmented-files/fragmented-%02u.bin",i);
            uint64_t bytes=ldtm_profile_file_bytes(q->profile,i);
            if(bytes>UINT32_MAX || api->stat(path,&file) || file.bytes!=bytes) {
                rc=-1;break;
            }
            unsigned char actual[488U*32U],expected[488U*32U];
            for(uint64_t pos=0;!rc && pos<bytes;) {
                const uint32_t n=bytes-pos<sizeof(actual)
                    ?(uint32_t)(bytes-pos):(uint32_t)sizeof(actual);
                if(api->read(path,(uint32_t)pos,actual,n)) {rc=-1;break;}
                for(uint32_t off=0;off<n;) {
                    uint32_t take=n-off<488U?n-off:488U;
                    payload(expected+off,take,i,(uint32_t)((pos+off)/488U));
                    off+=take;
                }
                if(memcmp(actual,expected,n))rc=-1;
                pos+=n;
            }
        }
        const char *directory="/Defragmenter-TestData/fragmented-directory";
        for(uint32_t i=0;!rc && i<q->profile->directory_initial+
                q->profile->directory_second;i++) {
            char path[160];IfsOfsEntryV3 file={.structure_bytes=sizeof(file)};
            (void)snprintf(path,sizeof(path),"%s/entry-%05u.txt",directory,i);
            const int found=api->stat(path,&file);
            const int present=(i>=q->profile->directory_initial || (i&1U));
            if((present && found!=IFS_OFS_OK) ||
               (!present && found!=IFS_OFS_NOT_FOUND))rc=-1;
        }
    }
    {int urc=api->unmount(); if(!rc)rc=urc;}
close_target:
    host->close_target(host->context,&f->target);
    if(q->detail && q->detail_capacity)
        (void)snprintf(q->detail,q->detail_capacity,"%s",
            rc ? "OFS.so integrity, corpus or directory verification failed" :
                 q->validate_only ? "OFS.so validated blank DOS0 volume" :
                 "OFS.so verified all corpus bytes and directory entries");
    return rc;
}
int ldtm_validate_ofs_module(const char *path) {
    OfsCheckRequest q={.base={.path=path},.validate_only=1};
    return ifs_filesystem_module_call("ofs","OFS.so",
        "infiltrator_filesystem_module_v3",inspect_ofs,&q)==0?0:-1;
}
int ldtm_verify_ofs_module(const char *path,const LdtmFragmentProfile *profile,
                           int after,char *detail,size_t capacity) {
    (void)after; /* Fragment layout is checked by the production module analyser. */
    OfsCheckRequest q={.base={.path=path},.profile=profile,
        .detail=detail,.detail_capacity=capacity};
    return ifs_filesystem_module_call("ofs","OFS.so",
        "infiltrator_filesystem_module_v3",inspect_ofs,&q)==0?0:-1;
}
