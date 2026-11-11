// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"

#include <stdint.h>
#include <string.h>

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

int ldtm_format_amiga_volume(const char *path, uint8_t dostype, const char *label) {
    if (label != NULL && strcmp(label, "LD_SFS") == 0)
        return ldtm_format_sfs_volume(path);
    if (label != NULL && strcmp(label, "LD_PFS3") == 0)
        return ldtm_format_pfs3_volume(path);
    return dostype == 0U ? ldtm_format_ofs_module(path, label) :
                          ldtm_format_ffs_module(path, label);
}

/* Test Media already knows the selected filesystem identity. Dispatch by
 * that explicit identity, never by reading or duplicating SFS/PFS3 disk magic.
 * Each independent SO verifies its own on-disk format before any mutation. */
int ldtm_populate_amiga_volume(const char *path, const char *format_id,
                               const LdtmFragmentProfile *profile) {
    if (!path || !format_id || !profile) return -1;
    if (strcmp(format_id, "sfs") == 0)
        return ldtm_populate_sfs_volume(path, profile);
    if (strcmp(format_id, "pfs3") == 0)
        return ldtm_populate_pfs3_volume(path, profile);
    if (strcmp(format_id, "ofs") == 0)
        return ldtm_populate_ofs_module(path, profile);
    if (strcmp(format_id, "ffs") == 0)
        return ldtm_populate_ffs_module(path, profile);
    return -1;
}

int ldtm_verify_amiga_payload(const char *path, const char *format_id,
                              const LdtmFragmentProfile *profile,
                              char *detail, size_t detail_capacity) {
    if (!path || !format_id || !profile) return -1;
    if (strcmp(format_id, "sfs") == 0)
        return ldtm_verify_sfs_payload(path, profile, detail, detail_capacity);
    if (strcmp(format_id, "pfs3") == 0)
        return ldtm_verify_pfs3_payload(path, profile, detail, detail_capacity);
    if (strcmp(format_id, "ofs") == 0)
        return ldtm_verify_ofs_module(path, profile, 0, detail, detail_capacity);
    if (strcmp(format_id, "ffs") == 0)
        return ldtm_verify_ffs_module(path, profile, 0, detail, detail_capacity);
    return -1;
}

int ldtm_verify_amiga_payload_after_defrag(
    const char *path, const char *format_id,
    const LdtmFragmentProfile *profile,
    char *detail, size_t detail_capacity) {
    if (!path || !format_id || !profile) return -1;
    if (strcmp(format_id, "sfs") == 0)
        return ldtm_verify_sfs_payload_after_defrag(
            path, profile, detail, detail_capacity);
    if (strcmp(format_id, "pfs3") == 0)
        return ldtm_verify_pfs3_payload_after_defrag(
            path, profile, detail, detail_capacity);
    if (strcmp(format_id, "ofs") == 0)
        return ldtm_verify_ofs_module(path, profile, 1, detail, detail_capacity);
    if (strcmp(format_id, "ffs") == 0)
        return ldtm_verify_ffs_module(path, profile, 1, detail, detail_capacity);
    return -1;
}

/* No fallback AmigaDOS parser/formatter is linked in Test Media. */
int ldtm_validate_amiga_volume(const char *path,uint8_t dostype) {
    return dostype==0U ? ldtm_validate_ofs_module(path) :
           dostype==1U ? ldtm_validate_ffs_module(path) : -1;
}
