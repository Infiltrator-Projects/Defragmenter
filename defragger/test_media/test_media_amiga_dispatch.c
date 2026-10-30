// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"

#include <infiltratr/posix_io.h>

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

int ldtm_ffs_module_available(void);
int ldtm_format_ffs_module(const char *, const char *);
int ldtm_validate_ffs_module(const char *);
int ldtm_populate_ffs_module(const char *, const LdtmFragmentProfile *);
int ldtm_verify_ffs_module(const char *, const LdtmFragmentProfile *,
                           int after, char *, size_t);
int ldtm_format_ofs_module(const char *path, const char *label);
int ldtm_validate_ofs_module(const char *);
int ldtm_verify_ofs_module(const char *, const LdtmFragmentProfile *,
                           int after, char *, size_t);
int ldtm_populate_ofs_module(const char *path, const LdtmFragmentProfile *profile);
int ldtm_format_sfs_volume(const char *path);
int ldtm_populate_sfs_volume(const char *path, const LdtmFragmentProfile *profile);
int ldtm_verify_sfs_payload(const char *path, const LdtmFragmentProfile *profile,
                            char *detail, size_t detail_capacity);
int ldtm_verify_sfs_payload_after_defrag(
    const char *path, const LdtmFragmentProfile *profile,
    char *detail, size_t detail_capacity);
int ldtm_format_pfs3_volume(const char *path);
int ldtm_populate_pfs3_volume(const char *path, const LdtmFragmentProfile *profile);
int ldtm_verify_pfs3_payload(const char *path, const LdtmFragmentProfile *profile,
                             char *detail, size_t detail_capacity);
int ldtm_verify_pfs3_payload_after_defrag(
    const char *path, const LdtmFragmentProfile *profile,
    char *detail, size_t detail_capacity);

static int path_is_pfs3(const char *path) {
    int fd;
    unsigned char magic[4];
    if (path == NULL) return 0;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    const int read_result = infiltratr_pread_full(fd, magic, sizeof(magic), 2U * 512U);
    (void)close(fd);
    return read_result == 0 && memcmp(magic, "PFS\1", 4U) == 0;
}

static int path_is_sfs(const char *path) {
    int fd;
    unsigned char magic[4];
    if (path == NULL) return 0;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    const int read_result = infiltratr_pread_full(fd, magic, sizeof(magic), 0U);
    (void)close(fd);
    return read_result == 0 && memcmp(magic, "SFS\0", 4U) == 0;
}

int ldtm_format_amiga_volume(const char *path, uint8_t dostype, const char *label) {
    if (label != NULL && strcmp(label, "LD_SFS") == 0)
        return ldtm_format_sfs_volume(path);
    if (label != NULL && strcmp(label, "LD_PFS3") == 0)
        return ldtm_format_pfs3_volume(path);
    return dostype == 0U ? ldtm_format_ofs_module(path, label) :
                          ldtm_format_ffs_module(path, label);
}

int ldtm_populate_amiga_volume(const char *path, uint8_t dostype,
                               const LdtmFragmentProfile *profile) {
    if (path_is_sfs(path)) return ldtm_populate_sfs_volume(path, profile);
    if (path_is_pfs3(path)) return ldtm_populate_pfs3_volume(path, profile);
    return dostype == 0U ? ldtm_populate_ofs_module(path, profile) :
                          ldtm_populate_ffs_module(path, profile);
}

int ldtm_verify_amiga_payload(const char *path, uint8_t dostype,
                              const LdtmFragmentProfile *profile,
                              char *detail, size_t detail_capacity) {
    if (path_is_sfs(path))
        return ldtm_verify_sfs_payload(path, profile, detail, detail_capacity);
    if (path_is_pfs3(path))
        return ldtm_verify_pfs3_payload(path, profile, detail, detail_capacity);
    return dostype == 1U
        ? ldtm_verify_ffs_module(path, profile, 0, detail, detail_capacity)
        : ldtm_verify_ofs_module(path, profile, 0, detail, detail_capacity);
}


int ldtm_verify_amiga_payload_after_defrag(
    const char *path, uint8_t dostype,
    const LdtmFragmentProfile *profile,
    char *detail, size_t detail_capacity) {
    if (path_is_sfs(path))
        return ldtm_verify_sfs_payload_after_defrag(
            path, profile, detail, detail_capacity);
    if (path_is_pfs3(path))
        return ldtm_verify_pfs3_payload_after_defrag(
            path, profile, detail, detail_capacity);
    return dostype == 1U
        ? ldtm_verify_ffs_module(path, profile, 1, detail, detail_capacity)
        : ldtm_verify_ofs_module(path, profile, 1, detail, detail_capacity);
}

/* No fallback AmigaDOS parser/formatter is linked in Test Media. */
int ldtm_validate_amiga_volume(const char *path,uint8_t dostype) {
    return dostype==0U ? ldtm_validate_ofs_module(path) :
           dostype==1U ? ldtm_validate_ffs_module(path) : -1;
}
