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
    {"fat12", "LD_FAT12", 64U, 4U, LDTM_CREATOR_FAT12, "dosfstools",
     "Small FAT12 profile; intentionally much lighter than the 200 MiB profiles."},
    {"fat16", "LD_FAT16", 512U, 200U, LDTM_CREATOR_FAT16, "dosfstools", ""},
    {"fat32", "LD_FAT32", 2048U, 200U, LDTM_CREATOR_FAT32, "dosfstools", ""},
    {"exfat", "LD_EXFAT", 2048U, 200U, LDTM_CREATOR_EXFAT, "exfatprogs", ""},
    {"ntfs", "LD_NTFS", 2048U, 200U, LDTM_CREATOR_NTFS, "ntfs-3g", ""},
    {"ext2", "LD_EXT2", 2048U, 200U, LDTM_CREATOR_EXT2, "e2fsprogs", ""},
    {"ext3", "LD_EXT3", 2048U, 200U, LDTM_CREATOR_EXT3, "e2fsprogs", ""},
    {"ext4", "LD_EXT4", 2048U, 200U, LDTM_CREATOR_EXT4, "e2fsprogs", ""},
    {"xfs", "LD_XFS", 2048U, 200U, LDTM_CREATOR_XFS, "xfsprogs", ""},
    {"btrfs", "LD_BTRFS", 2048U, 200U, LDTM_CREATOR_BTRFS, "btrfs-progs", ""},
    {"ofs", "LD_OFS", 1024U, 200U, LDTM_CREATOR_AFFS, "",
     "Amiga Old File System DOS\\0; formatted by the built-in first-party C creator."},
    {"ffs", "LD_FFS", 1024U, 200U, LDTM_CREATOR_AFFS, "",
     "Amiga Fast File System DOS\\1; formatted by the built-in first-party C creator."},
    {"sfs", "LD_SFS", 64U, 25U, LDTM_CREATOR_AFFS, "",
     "Amiga Smart File System SFS0 v3; built-in raw C creator makes a 25 MiB file with 100 extents and an intentionally unsatisfied Growth Defrag reserve."},
    {"pfs3", "LD_PFS3", 1024U, 25U, LDTM_CREATOR_PFS3, "",
     "Amiga Professional File System 3; built-in raw C creator makes a 25 MiB file with 100 extents in the qualified small-disk PFS3 subset."},
    {"hfs", "LD_HFS", 1024U, 200U, LDTM_CREATOR_HFS, "hfsutils", ""},
    {"hfsplus", "LD_HFSPLUS", 2048U, 200U, LDTM_CREATOR_HFSPLUS, "hfsprogs", ""},
    {"minix", "LD_MINIX", 1024U, 200U, LDTM_CREATOR_MINIX, "util-linux", ""},
    {"ufs", "LD_UFS", 2048U, 200U, LDTM_CREATOR_UFS, "makefs",
     "Creates a genuine UFS2/FFS image with makefs; exact fragmentation is not asserted yet."},
    {"zfs", "LD_ZFS", 4096U, 200U, LDTM_CREATOR_ZFS, "zfsutils-linux",
     "Creates an isolated single-disk ZFS v28 pool, exports it after population and requires the native exact analyser to accept it."},
    {"apfs", "LD_APFS", 4096U, 1U, LDTM_CREATOR_APFS, "",
     "Built-in first-party raw C creator manufactures the qualified bounded APFS checkpoint/spaceman/flat-tree fixture with a deliberately fragmented file."},
    {"swap", "LD_SWAP", 1024U, 0U, LDTM_CREATOR_SWAP, "util-linux",
     "Swap contains no files."}
};

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

LdtmFragmentProfile ldtm_fragment_profile(const LdtmFilesystemSpec *spec) {
    LdtmFragmentProfile profile = {64U, 2048U, 8U, 100U, 256U, 4096U, 4096U};
    if (spec != NULL && strcmp(spec->key, "fat12") == 0) {
        profile.anchors = 8U;
        profile.anchor_kib = 512U;
        profile.files = 2U;
        profile.chunks = 16U;
        profile.chunk_kib = 128U;
        profile.directory_initial = 128U;
        profile.directory_second = 128U;
    } else if (spec != NULL &&
               (strcmp(spec->key, "sfs") == 0 || strcmp(spec->key, "pfs3") == 0)) {
        profile.anchors = 0U;
        profile.anchor_kib = 0U;
        profile.files = 1U;
        profile.chunks = 100U;
        profile.chunk_kib = 256U;
        profile.directory_initial = 0U;
        profile.directory_second = 0U;
    }
    return profile;
}

