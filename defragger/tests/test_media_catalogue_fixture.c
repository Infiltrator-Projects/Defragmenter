// SPDX-License-Identifier: GPL-3.0-or-later
/* Fixed historical matrix for explicit integration/contract tests only.
 * This object is never linked into the installed Test Media application. */
#include "test_media.h"

static const LdtmFilesystemSpec LDTM_SPECS[44U] = {
    {"fat12", "LD_FAT12", 255U, 200U, LDTM_CREATOR_GENERIC_V2,
     "FAT12 Common V2 profile uses 64 KiB clusters below the 4085-cluster FAT16 threshold and "
     "retains the full 200 MiB corpus plus boundary files."},
    {"fat16", "LD_FAT16", 2048U, 200U, LDTM_CREATOR_GENERIC_V2, ""},
    {"fat32", "LD_FAT32", 2048U, 200U, LDTM_CREATOR_GENERIC_V2, ""},
    {"exfat", "LD_EXFAT", 2048U, 200U, LDTM_CREATOR_GENERIC_V2, ""},
    {"ntfs", "LD_NTFS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2, ""},
    {"ext2", "LD_EXT2", 2048U, 200U, LDTM_CREATOR_GENERIC_V2, ""},
    {"ext3", "LD_EXT3", 2048U, 200U, LDTM_CREATOR_GENERIC_V2, ""},
    {"ext4", "LD_EXT4", 2048U, 200U, LDTM_CREATOR_GENERIC_V2, ""},
    {"xfs", "LD_XFS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2, ""},
    {"btrfs", "LD_BTRFS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2, ""},
    {"ofs", "LD_OFS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Amiga Original File System DOS\\0; installed OFS driver uses the full 2 GiB qualification "
     "cap."},
    {"ffs", "LD_FFS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Amiga Fast File System DOS\\1; Common V2 driver uses the full 2 GiB qualification cap."},
    {"sfs", "LD_SFS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Amiga Smart File System SFS0 v3; Common V2 driver uses the standard 200 MiB "
     "heterogeneous fragmented corpus."},
    {"pfs3", "LD_PFS3", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Amiga Professional File System 3; Common V2 driver uses the standard 200 MiB "
     "heterogeneous fragmented corpus in the qualified small-disk subset."},
    {"hfs", "LD_HFS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2, ""},
    {"hfsplus", "LD_HFSPLUS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2, ""},
    {"minix", "LD_MINIX", 2048U, 200U, LDTM_CREATOR_GENERIC_V2, ""},
    {"ufs2", "LD_UFS2", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "UFS2 Common V2 provider must support the full corpus and exact fragmentation analysis."},
    {"zfs", "LD_ZFS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "ZFS Common V2 provider must create and verify the corpus; defragmentation is analysis-only."},
    {"apfs", "LD_APFS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "APFS Common V2 provider must create and verify the full fragmented corpus."},
    {"swap", "LD_SWAP", 2048U, 0U, LDTM_CREATOR_NO_NAMESPACE, "Swap contains no files."},
    {"ufs1", "LD_UFS1", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "UFS1 must independently support the full corpus and exact fragmentation analysis."},
    {"sfs2", "LD_SFS2", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "SFS2 must independently support the full corpus and exact fragmentation analysis."},
    /* New independent providers start with the standard disposable profile.
     * This is not qualification: the provider must accept default FORMAT,
     * the geometry and the entire real fragmented corpus before physical
     * media can be touched. No filesystem-specific fallback is permitted. */
    {"adfs", "LD_ADFS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."},
    {"bcachefs", "LD_BCACHEFS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."},
    {"befs", "LD_BEFS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."},
    {"bfs", "LD_BFS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."},
    {"cpm", "LD_CPM", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."},
    {"efs", "LD_EFS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."},
    {"f2fs", "LD_F2FS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."},
    {"fosfat", "LD_FOSFAT", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."},
    {"gfs2", "LD_GFS2", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."},
    {"hpfs", "LD_HPFS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."},
    {"jfs", "LD_JFS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."},
    {"nilfs2", "LD_NILFS2", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."},
    {"ocfs2", "LD_OCFS2", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."},
    {"omfs", "LD_OMFS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."},
    {"qnx4", "LD_QNX4", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."},
    {"qnx6", "LD_QNX6", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."},
    {"reiserfs", "LD_REISERFS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."},
    {"sysv", "LD_SYSV", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."},
    {"udf", "LD_UDF", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."},
    {"vmfs", "LD_VMFS", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."},
    {"vmfs6", "LD_VMFS6", 2048U, 200U, LDTM_CREATOR_GENERIC_V2,
     "Common V2 default format and real corpus qualification required."}};

const LdtmFilesystemSpec *ldtm_specs(void) { return LDTM_SPECS; }
size_t ldtm_spec_count(void) { return 44U; }

