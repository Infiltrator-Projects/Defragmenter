// SPDX-License-Identifier: GPL-3.0-or-later
#include "ext_native.h"

#include "ld_io.h"
#include "ld_runtime.h"
#include "ld_stop.h"

#include "infiltratr/arithmetic.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef LINUX_S_IFMT
#define LINUX_S_IFMT 0170000
#endif
#ifndef LINUX_S_IFDIR
#define LINUX_S_IFDIR 0040000
#endif
#ifndef LINUX_S_IFREG
#define LINUX_S_IFREG 0100000
#endif

typedef struct {
    int64_t logical;
    uint64_t physical;
} ExtBlockRef;
typedef struct {
    ExtBlockRef *items;
    size_t count;
    size_t capacity;
} ExtBlockVec;
static void block_push(ExtBlockVec *vec, int64_t logical, uint64_t physical) {
    if (physical == 0U) return;
    if (vec->count == SIZE_MAX ||
        !infiltratr_array_reserve((void **)&vec->items, &vec->capacity,
                                  sizeof(*vec->items), vec->count + 1U, 32U))
        ld_die("cannot grow EXT block vector");
    vec->items[vec->count++] = (ExtBlockRef){logical, physical};
}
static void block_free(ExtBlockVec *vec) {
    free(vec->items); memset(vec, 0, sizeof(*vec));
}
static int collect_payload(ExtFs *fs, uint32_t ino, int64_t logical,
                           uint64_t *physical, bool mutable_mapping,
                           void *private_data, char **error) {
    (void)fs; (void)ino; (void)mutable_mapping; (void)error;
    block_push((ExtBlockVec *)private_data, logical, *physical);
    return 0;
}
static int collect_inode_blocks(ExtFs *fs, ExtInode *inode,
                                ExtBlockVec *blocks, char **error) {
    return ext_fs_iterate_payload(fs, inode, false, collect_payload,
                                  blocks, error);
}

static bool allocation_is_contiguous(const ExtBlockVec *blocks, uint64_t *last_block) {
    if (blocks->count == 0) {
        if (last_block != NULL) *last_block = 0;
        return true;
    }
    uint64_t minimum = UINT64_MAX, maximum = 0;
    for (size_t index = 0; index < blocks->count; ++index) {
        uint64_t physical = blocks->items[index].physical;
        if (physical < minimum) minimum = physical;
        if (physical > maximum) maximum = physical;
    }
    if (last_block != NULL) *last_block = maximum;
    return maximum >= minimum && maximum - minimum + 1U == blocks->count;
}

static void add_data_ranges(const ExtBlockVec *blocks, ExtRangeVec *ranges) {
    for (size_t index = 0U; index < blocks->count; ++index)
        ext_range_push(ranges, blocks->items[index].physical,
                       blocks->items[index].physical + 1U);
}
static int bitmap_free(ExtFs *fs, uint64_t block, bool *is_free,
                       char **error) {
    bool allocated = false;
    if (ext_fs_block_allocated(fs, block, &allocated, error) != 0) return -1;
    *is_free = !allocated;
    return 0;
}
static int scan_free_ranges(ExtFs *fs, const ExtGeometry *geometry,
                            ExtRangeVec *ranges, char **error) {
    bool in_run = false; uint64_t start = 0U;
    for (uint64_t block = geometry->first_data_block;
         block < geometry->total_blocks; ++block) {
        bool free_block = false;
        if (bitmap_free(fs, block, &free_block, error) != 0) return -1;
        if (free_block && !in_run) { start = block; in_run = true; }
        else if (!free_block && in_run) {
            ext_range_push(ranges, start, block); in_run = false;
        }
    }
    if (in_run) ext_range_push(ranges, start, geometry->total_blocks);
    return 0;
}
typedef struct {
    const ExtGeometry *geometry;
    ExtCatalogue *catalogue;
} ScanContext;
static int scan_inode(ExtFs *fs, ExtInode *inode, void *private_data,
                      char **error) {
    ScanContext *context = private_data;
    ExtCatalogue *catalogue = context->catalogue;
    if (inode->mode == 0U || inode->links == 0U) return 0;
    unsigned kind = (unsigned)inode->mode & LINUX_S_IFMT;
    if (kind != LINUX_S_IFREG && kind != LINUX_S_IFDIR) return 0;
    catalogue->inodes_scanned++;
    ExtBlockVec blocks = {0}; char *local_error = NULL;
    if (collect_inode_blocks(fs, inode, &blocks, &local_error) != 0) {
        catalogue->malformed_inodes++; free(local_error); block_free(&blocks);
        return 0;
    }
    uint64_t last_allocation = 0U;
    bool contiguous = allocation_is_contiguous(&blocks, &last_allocation);
    bool fragmented = blocks.count > 0U && !contiguous;
    if (inode->number < ext_fs_first_inode(fs) && inode->number != EXT_ROOT_INO) {
        block_free(&blocks); return 0;
    }
    if (kind == LINUX_S_IFDIR) {
        catalogue->directories++; add_data_ranges(&blocks, &catalogue->directory_ranges);
        if (fragmented) catalogue->fragmented_directories++;
    } else {
        catalogue->regular_files++; if (fragmented) catalogue->fragmented_files++;
        if (blocks.count != 0U) {
            if (!contiguous) catalogue->growth_10_satisfied = false;
            else {
                uint64_t reserve = ((uint64_t)blocks.count + 9U) / 10U;
                for (uint64_t offset = 1U; offset <= reserve; ++offset) {
                    bool free_block = false;
                    if (last_allocation + offset >= context->geometry->total_blocks ||
                        bitmap_free(fs, last_allocation + offset, &free_block, error) != 0 ||
                        !free_block) {
                        catalogue->growth_10_satisfied = false;
                        if (error != NULL && *error != NULL) { block_free(&blocks); return -1; }
                        break;
                    }
                }
            }
        }
    }
    if (fragmented) add_data_ranges(&blocks, &catalogue->fragmented_ranges);
    block_free(&blocks); return 0;
}

