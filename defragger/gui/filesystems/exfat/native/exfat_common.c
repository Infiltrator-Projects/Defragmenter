// SPDX-License-Identifier: GPL-3.0-or-later
#include "exfat_native.h"

#include "ld_device.h"
#include "ld_io.h"
#include "ld_runtime.h"

#include "infiltratr/endian.h"
#include "infiltratr/arithmetic.h"
#include "infiltratr/posix.h"
#include "infiltratr/utf8.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

void exfat_set_error(char **error, const char *format, ...) { if (error == NULL || *error != NULL) return; va_list ap; va_start(ap, format); va_list copy; va_copy(copy, ap); int n=vsnprintf(NULL,0,format,copy); va_end(copy); if(n<0){va_end(ap);return;} *error=ld_xmalloc((size_t)n+1U); (void)vsnprintf(*error,(size_t)n+1U,format,ap); va_end(ap); }
uint16_t exfat_u16(const void *data,size_t off){return infiltratr_load_le16((const uint8_t*)data+off);} uint32_t exfat_u32(const void *data,size_t off){return infiltratr_load_le32((const uint8_t*)data+off);} uint64_t exfat_u64(const void *data,size_t off){return infiltratr_load_le64((const uint8_t*)data+off);}
void exfat_put_u16(void *data,size_t off,uint16_t v){infiltratr_store_le16((uint8_t*)data+off,v);} void exfat_put_u32(void *data,size_t off,uint32_t v){infiltratr_store_le32((uint8_t*)data+off,v);}
/*
 * exFAT entry-set checksum: bytes 2 and 3 of the primary entry contain the
 * checksum itself and are excluded from the rotating checksum calculation.
 */
uint16_t exfat_entry_checksum(const uint8_t *data,size_t bytes){uint16_t c=0;for(size_t i=0;i<bytes;++i){if(i==2U||i==3U)continue;c=(uint16_t)((c>>1)|((c&1U)<<15));c=(uint16_t)(c+data[i]);}return c;}
uint32_t exfat_table_checksum(const uint8_t *data,size_t bytes){uint32_t c=0;for(size_t i=0;i<bytes;++i)c=((c&1U)<<31)+(c>>1)+data[i];return c;}
/*
 * Boot checksum covers the first eleven sectors but excludes VolumeFlags
 * (106-107) and PercentInUse (112), which may change without invalidating the
 * boot-region checksum.
 */
uint32_t exfat_boot_checksum(const uint8_t *region,uint32_t bps){uint32_t c=0;uint64_t limit=(uint64_t)bps*11U;for(uint64_t i=0;i<limit;++i){if(i==106U||i==107U||i==112U)continue;c=((c&1U)<<31)+(c>>1)+region[i];}return c;}

int exfat_clusters_push(ExfatClusterVec *v,uint32_t c){if(v->count==SIZE_MAX||!infiltratr_array_reserve((void**)&v->items,&v->capacity,sizeof(*v->items),v->count+1U,16U))ld_die("cannot grow exFAT cluster list");v->items[v->count++]=c;return 0;}
void exfat_clusters_free(ExfatClusterVec *v){if(v==NULL)return;free(v->items);memset(v,0,sizeof(*v));}
static int cluster_copy(ExfatClusterVec *dst,const ExfatClusterVec *src){for(size_t i=0;i<src->count;++i)exfat_clusters_push(dst,src->items[i]);return 0;}
size_t exfat_fragments(const ExfatClusterVec *v){if(v==NULL||v->count==0)return 0;size_t f=1;for(size_t i=1;i<v->count;++i)if(v->items[i]!=v->items[i-1]+1U)f++;return f;}

static int validate_boot_region(const uint8_t *region,uint32_t bps,const char *name,char **error){if(memcmp(region+3,"EXFAT   ",8)!=0){exfat_set_error(error,"%s exFAT boot sector is invalid",name);return -1;}uint32_t expected=exfat_boot_checksum(region,bps);for(uint32_t off=0;off<bps;off+=4U)if(exfat_u32(region,(size_t)11U*bps+off)!=expected){exfat_set_error(error,"%s exFAT boot-region checksum is invalid",name);return -1;}return 0;}

