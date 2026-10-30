/* SPDX-License-Identifier: GPL-3.0-or-later
 * Filesystem-UFS owns the UFS2 block, inode and allocation interpretation.
 * The host owns its deterministic corpus and supplies independently computed
 * expected file sizes and SHA-256 hashes.
 */
#ifndef IFS_UFS_PAYLOAD_VERIFIER_V1_H
#define IFS_UFS_PAYLOAD_VERIFIER_V1_H
#include <stddef.h>
#include <stdint.h>
#define IFS_UFS_PAYLOAD_VERIFIER_ABI_V1 1u
#define IFS_UFS_PAYLOAD_VERIFIER_MAX_TARGETS 32u
typedef struct {
    uint64_t size_bytes;
    char sha256[65];
} IfsUfsPayloadTarget;
typedef struct {
    uint32_t abi_version;
    uint32_t structure_bytes;
    const char *format_id;
    int (*verify_payload)(const char *path,
                          const IfsUfsPayloadTarget *targets,
                          size_t target_count,
                          char *detail, size_t detail_capacity);
} IfsUfsPayloadVerifierV1;
const IfsUfsPayloadVerifierV1 *infiltrator_filesystem_ufs_payload_verifier_v1(void);
#endif