int ext_scan_catalogue(const char *path, ExtGeometry *geometry,
                       ExtCatalogue *catalogue, char **error) {
    memset(catalogue, 0, sizeof(*catalogue));
    catalogue->growth_10_satisfied = true;
    if (ext_read_geometry(path, geometry, error) != 0) return -1;
    ExtFs *fs = NULL;
    if (ext_open_fs(path, false, &fs, error) != 0) return -1;
    int result = -1;
    if (ext_validate_metadata(fs, false, error) != 0 ||
        scan_free_ranges(fs, geometry, &catalogue->free_ranges, error) != 0)
        goto done;
    ScanContext context = {.geometry = geometry, .catalogue = catalogue};
    if (ext_fs_foreach_inode(fs, scan_inode, &context, error) != 0) goto done;
    ext_range_sort_merge(&catalogue->free_ranges);
    ext_range_sort_merge(&catalogue->fragmented_ranges);
    ext_range_sort_merge(&catalogue->directory_ranges);
    if (catalogue->regular_files == 0U) catalogue->growth_10_satisfied = false;
    result = 0;
done:
    ext_fs_close(fs);
    if (result != 0) ext_catalogue_free(catalogue);
    return result;
}

void ext_catalogue_free(ExtCatalogue *catalogue) {
    if (catalogue == NULL) return;
    ext_range_free(&catalogue->free_ranges);
    ext_range_free(&catalogue->fragmented_ranges);
    ext_range_free(&catalogue->directory_ranges);
    memset(catalogue, 0, sizeof(*catalogue));
}

static int sql_exec(sqlite3 *db, const char *sql, char **error) {
    char *message = NULL;
    int code = sqlite3_exec(db, sql, NULL, NULL, &message);
    if (code != SQLITE_OK) {
        ext_set_error(error, "EXT plan database: %s", message != NULL ? message : sqlite3_errmsg(db));
        sqlite3_free(message);
        return -1;
    }
    return 0;
}

int ext_open_plan_db(const char *path, bool create, sqlite3 **db, char **error) {
    if (create) {
        (void)unlink(path);
        char wal[4096], shm[4096];
        if (snprintf(wal, sizeof(wal), "%s-wal", path) > 0) (void)unlink(wal);
        if (snprintf(shm, sizeof(shm), "%s-shm", path) > 0) (void)unlink(shm);
    }
    int flags = create ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)
                       : SQLITE_OPEN_READWRITE;
#ifdef SQLITE_OPEN_NOFOLLOW
    flags |= SQLITE_OPEN_NOFOLLOW;