int exfat_open_volume(const char *path,bool writable,bool allow_dirty,ExfatVolume *v,char **error){memset(v,0,sizeof(*v));v->fd=-1;char *real=realpath(path,NULL);if(real==NULL){exfat_set_error(error,"cannot resolve exFAT target: %s",strerror(errno));return -1;}struct stat st;if(stat(real,&st)!=0||(!S_ISREG(st.st_mode)&&!S_ISBLK(st.st_mode))){free(real);exfat_set_error(error,"exFAT target must be a block device or regular image");return -1;}if(writable&&S_ISBLK(st.st_mode)&&ld_path_is_mounted(real)){free(real);exfat_set_error(error,"exFAT target is mounted; raw mutation requires an unmounted filesystem");return -1;}int flags=(writable?O_RDWR:O_RDONLY)|O_CLOEXEC;if(writable&&S_ISBLK(st.st_mode))flags|=O_EXCL;
#ifdef O_NOFOLLOW
flags|=O_NOFOLLOW;
#endif
int fd=open(real,flags);if(fd<0){free(real);exfat_set_error(error,"cannot open exFAT target: %s",strerror(errno));return -1;}struct stat opened;if(fstat(fd,&opened)!=0||(st.st_mode&S_IFMT)!=(opened.st_mode&S_IFMT)||st.st_dev!=opened.st_dev||st.st_ino!=opened.st_ino||(S_ISBLK(st.st_mode)&&st.st_rdev!=opened.st_rdev)){close(fd);free(real);exfat_set_error(error,"exFAT target identity changed between validation and open");return -1;}if(writable&&S_ISBLK(opened.st_mode)&&ld_device_number_is_mounted(opened.st_rdev)){close(fd);free(real);exfat_set_error(error,"exFAT target became mounted while opening for raw mutation");return -1;}st=opened;if(writable&&flock(fd,LOCK_EX|LOCK_NB)!=0){close(fd);free(real);exfat_set_error(error,"cannot lock exFAT target exclusively: %s",strerror(errno));return -1;}uint8_t boot[512];ssize_t got=ld_pread_full(fd,boot,sizeof(boot),0);if(got!=(ssize_t)sizeof(boot)||memcmp(boot+3,"EXFAT   ",8)!=0){close(fd);free(real);exfat_set_error(error,"not an exFAT volume");return -1;}uint8_t bps_shift=boot[108],spc_shift=boot[109];if(bps_shift<9U||bps_shift>12U||spc_shift>25U){close(fd);free(real);exfat_set_error(error,"unsupported exFAT sector or cluster geometry");return -1;}uint32_t bps=UINT32_C(1)<<bps_shift;uint64_t spc64=UINT64_C(1)<<spc_shift;if(spc64>UINT32_MAX||spc64*bps>32U*1024U*1024U){close(fd);free(real);exfat_set_error(error,"unsupported exFAT cluster size");return -1;}uint32_t spc=(uint32_t)spc64,cluster=(uint32_t)(spc64*bps);uint64_t volume_sectors=exfat_u64(boot,72);uint32_t fat_off=exfat_u32(boot,80),fat_len=exfat_u32(boot,84),heap_off=exfat_u32(boot,88),clusters=exfat_u32(boot,92),root=exfat_u32(boot,96);uint16_t volume_flags=exfat_u16(boot,106);if(clusters<1U||root<2U||root>=clusters+2U||boot[110]!=1U){close(fd);free(real);exfat_set_error(error,boot[110]!=1U?"TexFAT/two-FAT volumes are not yet safe for direct rewriting":"invalid exFAT geometry");return -1;}if((volume_flags&EXFAT_MEDIA_FAILURE)!=0U){close(fd);free(real);exfat_set_error(error,"exFAT reports media failure");return -1;}if((volume_flags&EXFAT_VOLUME_DIRTY)!=0U&&!allow_dirty){close(fd);free(real);exfat_set_error(error,"exFAT volume is marked dirty; complete filesystem checking first");return -1;}uint64_t device_size=S_ISREG(st.st_mode)?(uint64_t)st.st_size:0;if(S_ISBLK(st.st_mode)){LdDevice d=ld_device_open(real,false);device_size=d.size_bytes;ld_device_close(&d);}if(volume_sectors==0||volume_sectors>UINT64_MAX/bps||volume_sectors*bps>device_size){close(fd);free(real);exfat_set_error(error,"exFAT volume boundary exceeds the target device");return -1;}uint64_t fat_offset=(uint64_t)fat_off*bps,fat_length=(uint64_t)fat_len*bps,heap_offset=(uint64_t)heap_off*bps;if(fat_length<(uint64_t)(clusters+2U)*4U||fat_offset+fat_length>volume_sectors*bps||heap_offset+(uint64_t)clusters*cluster>volume_sectors*bps){close(fd);free(real);exfat_set_error(error,"exFAT FAT or cluster heap exceeds the volume boundary");return -1;}size_t boot_len=(size_t)bps*24U;uint8_t *regions=ld_xmalloc(boot_len);if(ld_pread_full(fd,regions,boot_len,0)!=(ssize_t)boot_len||validate_boot_region(regions,bps,"main",error)!=0||validate_boot_region(regions+(size_t)bps*12U,bps,"backup",error)!=0){free(regions);close(fd);free(real);if(error&&*error==NULL)exfat_set_error(error,"short exFAT boot region");return -1;}uint8_t *fat=ld_xmalloc((size_t)fat_length);if(ld_pread_full(fd,fat,(size_t)fat_length,fat_offset)!=(ssize_t)fat_length){free(fat);free(regions);close(fd);free(real);exfat_set_error(error,"short exFAT FAT read");return -1;}v->path=real;v->fd=fd;v->writable=writable;v->device_size=device_size;v->volume_bytes=volume_sectors*bps;v->bytes_per_sector=bps;v->sectors_per_cluster=spc;v->cluster_size=cluster;v->fat_offset_sectors=fat_off;v->fat_length_sectors=fat_len;v->heap_offset_sectors=heap_off;v->cluster_count=clusters;v->root_cluster=root;v->serial=exfat_u32(boot,100);v->revision=exfat_u16(boot,104);v->volume_flags=volume_flags;v->number_of_fats=boot[110];v->percent_in_use=boot[112];v->fat_offset=fat_offset;v->fat_length=fat_length;v->heap_offset=heap_offset;v->fat=fat;v->boot_regions=regions;return 0;}
void exfat_close_volume(ExfatVolume *v){if(v==NULL)return;if(v->fd>=0){if(v->writable)(void)flock(v->fd,LOCK_UN);close(v->fd);}free(v->path);free(v->fat);free(v->boot_regions);memset(v,0,sizeof(*v));v->fd=-1;}
uint64_t exfat_cluster_offset(const ExfatVolume *v,uint32_t cluster){return v->heap_offset+(uint64_t)(cluster-2U)*v->cluster_size;}
int exfat_read_cluster(const ExfatVolume*v,uint32_t cluster,void*buffer,char**error){if(cluster<2U||cluster>=v->cluster_count+2U){exfat_set_error(error,"exFAT cluster %u is outside the heap",cluster);return -1;}if(ld_pread_full(v->fd,buffer,v->cluster_size,exfat_cluster_offset(v,cluster))!=(ssize_t)v->cluster_size){exfat_set_error(error,"short exFAT cluster read");return -1;}return 0;}
int exfat_write_cluster(const ExfatVolume*v,uint32_t cluster,const void*buffer,char**error){if(!v->writable){exfat_set_error(error,"exFAT volume is read-only");return -1;}if(cluster<2U||cluster>=v->cluster_count+2U){exfat_set_error(error,"exFAT cluster %u is outside the heap",cluster);return -1;}if(ld_pwrite_full(v->fd,buffer,v->cluster_size,exfat_cluster_offset(v,cluster))!=(ssize_t)v->cluster_size){exfat_set_error(error,"short exFAT cluster write");return -1;}return 0;}
static uint32_t fat_get(const ExfatVolume*v,uint32_t cluster){return exfat_u32(v->fat,(size_t)cluster*4U);}
/*
 * NoFatChain streams are validated as one contiguous heap run and never follow
 * FAT entries. FAT-chained streams use a visit bitmap so cycles, bad clusters,
 * short chains and chains longer than DataLength fail closed.
 */
