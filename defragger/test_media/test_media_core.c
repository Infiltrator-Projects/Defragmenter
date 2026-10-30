// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"

#include "infiltratr/core.h"
#include "infiltratr/posix.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const LdtmFilesystemSpec LDTM_SPECS[LDTM_SPEC_COUNT] = {
    {"fat12", "LD_FAT12", 255U, 200U, LDTM_CREATOR_FAT12,
     "First-party FAT12 fixture uses 64 KiB clusters below the 4085-cluster FAT16 threshold and "
     "retains the full 200 MiB corpus plus boundary files."},
    {"fat16", "LD_FAT16", 2048U, 200U, LDTM_CREATOR_FAT16, ""},
    {"fat32", "LD_FAT32", 2048U, 200U, LDTM_CREATOR_FAT32, ""},
    {"exfat", "LD_EXFAT", 2048U, 200U, LDTM_CREATOR_EXFAT, ""},
    {"ntfs", "LD_NTFS", 2048U, 200U, LDTM_CREATOR_NTFS, ""},
    {"ext2", "LD_EXT2", 2048U, 200U, LDTM_CREATOR_EXT2, ""},
    {"ext3", "LD_EXT3", 2048U, 200U, LDTM_CREATOR_EXT3, ""},
    {"ext4", "LD_EXT4", 2048U, 200U, LDTM_CREATOR_EXT4, ""},
    {"xfs", "LD_XFS", 2048U, 200U, LDTM_CREATOR_XFS, ""},
    {"btrfs", "LD_BTRFS", 2048U, 200U, LDTM_CREATOR_BTRFS, ""},
    {"ofs", "LD_OFS", 2048U, 200U, LDTM_CREATOR_AFFS,
     "Amiga Original File System DOS\\0; installed OFS driver uses the full 2 GiB qualification "
     "cap."},
    {"ffs", "LD_FFS", 2048U, 200U, LDTM_CREATOR_AFFS,
     "Amiga Fast File System DOS\\1; first-party raw C creator uses the full 2 GiB qualification "
     "cap."},
    {"sfs", "LD_SFS", 2048U, 200U, LDTM_CREATOR_AFFS,
     "Amiga Smart File System SFS0 v3; first-party raw C creator uses the standard 200 MiB "
     "heterogeneous fragmented corpus."},
    {"pfs3", "LD_PFS3", 2048U, 200U, LDTM_CREATOR_PFS3,
     "Amiga Professional File System 3; first-party raw C creator uses the standard 200 MiB "
     "heterogeneous fragmented corpus in the qualified small-disk subset."},
    {"hfs", "LD_HFS", 2048U, 200U, LDTM_CREATOR_HFS, ""},
    {"hfsplus", "LD_HFSPLUS", 2048U, 200U, LDTM_CREATOR_HFSPLUS, ""},
    {"minix", "LD_MINIX", 2048U, 200U, LDTM_CREATOR_MINIX, ""},
    {"ufs", "LD_UFS", 2048U, 200U, LDTM_CREATOR_UFS,
     "Built-in first-party bounded UFS2/FFS creator writes the qualification image directly and "
     "requires exact allocation/fragmentation analysis before native mutation testing."},
    {"zfs", "LD_ZFS", 2048U, 200U, LDTM_CREATOR_ZFS,
     "First-party raw C creator writes a bounded legacy-version-28 single-disk ZFS fixture with "
     "Fletcher-4 block checksums; the native exact analyser verifies allocation and fragmentation. "
     "Analysis-only by design."},
    {"apfs", "LD_APFS", 2048U, 200U, LDTM_CREATOR_APFS,
     "Built-in first-party raw C creator manufactures a 2 GiB qualified bounded APFS "
     "checkpoint/spaceman/flat-tree fixture with the standard 200 MiB heterogeneous fragmented "
     "corpus."},
    {"swap", "LD_SWAP", 2048U, 0U, LDTM_CREATOR_SWAP, "Swap contains no files."}};

const LdtmFilesystemSpec *ldtm_specs(void) { return LDTM_SPECS; }
size_t ldtm_spec_count(void) { return LDTM_SPEC_COUNT; }

const LdtmFilesystemSpec *ldtm_find_spec(const char *key) {
    size_t index;
    if (key == NULL) return NULL;
    for (index = 0U; index < LDTM_SPEC_COUNT; ++index) {
        if (strcmp(LDTM_SPECS[index].key, key) == 0) return &LDTM_SPECS[index];
    }
    return NULL;
}