#endif
    int code = sqlite3_open_v2(path, db, flags, NULL);
    if (code != SQLITE_OK) {
        ext_set_error(error, "opening EXT plan database %s: %s", path,
                      *db != NULL ? sqlite3_errmsg(*db) : "SQLite error");
        if (*db != NULL) sqlite3_close(*db);
        *db = NULL;
        return -1;
    }
    if (sql_exec(*db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;", error) != 0)
        return -1;
    if (!create) return 0;
    return sql_exec(*db,
        "CREATE TABLE objects ("
        "inode INTEGER PRIMARY KEY,kind TEXT NOT NULL,mode INTEGER NOT NULL,"
        "links INTEGER NOT NULL,size INTEGER NOT NULL,data_block_count INTEGER NOT NULL,"
        "allocation_block_count INTEGER NOT NULL,payload_sha256 BLOB NOT NULL,sort_class INTEGER NOT NULL);"
        "CREATE TABLE blocks (inode INTEGER NOT NULL,sequence INTEGER NOT NULL,logical INTEGER NOT NULL,"
        "is_data INTEGER NOT NULL,old INTEGER PRIMARY KEY,target INTEGER,placed INTEGER NOT NULL DEFAULT 0,"
        "UNIQUE(inode,sequence));"
        "CREATE INDEX blocks_inode ON blocks(inode,sequence);"
        "CREATE TABLE reserves (inode INTEGER NOT NULL,start INTEGER NOT NULL,length INTEGER NOT NULL);"
        "CREATE TABLE spaces (start INTEGER PRIMARY KEY,length INTEGER NOT NULL);"
        "CREATE TABLE metadata (key TEXT PRIMARY KEY,value TEXT NOT NULL);", error);
}

static int payload_digest(int fd, uint32_t block_size, const ExtBlockVec *blocks,
                          uint8_t output[SHA256_DIGEST_LENGTH], char **error) {
    SHA256_CTX digest;
    if (SHA256_Init(&digest) != 1) {
        ext_set_error(error, "initializing EXT payload digest failed");
        return -1;
    }
    uint8_t *buffer = ld_xmalloc(block_size);
    for (size_t index = 0; index < blocks->count; ++index) {
                uint64_t physical = blocks->items[index].physical;
        ssize_t got = ld_pread_full(fd, buffer, block_size, physical * block_size);
        if (got < 0 || (size_t)got != block_size) {
            ext_set_error(error, "short read while hashing EXT payload data");
            free(buffer);
            return -1;
        }
        uint8_t logical[8];
        uint64_t value = (uint64_t)blocks->items[index].logical;
        for (unsigned byte = 0; byte < 8U; ++byte) logical[byte] = (uint8_t)(value >> (byte * 8U));
        if (SHA256_Update(&digest, logical, sizeof(logical)) != 1 ||
            SHA256_Update(&digest, buffer, block_size) != 1) {
            ext_set_error(error, "updating EXT payload digest failed");
            free(buffer);
            return -1;
        }
    }
    free(buffer);
    if (SHA256_Final(output, &digest) != 1) {
        ext_set_error(error, "finalizing EXT payload digest failed");
        return -1;
    }
    return 0;
}

static int bind_int64(sqlite3_stmt *stmt, int index, uint64_t value, char **error) {
    if (value > INT64_MAX) {
        ext_set_error(error, "EXT block number exceeds SQLite signed integer range");
        return -1;
    }
    return sqlite3_bind_int64(stmt, index, (sqlite3_int64)value) == SQLITE_OK ? 0 : -1;
}

typedef struct {
    int raw_fd; sqlite3 *db; const ExtGeometry *geometry;
    sqlite3_stmt *insert_object; sqlite3_stmt *insert_block;
    uint64_t *movable; uint64_t indexed;
} PlanContext;
static int plan_inode(ExtFs *fs, ExtInode *inode, void *private_data,
                      char **error) {
    PlanContext *context = private_data;
    if (ld_stop_requested()) { ext_set_error(error, "stop requested before EXT source commit"); return -1; }
    unsigned kind = (unsigned)inode->mode & LINUX_S_IFMT;
    if (inode->mode == 0U || inode->links == 0U ||
        (kind != LINUX_S_IFREG && kind != LINUX_S_IFDIR)) return 0;
    if (inode->number < ext_fs_first_inode(fs) && inode->number != EXT_ROOT_INO) return 0;
    ExtBlockVec blocks = {0};
    /* Payload moves; extent-index/indirect metadata stays physically fixed.
       The native disk layer rewrites only physical payload references. */
    if (collect_inode_blocks(fs, inode, &blocks, error) != 0) { block_free(&blocks); return -1; }
    uint8_t digest[SHA256_DIGEST_LENGTH];
    if (payload_digest(context->raw_fd, context->geometry->block_size,
                       &blocks, digest, error) != 0) { block_free(&blocks); return -1; }
    int sort_class = inode->number == EXT_ROOT_INO ? 0 : (kind == LINUX_S_IFDIR ? 1 : 2);
    sqlite3_stmt *object=context->insert_object;
    sqlite3_reset(object); sqlite3_clear_bindings(object);
    sqlite3_bind_int64(object,1,(sqlite3_int64)inode->number);
    sqlite3_bind_text(object,2,kind==LINUX_S_IFDIR?"directory":"file",-1,SQLITE_STATIC);
    sqlite3_bind_int(object,3,inode->mode); sqlite3_bind_int(object,4,inode->links);
    if (bind_int64(object,5,inode->size,error)!=0) { block_free(&blocks); return -1; }
    sqlite3_bind_int64(object,6,(sqlite3_int64)blocks.count);
    sqlite3_bind_int64(object,7,(sqlite3_int64)blocks.count);
    sqlite3_bind_blob(object,8,digest,SHA256_DIGEST_LENGTH,SQLITE_TRANSIENT);
    sqlite3_bind_int(object,9,sort_class);
    if(sqlite3_step(object)!=SQLITE_DONE){
        ext_set_error(error,"cataloguing EXT inode %u: %s",inode->number,sqlite3_errmsg(context->db));
        block_free(&blocks); return -1;
    }
    for(size_t index=0U;index<blocks.count;++index){
        sqlite3_stmt *block=context->insert_block;
        sqlite3_reset(block);sqlite3_clear_bindings(block);
        sqlite3_bind_int64(block,1,(sqlite3_int64)inode->number);
        sqlite3_bind_int64(block,2,(sqlite3_int64)index);
        sqlite3_bind_int64(block,3,(sqlite3_int64)blocks.items[index].logical);
        sqlite3_bind_int(block,4,1);
        if(bind_int64(block,5,blocks.items[index].physical,error)!=0){block_free(&blocks);return -1;}
        if(sqlite3_step(block)!=SQLITE_DONE){
            ext_set_error(error,"shared or duplicate EXT allocation block %llu is outside the bounded native mover",
                          (unsigned long long)blocks.items[index].physical);
            block_free(&blocks);return -1;
        }
    }
    *context->movable+=(uint64_t)blocks.count;context->indexed++;block_free(&blocks);
    if((context->indexed%256U)==0U &&
       sql_exec(context->db,"COMMIT; BEGIN IMMEDIATE",error)!=0) return -1;
    return 0;
}
int ext_catalog_plan(ExtFs *fs, int raw_fd, sqlite3 *db,
                     const ExtGeometry *geometry, uint64_t *movable,
                     char **error) {
    *movable=0U;
    sqlite3_stmt *insert_object=NULL,*insert_block=NULL,*meta=NULL;
    if(sqlite3_prepare_v2(db,"INSERT INTO objects VALUES (?,?,?,?,?,?,?,?,?)",-1,&insert_object,NULL)!=SQLITE_OK ||
       sqlite3_prepare_v2(db,"INSERT INTO blocks(inode,sequence,logical,is_data,old) VALUES (?,?,?,?,?)",-1,&insert_block,NULL)!=SQLITE_OK ||
       sqlite3_prepare_v2(db,"INSERT INTO metadata VALUES (?,?)",-1,&meta,NULL)!=SQLITE_OK){
        ext_set_error(error,"preparing EXT plan database statements: %s",sqlite3_errmsg(db));goto fail;
    }
    if(sql_exec(db,"BEGIN IMMEDIATE",error)!=0)goto fail;
    PlanContext context={.raw_fd=raw_fd,.db=db,.geometry=geometry,.insert_object=insert_object,.insert_block=insert_block,.movable=movable};
    if(ext_fs_foreach_inode(fs,plan_inode,&context,error)!=0)goto rollback;
    if(*movable==0U){ext_set_error(error,"the EXT filesystem contains no movable file or directory blocks");goto rollback;}
    const char *keys[2]={"block_size","total_blocks"};uint64_t values[2]={geometry->block_size,geometry->total_blocks};char rendered[64];
    for(size_t index=0U;index<2U;++index){
        sqlite3_reset(meta);sqlite3_clear_bindings(meta);sqlite3_bind_text(meta,1,keys[index],-1,SQLITE_STATIC);
        (void)snprintf(rendered,sizeof(rendered),"%llu",(unsigned long long)values[index]);
        sqlite3_bind_text(meta,2,rendered,-1,SQLITE_TRANSIENT);
        if(sqlite3_step(meta)!=SQLITE_DONE){ext_set_error(error,"writing EXT plan metadata: %s",sqlite3_errmsg(db));goto rollback;}
    }
    if(sql_exec(db,"COMMIT",error)!=0)goto fail;
    sqlite3_finalize(insert_object);sqlite3_finalize(insert_block);sqlite3_finalize(meta);return 0;
rollback:
    (void)sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);
fail:
    sqlite3_finalize(insert_object);sqlite3_finalize(insert_block);sqlite3_finalize(meta);return -1;
}