int exfat_chain(const ExfatVolume*v,uint32_t first,uint64_t count,bool count_known,bool contiguous,ExfatClusterVec*out,char**error){memset(out,0,sizeof(*out));if(first==0U){if(count_known&&count!=0U){exfat_set_error(error,"zero exFAT first cluster has nonzero length");return -1;}return 0;}if(first<2U||first>=v->cluster_count+2U){exfat_set_error(error,"exFAT first cluster is outside the heap");return -1;}if(contiguous){if(!count_known){exfat_set_error(error,"contiguous exFAT stream has unknown length");return -1;}if(count>(uint64_t)v->cluster_count||first+(uint64_t)count>v->cluster_count+2ULL){exfat_set_error(error,"contiguous exFAT stream exceeds the heap");return -1;}for(uint64_t i=0;i<count;++i)exfat_clusters_push(out,first+(uint32_t)i);return 0;}uint8_t *seen=calloc((v->cluster_count+7U)/8U,1);if(seen==NULL){exfat_set_error(error,"allocating exFAT chain guard failed");return -1;}uint32_t cluster=first;while(cluster>=2U&&cluster<EXFAT_EOC_MIN){if(cluster>=v->cluster_count+2U){exfat_set_error(error,"exFAT FAT chain exceeds the heap");goto fail;}uint32_t bit=cluster-2U;if((seen[bit>>3U]&(uint8_t)(1U<<(bit&7U)))!=0U){exfat_set_error(error,"exFAT FAT loop");goto fail;}seen[bit>>3U]|=(uint8_t)(1U<<(bit&7U));exfat_clusters_push(out,cluster);if(count_known&&out->count==(size_t)count){uint32_t next=fat_get(v,cluster);if(next<EXFAT_EOC_MIN){exfat_set_error(error,"exFAT FAT chain is longer than DataLength");goto fail;}free(seen);return 0;}cluster=fat_get(v,cluster);if(cluster==EXFAT_BAD_CLUSTER){exfat_set_error(error,"exFAT chain contains a bad cluster");goto fail;}}if(count_known&&out->count!=(size_t)count){exfat_set_error(error,"short exFAT FAT chain");goto fail;}free(seen);return 0;fail:free(seen);exfat_clusters_free(out);return -1;}
int exfat_read_stream(const ExfatVolume*v,const ExfatClusterVec*clusters,uint64_t length,uint8_t**data,char**error){if(length>SIZE_MAX){exfat_set_error(error,"exFAT stream exceeds addressable memory");return -1;}uint8_t*out=ld_xmalloc((size_t)(length?length:1U));uint64_t remaining=length,position=0;uint8_t*buffer=ld_xmalloc(v->cluster_size);for(size_t i=0;i<clusters->count&&remaining>0;++i){if(exfat_read_cluster(v,clusters->items[i],buffer,error)!=0){free(buffer);free(out);return -1;}size_t take=remaining<v->cluster_size?(size_t)remaining:v->cluster_size;memcpy(out+(size_t)position,buffer,take);position+=take;remaining-=take;}free(buffer);if(remaining!=0){free(out);exfat_set_error(error,"short exFAT stream");return -1;}*data=out;return 0;}