uint64_t ldtm_allocated_capacity_bytes(void) {
    size_t index;
    uint64_t total = 0U;
    for (index = 0U; index < LDTM_SPEC_COUNT; ++index) {
        total += (uint64_t)LDTM_SPECS[index].size_mib * LDTM_MIB;
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
    /* Classic Amiga fixtures additionally stress directory chains. The other
     * bounded creators manufacture fragmentation directly in their records. */
    if (spec != NULL && (strcmp(spec->key, "ofs") == 0 || strcmp(spec->key, "ffs") == 0)) {
        profile.directory_initial = 4096U;
        profile.directory_second = 4096U;
    }
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
    if (buffer == NULL || capacity == 0U) return -1;
    if (appendf(buffer, capacity, &used, "label: gpt\n\n") != 0) return -1;
    for (index = 0U; index < LDTM_SPEC_COUNT; ++index) {
        const LdtmFilesystemSpec *spec = &LDTM_SPECS[index];
        const char *type = spec->creator == LDTM_CREATOR_SWAP ? "swap" : "linux";
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

const char *ldtm_creator_program(const LdtmFilesystemSpec *spec) {
    if (spec == NULL) return NULL;
    if (spec->creator == LDTM_CREATOR_UFS)
        return "makefs";
    if (spec->creator == LDTM_CREATOR_AFFS) {
        if (strcmp(spec->key, "ofs") == 0)
            return "/System/Commands/Defragmenter/test-media-mkfs-ofs";
        if (strcmp(spec->key, "ffs") == 0)
            return "/System/Commands/Defragmenter/test-media-mkfs-ffs";
    }
    return NULL;
}

const char *ldtm_creator_display_name(const LdtmFilesystemSpec *spec) {
    if (spec == NULL) return "Unavailable";
    if (spec->creator == LDTM_CREATOR_FAT12 || spec->creator == LDTM_CREATOR_FAT16 ||
        strcmp(spec->key, "ofs") == 0)
        return "Filesystem driver";
    return spec->creator == LDTM_CREATOR_MANUAL ? "Manual" : "Built-in native";
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

    /*
     * UFS Test Media is intentionally self-contained.  Keep the historical
     * argv[0] spelling used by the worker, but bind it exclusively to the
     * package-owned creator so a host makefs installation can neither become
     * a hidden dependency nor silently change the qualification geometry.
     */
    if (strcmp(program, "makefs") == 0)
        return copy_resolved_program(
            "/System/Commands/Defragmenter/test-media-mkfs-ufs",
            output, output_capacity);

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

int ldtm_spec_creator_available(const LdtmFilesystemSpec *spec,
                                char *detail, size_t detail_capacity) {
    if (detail == NULL || detail_capacity == 0U || spec == NULL) return 0;
    if (spec->creator == LDTM_CREATOR_FAT12 || spec->creator == LDTM_CREATOR_FAT16 ||
        spec->creator == LDTM_CREATOR_FAT32) {
        extern int ldtm_fat_module_available(const LdtmFilesystemSpec *);
        const int ready = ldtm_fat_module_available(spec);
        (void)snprintf(detail, detail_capacity, ready ?
            "%s driver is ready to create and populate test media." :
            "Install or update the %s filesystem driver before creating test media.", spec->key);
        return ready;
    }
    if (spec->creator == LDTM_CREATOR_MINIX) {
        extern int ldtm_minix_module_available(const LdtmFilesystemSpec *);
        const int ready=ldtm_minix_module_available(spec);
        (void)snprintf(detail,detail_capacity,"%s",ready ?
            "MINIX.so is ready to format and verify test media." :
            "Install or update MINIX.so.");
        return ready;
    }
    if (spec->creator == LDTM_CREATOR_BTRFS) {
        extern int ldtm_btrfs_module_available(const LdtmFilesystemSpec *);
        const int ready=ldtm_btrfs_module_available(spec);
        (void)snprintf(detail,detail_capacity,"%s",ready ?
            "BTRFS.so is ready to format and verify test media." :
            "Install or update BTRFS.so.");
        return ready;
    }
    if (spec->creator == LDTM_CREATOR_HFS) {
        extern int ldtm_hfs_module_available(const LdtmFilesystemSpec *);
        const int ready=ldtm_hfs_module_available(spec);
        (void)snprintf(detail,detail_capacity,"%s",ready ?
            "HFS.so is ready to format and verify test media." :
            "Install or update the HFS.so filesystem driver.");
        return ready;
    }
    if (spec->creator == LDTM_CREATOR_HFSPLUS) {
        extern int ldtm_hfsplus_module_available(const LdtmFilesystemSpec *);
        const int ready = ldtm_hfsplus_module_available(spec);
        (void)snprintf(detail,detail_capacity,"%s",ready ?
            "HFSPLUS.so is ready to format and verify test media." :
            "Install or update the HFSPLUS.so filesystem driver.");
        return ready;
    }
    if (spec->creator == LDTM_CREATOR_NTFS) {
        extern int ldtm_ntfs_module_available(const LdtmFilesystemSpec *);
        const int ready = ldtm_ntfs_module_available(spec);
        (void)snprintf(detail, detail_capacity, "%s", ready ?
            "NTFS.so is ready to create and populate test media." :
            "Install or update the NTFS filesystem driver before creating test media.");
        return ready;
    }
    if (spec->creator == LDTM_CREATOR_SWAP) {
        extern int ldtm_swap_module_available(const LdtmFilesystemSpec *);
        const int ready = ldtm_swap_module_available(spec);
        (void)snprintf(detail, detail_capacity, "%s", ready ?
            "SWAP driver is ready to format disposable test media." :
            "Install or update the SWAP filesystem driver before creating test media.");
        return ready;
    }
    if (spec->creator == LDTM_CREATOR_ZFS) {
        extern int ldtm_zfs_module_available(const LdtmFilesystemSpec *);
        const int ready = ldtm_zfs_module_available(spec);
        (void)snprintf(detail,detail_capacity,"%s",ready ?
           "ZFS driver ready to create and verify test media." :
           "Install or update the ZFS filesystem driver before creating test media.");
        return ready;
    }
    if (spec->creator == LDTM_CREATOR_AFFS && strcmp(spec->key, "sfs") == 0) {
        extern int ldtm_sfs_module_available(void);
        const int ready = ldtm_sfs_module_available();
        (void)snprintf(detail,detail_capacity,"%s",ready ?
            "SFS driver ready to format and verify test media." :
            "Install or update the SFS filesystem driver before creating test media.");
        return ready;
    }
    if (spec->creator == LDTM_CREATOR_AFFS && strcmp(spec->key, "ofs") == 0) {
        extern int ldtm_ofs_module_available(void);
        const int ready = ldtm_ofs_module_available();
        (void)snprintf(detail, detail_capacity, "%s", ready ?
            "OFS driver is ready to create and populate test media." :
            "Install or update the OFS filesystem driver before creating test media.");
        return ready;
    }
    if (spec->creator == LDTM_CREATOR_AFFS && strcmp(spec->key, "ffs") == 0) {
        (void)snprintf(detail, detail_capacity, "Built-in raw C creator + payload engine: Amiga DOS\\1 FFS");
        return 1;
    }
    if (spec->creator == LDTM_CREATOR_PFS3) {
        extern int ldtm_pfs3_module_available(void);
        const int ready=ldtm_pfs3_module_available();
        (void)snprintf(detail,detail_capacity,"%s",ready ?
            "PFS3 driver ready to format and verify test media." :
            "Install or update the PFS3 filesystem driver before creating test media.");
        return ready;
    }
    if (spec->creator == LDTM_CREATOR_UFS) {
        if (ldtm_program_available("makefs")) {
            (void)snprintf(detail, detail_capacity,
                           "Built-in first-party UFS2 creator: bounded raw FFS fixture engine");
            return 1;
        }
        (void)snprintf(detail, detail_capacity,
                       "Missing bundled UFS2 creator: /System/Commands/Defragmenter/test-media-mkfs-ufs");
        return 0;
    }
    if (spec->creator == LDTM_CREATOR_APFS) {
        extern int ldtm_apfs_module_available(void);
        const int ready=ldtm_apfs_module_available();
        (void)snprintf(detail,detail_capacity,"%s",ready ?
            "APFS.so ready to format and verify disposable media." :
            "Install or update the APFS filesystem driver before creating test media.");
        return ready;
    }
    if (spec->creator == LDTM_CREATOR_MANUAL) {
        (void)snprintf(detail, detail_capacity, "Reserved: %s", spec->note);
        return 0;
    }
    (void)snprintf(detail, detail_capacity, "Built-in raw C creator + complete corpus verifier: %s",
                   spec->key);
    return 1;
}

int ldtm_required_modules_available(char *detail, size_t detail_capacity) {
    for (size_t i = 0U; i < ldtm_spec_count(); ++i) {
        const LdtmFilesystemSpec *spec = &ldtm_specs()[i];
        if ((spec->creator == LDTM_CREATOR_FAT12 || spec->creator == LDTM_CREATOR_FAT16 ||
             strcmp(spec->key, "ofs") == 0 || strcmp(spec->key, "sfs") == 0 ||
             spec->creator == LDTM_CREATOR_PFS3 ||
             spec->creator == LDTM_CREATOR_APFS ||
             spec->creator == LDTM_CREATOR_SWAP ||
             spec->creator == LDTM_CREATOR_ZFS ||
             spec->creator == LDTM_CREATOR_HFSPLUS ||
             spec->creator == LDTM_CREATOR_HFS ||
             spec->creator == LDTM_CREATOR_BTRFS ||
             spec->creator == LDTM_CREATOR_MINIX) &&
            !ldtm_spec_creator_available(spec, detail, detail_capacity)) return 0;
    }
    return 1;
}