uint64_t ldtm_target_payload_bytes(const LdtmFilesystemSpec *spec) {
    const LdtmFragmentProfile profile = ldtm_fragment_profile(spec);
    if (spec == NULL || spec->payload_mib == 0U) return 0U;
    return (uint64_t)profile.files * (uint64_t)profile.chunks *
           (uint64_t)profile.chunk_kib * UINT64_C(1024);
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
    (void)removable;
    (void)transport;
    return 1;
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
    switch (spec->creator) {
        case LDTM_CREATOR_FAT12:
        case LDTM_CREATOR_FAT16:
        case LDTM_CREATOR_FAT32: return "mkfs.fat";
        case LDTM_CREATOR_EXFAT: return "mkfs.exfat";
        case LDTM_CREATOR_NTFS: return "mkntfs";
        case LDTM_CREATOR_EXT2: return "mkfs.ext2";
        case LDTM_CREATOR_EXT3: return "mkfs.ext3";
        case LDTM_CREATOR_EXT4: return "mkfs.ext4";
        case LDTM_CREATOR_XFS: return "mkfs.xfs";
        case LDTM_CREATOR_BTRFS: return "mkfs.btrfs";
        case LDTM_CREATOR_AFFS:
            if (strcmp(spec->key, "ofs") == 0) return "/usr/lib/linux-defragger/test-media-mkfs-ofs";
            if (strcmp(spec->key, "ffs") == 0) return "/usr/lib/linux-defragger/test-media-mkfs-ffs";
            return NULL;
        case LDTM_CREATOR_PFS3: return NULL;
        case LDTM_CREATOR_HFS: return "hformat";
        case LDTM_CREATOR_HFSPLUS: return "mkfs.hfsplus";
        case LDTM_CREATOR_MINIX: return "mkfs.minix";
        case LDTM_CREATOR_UFS: return "makefs";
        case LDTM_CREATOR_ZFS: return "zpool";
        case LDTM_CREATOR_APFS: return NULL;
        case LDTM_CREATOR_SWAP: return "mkswap";
        case LDTM_CREATOR_MANUAL: return NULL;
    }
    return NULL;
}

int ldtm_program_available(const char *program) {
    static const char *const directories[] = {
        "/usr/sbin", "/usr/bin", "/sbin", "/bin", "/usr/local/sbin", "/usr/local/bin"
    };
    size_t index;
    char path[PATH_MAX];
    const char *path_env;
    char *copy;
    char *token;
    char *saveptr = NULL;
    if (program == NULL || *program == '\0') return 0;
    if (strchr(program, '/') != NULL) return access(program, X_OK) == 0;
    for (index = 0U; index < sizeof(directories) / sizeof(directories[0]); ++index) {
        if (snprintf(path, sizeof(path), "%s/%s", directories[index], program) > 0 &&
            access(path, X_OK) == 0) return 1;
    }
    path_env = getenv("PATH");
    if (path_env == NULL) return 0;
    copy = strdup(path_env);
    if (copy == NULL) return 0;
    token = strtok_r(copy, ":", &saveptr);
    while (token != NULL) {
        if (snprintf(path, sizeof(path), "%s/%s", token, program) > 0 &&
            access(path, X_OK) == 0) {
            free(copy);
            return 1;
        }
        token = strtok_r(NULL, ":", &saveptr);
    }
    free(copy);
    return 0;
}

int ldtm_spec_creator_available(const LdtmFilesystemSpec *spec,
                                char *detail, size_t detail_capacity) {
    const char *program;
    if (detail == NULL || detail_capacity == 0U || spec == NULL) return 0;
    if (spec->creator == LDTM_CREATOR_AFFS && strcmp(spec->key, "ofs") == 0) {
        (void)snprintf(detail, detail_capacity, "Built-in raw C creator + payload engine: Amiga DOS\\0 OFS");
        return 1;
    }
    if (spec->creator == LDTM_CREATOR_AFFS && strcmp(spec->key, "ffs") == 0) {
        (void)snprintf(detail, detail_capacity, "Built-in raw C creator + payload engine: Amiga DOS\\1 FFS");
        return 1;
    }
    if (spec->creator == LDTM_CREATOR_AFFS && strcmp(spec->key, "sfs") == 0) {
        (void)snprintf(detail, detail_capacity, "Built-in raw C creator + payload engine: Amiga SFS0 v3, 100-fragment native fixture");
        return 1;
    }
    if (spec->creator == LDTM_CREATOR_PFS3) {
        (void)snprintf(detail, detail_capacity,
                       "Built-in raw C creator + payload engine: Amiga PFS3, 100-fragment native fixture");
        return 1;
    }
    if (spec->creator == LDTM_CREATOR_APFS) {
        (void)snprintf(detail, detail_capacity,
                       "Built-in raw C creator + payload verifier: bounded APFS checkpoint/spaceman/flat-tree fixture");
        return 1;
    }
    if (spec->creator == LDTM_CREATOR_MANUAL) {
        (void)snprintf(detail, detail_capacity, "Reserved: %s", spec->note);
        return 0;
    }
    program = ldtm_creator_program(spec);
    if (program != NULL && ldtm_program_available(program)) {
        (void)snprintf(detail, detail_capacity, "Available: %s", program);
        return 1;
    }
    if (spec->package_hint != NULL && *spec->package_hint != '\0') {
        (void)snprintf(detail, detail_capacity, "Missing: %s (package: %s)",
                       program != NULL ? program : "creator", spec->package_hint);
    } else {
        (void)snprintf(detail, detail_capacity, "Missing: %s",
                       program != NULL ? program : "creator");
    }
    return 0;
}

int ldtm_canonicalize_device(const char *input, char *output, size_t output_capacity) {
    return infiltratr_realpath_copy(input, output, output_capacity) ? 0 : -1;
}