static ExfatObject *object_push(ExfatObjectVec *v){if(v->count==SIZE_MAX||!infiltratr_array_reserve((void**)&v->items,&v->capacity,sizeof(*v->items),v->count+1U,32U))ld_die("cannot grow exFAT object list");ExfatObject*o=&v->items[v->count++];memset(o,0,sizeof(*o));o->parent_index=SIZE_MAX;o->system_entry_offset=UINT64_MAX;o->entry_offset=UINT64_MAX;return o;}
static char *utf16_name(const uint8_t *data, size_t chars) {
    const size_t capacity = chars * 3U + 1U;
    char *out = ld_xmalloc(capacity);
    size_t position = 0U;

    for (size_t index = 0U; index < chars; ++index) {
        const uint16_t high = exfat_u16(data, index * 2U);
        uint32_t codepoint = high;
        if (high >= 0xd800U && high <= 0xdbffU) {
            if (index + 1U < chars) {
                const uint16_t low = exfat_u16(data, (index + 1U) * 2U);
                if (low >= 0xdc00U && low <= 0xdfffU) {
                    codepoint = UINT32_C(0x10000) +
                        (((uint32_t)high - UINT32_C(0xd800)) << 10U) +
                        ((uint32_t)low - UINT32_C(0xdc00));
                    ++index;
                } else {
                    codepoint = UINT32_C(0xfffd);
                }
            } else {
                codepoint = UINT32_C(0xfffd);
            }
        } else if (high >= 0xdc00U && high <= 0xdfffU) {
            codepoint = UINT32_C(0xfffd);
        }

        char encoded[4];
        size_t encoded_length = 0U;
        if (!infiltratr_utf8_encode_codepoint(
                codepoint, encoded, sizeof(encoded), &encoded_length)) {
            encoded_length = 0U;
            (void)infiltratr_utf8_encode_codepoint(
                UINT32_C(0xfffd), encoded, sizeof(encoded), &encoded_length);
        }
        if (encoded_length > capacity - position - 1U) break;
        memcpy(out + position, encoded, encoded_length);
        position += encoded_length;
    }
    out[position] = '\0';
    return out;
}
static char *join_path_alloc(const char *parent, const char *name) {
    const char *left = (parent == NULL || parent[0] == '\0' ||
                        strcmp(parent, "/") == 0) ? "/" : parent;
    size_t needed = 0U;
    if (!infiltratr_size_add_checked(strlen(left), strlen(name), &needed) ||
        !infiltratr_size_add_checked(needed, 2U, &needed))
        ld_die("exFAT path length overflow");
    char *out = ld_xmalloc(needed);
    if (!infiltratr_path_join(out, needed, left, name))
        ld_die("cannot join exFAT catalogue path");
    return out;
}
static bool allocated_bit(const ExfatCatalogue*c,uint32_t cluster){if(cluster<2U)return true;uint32_t bit=cluster-2U;if((uint64_t)bit>=c->bitmap_length*8ULL)return true;return (c->bitmap[bit>>3U]&(uint8_t)(1U<<(bit&7U)))!=0U;}
bool exfat_allocated(const ExfatCatalogue*c,uint32_t cluster){return allocated_bit(c,cluster);}

static int parse_directory(ExfatVolume*v,ExfatCatalogue*c,size_t parent_index,const char*display,const ExfatClusterVec*clusters,uint8_t*data,size_t data_len,uint8_t*visited,char**error){(void)clusters;size_t off=0;while(off+32U<=data_len){uint8_t type=data[off];if(type==0U)break;if(type!=0x85U){off+=32U;continue;}uint32_t entry_count=(uint32_t)data[off+1U]+1U;if(entry_count<2U||off+(size_t)entry_count*32U>data_len){exfat_set_error(error,"truncated exFAT file directory-entry set");return -1;}if(exfat_entry_checksum(data+off,(size_t)entry_count*32U)!=exfat_u16(data,off+2U)){exfat_set_error(error,"exFAT file entry-set checksum is invalid in %s",display);return -1;}size_t stream=SIZE_MAX;uint8_t name_len=0;uint8_t*namebuf=ld_xmalloc((size_t)entry_count*30U);size_t namebytes=0;for(uint32_t i=1;i<entry_count;++i){size_t so=off+(size_t)i*32U;if(data[so]==0xc0U){if(stream!=SIZE_MAX){free(namebuf);exfat_set_error(error,"duplicate exFAT stream extension");return -1;}stream=so;name_len=data[so+3U];}else if(data[so]==0xc1U){memcpy(namebuf+namebytes,data+so+2U,30U);namebytes+=30U;}}if(stream==SIZE_MAX){free(namebuf);exfat_set_error(error,"exFAT file entry set has no stream extension");return -1;}if((size_t)name_len*2U>namebytes){free(namebuf);exfat_set_error(error,"truncated exFAT file name");return -1;}char*name=utf16_name(namebuf,name_len);free(namebuf);uint32_t first=exfat_u32(data,stream+20U);uint64_t valid=exfat_u64(data,stream+8U),length=exfat_u64(data,stream+24U);bool nofat=(data[stream+1U]&0x02U)!=0U;bool dir=(exfat_u16(data,off+4U)&0x10U)!=0U;uint64_t count=length==0?0:(length+v->cluster_size-1U)/v->cluster_size;ExfatClusterVec object_clusters={0};if(count!=0&&exfat_chain(v,first,count,true,nofat,&object_clusters,error)!=0){free(name);return -1;}if(count==0&&(first!=0U||length!=0U||nofat)){free(name);exfat_set_error(error,"invalid zero-length exFAT stream fields");return -1;}if(count!=0){ExfatObject*o=object_push(&c->objects);o->kind=dir?EXFAT_OBJ_DIRECTORY:EXFAT_OBJ_FILE;o->path=join_path_alloc(display,name);o->data_length=length;o->valid_length=valid;o->regular_file=!dir;o->directory=dir;o->original_no_fat_chain=nofat;o->parent_index=parent_index;o->entry_offset=off;o->entry_count=entry_count;cluster_copy(&o->clusters,&object_clusters);size_t object_index=c->objects.count-1U;if(dir){uint32_t visit=first-2U;if((visited[visit>>3U]&(uint8_t)(1U<<(visit&7U)))!=0U){free(name);exfat_clusters_free(&object_clusters);exfat_set_error(error,"exFAT directory cycle or duplicate first cluster at %s",o->path);return -1;}visited[visit>>3U]|=(uint8_t)(1U<<(visit&7U));uint64_t alloc=(uint64_t)object_clusters.count*v->cluster_size;if(alloc>SIZE_MAX){free(name);exfat_clusters_free(&object_clusters);exfat_set_error(error,"exFAT directory is too large");return -1;}uint8_t*child=NULL;if(exfat_read_stream(v,&object_clusters,alloc,&child,error)!=0){free(name);exfat_clusters_free(&object_clusters);return -1;}if(parse_directory(v,c,object_index,o->path,&object_clusters,child,(size_t)alloc,visited,error)!=0){free(child);free(name);exfat_clusters_free(&object_clusters);return -1;}free(child);}}exfat_clusters_free(&object_clusters);free(name);off+=(size_t)entry_count*32U;}return 0;}

static int find_system_entries(ExfatVolume*v,ExfatCatalogue*c,const uint8_t*root,size_t root_len,char**error){for(size_t off=0;off+32U<=root_len;off+=32U){uint8_t type=root[off];if(type==0U)break;if(type==0x81U&&(root[off+1U]&1U)==0U){if(c->bitmap_entry_offset!=UINT64_MAX){exfat_set_error(error,"duplicate active exFAT allocation bitmap entry");return -1;}c->bitmap_entry_offset=off;c->bitmap_length=(size_t)exfat_u64(root,off+24U);uint32_t first=exfat_u32(root,off+20U);uint64_t count=(c->bitmap_length+v->cluster_size-1U)/v->cluster_size;if(exfat_chain(v,first,count,true,false,&c->bitmap_clusters,error)!=0)return -1;}else if(type==0x82U){if(c->upcase_entry_offset!=UINT64_MAX){exfat_set_error(error,"duplicate exFAT up-case table entry");return -1;}c->upcase_entry_offset=off;c->upcase_length=exfat_u64(root,off+24U);uint32_t first=exfat_u32(root,off+20U);uint64_t count=(c->upcase_length+v->cluster_size-1U)/v->cluster_size;if(exfat_chain(v,first,count,true,false,&c->upcase_clusters,error)!=0)return -1;}}if(c->bitmap_entry_offset==UINT64_MAX||c->bitmap_clusters.count==0){exfat_set_error(error,"active exFAT allocation bitmap entry was not found");return -1;}if(c->upcase_entry_offset==UINT64_MAX||c->upcase_clusters.count==0){exfat_set_error(error,"exFAT up-case table entry was not found");return -1;}return 0;}

int exfat_scan(const char *path, bool allow_dirty, ExfatVolume *v,
               ExfatCatalogue *c, char **error) {
    uint8_t *root = NULL;
    uint8_t *upcase = NULL;
    uint8_t *visited = NULL;
    uint8_t *owners = NULL;
    uint64_t root_len;

    memset(c, 0, sizeof(*c));
    c->bitmap_entry_offset = UINT64_MAX;
    c->upcase_entry_offset = UINT64_MAX;

    if (exfat_open_volume(path, false, allow_dirty, v, error) != 0) return -1;
    if (exfat_chain(v, v->root_cluster, 0, false, false, &c->root_clusters, error) != 0) goto fail;

    root_len = (uint64_t)c->root_clusters.count * v->cluster_size;
    if (root_len == 0 || root_len > SIZE_MAX) {
        exfat_set_error(error, "invalid exFAT root directory allocation");
        goto fail;
    }
    if (exfat_read_stream(v, &c->root_clusters, root_len, &root, error) != 0) goto fail;
    if (find_system_entries(v, c, root, (size_t)root_len, error) != 0) goto fail;
    if (c->bitmap_length == 0 || c->bitmap_length * 8ULL < v->cluster_count) {
        exfat_set_error(error, "exFAT allocation bitmap is shorter than the heap");
        goto fail;
    }
    if (exfat_read_stream(v, &c->bitmap_clusters, c->bitmap_length, &c->bitmap, error) != 0) goto fail;
    if (exfat_read_stream(v, &c->upcase_clusters, c->upcase_length, &upcase, error) != 0) goto fail;
    if (exfat_table_checksum(upcase, (size_t)c->upcase_length) !=
        exfat_u32(root, (size_t)c->upcase_entry_offset + 4U)) {
        exfat_set_error(error, "exFAT up-case table checksum is invalid");
        goto fail;
    }
    free(upcase);
    upcase = NULL;

    ExfatObject *bitmap = object_push(&c->objects);
    bitmap->kind = EXFAT_OBJ_BITMAP;
    bitmap->path = ld_xstrdup("<allocation bitmap>");
    bitmap->data_length = c->bitmap_length;
    bitmap->valid_length = c->bitmap_length;
    bitmap->system_entry_offset = c->bitmap_entry_offset;
    cluster_copy(&bitmap->clusters, &c->bitmap_clusters);

    ExfatObject *up = object_push(&c->objects);
    up->kind = EXFAT_OBJ_UPCASE;
    up->path = ld_xstrdup("<up-case table>");
    up->data_length = c->upcase_length;
    up->valid_length = c->upcase_length;
    up->system_entry_offset = c->upcase_entry_offset;
    cluster_copy(&up->clusters, &c->upcase_clusters);

    ExfatObject *root_obj = object_push(&c->objects);
    root_obj->kind = EXFAT_OBJ_ROOT;
    root_obj->path = ld_xstrdup("/");
    root_obj->directory = true;
    root_obj->data_length = root_len;
    root_obj->valid_length = root_len;
    cluster_copy(&root_obj->clusters, &c->root_clusters);

    visited = calloc((v->cluster_count + 7U) / 8U, 1U);
    if (visited == NULL) {
        exfat_set_error(error, "allocating exFAT directory-cycle guard failed");
        goto fail;
    }
    uint32_t rootbit = v->root_cluster - 2U;
    visited[rootbit >> 3U] |= (uint8_t)(1U << (rootbit & 7U));
    if (parse_directory(v, c, 2U, "/", &c->root_clusters, root,
                        (size_t)root_len, visited, error) != 0) goto fail;
    free(visited);
    visited = NULL;
    free(root);
    root = NULL;

    owners = calloc((v->cluster_count + 7U) / 8U, 1U);
    if (owners == NULL) {
        exfat_set_error(error, "allocating exFAT ownership map failed");
        goto fail;
    }
    c->growth_10_satisfied = true;
    for (size_t i = 0; i < c->objects.count; ++i) {
        ExfatObject *o = &c->objects.items[i];
        size_t fragments = exfat_fragments(&o->clusters);
        if (o->kind == EXFAT_OBJ_FILE) {
            c->regular_files++;
            if (fragments > 1U) c->fragmented_files++;
        }
        if (o->directory) {
            c->directories++;
            if (fragments > 1U) c->fragmented_directories++;
        }
        for (size_t j = 0; j < o->clusters.count; ++j) {
            uint32_t cluster = o->clusters.items[j];
            uint32_t bit = cluster - 2U;
            if ((owners[bit >> 3U] & (uint8_t)(1U << (bit & 7U))) != 0U) {
                exfat_set_error(error, "exFAT cluster %u has multiple owners", cluster);
                goto fail;
            }
            owners[bit >> 3U] |= (uint8_t)(1U << (bit & 7U));
            if (!allocated_bit(c, cluster)) {
                exfat_set_error(error, "%s references exFAT cluster %u marked free", o->path, cluster);
                goto fail;
            }
        }
        if (o->regular_file) {
            if (fragments != 1U) {
                c->growth_10_satisfied = false;
            } else {
                uint32_t reserve = (uint32_t)(((uint64_t)o->clusters.count * 10U + 99U) / 100U);
                uint64_t cursor = (uint64_t)o->clusters.items[o->clusters.count - 1U] + 1U;
                for (uint32_t r = 0; r < reserve; ++r) {
                    if (cursor + r >= (uint64_t)v->cluster_count + 2ULL ||
                        allocated_bit(c, (uint32_t)(cursor + r))) {
                        c->growth_10_satisfied = false;
                    }
                }
            }
        }
    }
    for (uint32_t cluster = 2U; cluster < v->cluster_count + 2U; ++cluster) {
        uint32_t bit = cluster - 2U;
        bool owned = (owners[bit >> 3U] & (uint8_t)(1U << (bit & 7U))) != 0U;
        if (allocated_bit(c, cluster) && !owned) {
            exfat_set_error(error, "exFAT allocation bitmap contains unowned cluster %u", cluster);
            goto fail;
        }
    }
    free(owners);
    owners = NULL;
    if (c->regular_files == 0 || c->fragmented_directories != 0) c->growth_10_satisfied = false;
    return 0;

fail:
    free(owners);
    free(visited);
    free(upcase);
    free(root);
    exfat_catalogue_free(c);
    exfat_close_volume(v);
    return -1;
}

void exfat_catalogue_free(ExfatCatalogue*c){if(c==NULL)return;for(size_t i=0;i<c->objects.count;++i){free(c->objects.items[i].path);exfat_clusters_free(&c->objects.items[i].clusters);}free(c->objects.items);free(c->bitmap);exfat_clusters_free(&c->root_clusters);exfat_clusters_free(&c->bitmap_clusters);exfat_clusters_free(&c->upcase_clusters);memset(c,0,sizeof(*c));}
