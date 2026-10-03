// SPDX-License-Identifier: GPL-3.0-or-later
#include "ntfs_native.h"

#include "infiltratr/core.h"
#include "infiltratr/arithmetic.h"
#include "infiltratr/endian.h"
#include "ld_device.h"
#include "ld_io.h"
#include "ld_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

void ntfs_set_error(char **error, const char *format, ...) {
    if (error == NULL || *error != NULL) return;
    va_list ap, copy;
    va_start(ap, format);
    va_copy(copy, ap);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0) {
        *error = ld_xstrdup("NTFS engine error");
        va_end(ap);
        return;
    }
    *error = ld_xmalloc((size_t)needed + 1U);
    (void)vsnprintf(*error, (size_t)needed + 1U, format, ap);
    va_end(ap);
}

uint16_t ntfs_u16(const void *data, size_t offset) {
    const uint8_t *p = data;
    return infiltratr_load_le16(p + offset);
}
uint32_t ntfs_u32(const void *data, size_t offset) {
    const uint8_t *p = data;
    return infiltratr_load_le32(p + offset);
}
uint64_t ntfs_u64(const void *data, size_t offset) {
    const uint8_t *p = data;
    return infiltratr_load_le64(p + offset);
}
void ntfs_put_u16(void *data, size_t offset, uint16_t value) {
    uint8_t *p = data;
    infiltratr_store_le16(p + offset, value);
}
void ntfs_put_u64(void *data, size_t offset, uint64_t value) {
    uint8_t *p = data;
    infiltratr_store_le64(p + offset, value);
}

void ntfs_runs_free(NtfsRunVec *runs) {
    if (runs == NULL) return;
    free(runs->items); memset(runs, 0, sizeof(*runs));
}
int ntfs_runs_push(NtfsRunVec *runs, uint64_t lcn, uint64_t length, bool sparse) {
    if (length == 0) return 0;
    if (runs->count != 0) {
        NtfsRun *last = &runs->items[runs->count - 1U];
        uint64_t last_end = 0U;
        const bool contiguous = sparse ||
            (infiltratr_u64_add_checked(last->lcn, last->length, &last_end) &&
             last_end == lcn);
        if (last->sparse == sparse && contiguous) {
            uint64_t combined_length = 0U;
            if (!infiltratr_u64_add_checked(last->length, length,
                                            &combined_length))
                return -1;
            last->length = combined_length;
            return 0;
        }
    }
    if (!infiltratr_array_reserve((void **)&runs->items, &runs->capacity,
                                  sizeof(*runs->items), runs->count + 1U, 8U))
        ld_die("cannot grow NTFS run list");
    runs->items[runs->count++] = (NtfsRun){.lcn=lcn,.length=length,.sparse=sparse};
    return 0;
}
uint64_t ntfs_run_clusters(const NtfsRunVec *runs) {
    uint64_t result = 0;
    for (size_t index = 0; index < runs->count; ++index)
        if (!runs->items[index].sparse) result += runs->items[index].length;
    return result;
}
size_t ntfs_fragment_count(const NtfsRunVec *runs) {
    size_t count = 0;
    for (size_t index = 0; index < runs->count; ++index) if (!runs->items[index].sparse) count++;
    return count;
}

/*
 * NTFS mapping pairs encode run length plus a signed little-endian LCN delta
 * from the previous non-sparse run. Decoding to a full-width signed value first
 * makes sign extension and negative-delta underflow checks explicit.
 */
static int64_t decode_signed(const uint8_t *bytes, size_t length) {
    uint64_t value = 0;
    for (size_t index = 0; index < length; ++index) value |= (uint64_t)bytes[index] << (index * 8U);
    if (length < 8U && (bytes[length - 1U] & 0x80U) != 0U)
        value |= UINT64_MAX << (length * 8U);
    return (int64_t)value;
}

int ntfs_decode_runlist(const uint8_t *data, size_t length, NtfsRunVec *runs, char **error) {
    uint64_t previous_lcn = 0;
    size_t position = 0;
    while (position < length) {
        uint8_t header = data[position++];
        if (header == 0) return 0;
        unsigned length_size = header & 0x0fU;
        unsigned offset_size = header >> 4;
        if (length_size == 0 || length_size > 8U || offset_size > 8U ||
            position + length_size + offset_size > length) {
            ntfs_set_error(error, "invalid NTFS mapping-pairs array");
            return -1;
        }
        int64_t signed_length = decode_signed(data + position, length_size);
        position += length_size;
        if (signed_length <= 0) {
            ntfs_set_error(error, "invalid non-positive NTFS run length");
            return -1;
        }
        uint64_t run_length = (uint64_t)signed_length;
        if (offset_size == 0U) {
            if (ntfs_runs_push(runs, 0, run_length, true) != 0) goto overflow;
            continue;
        }
        int64_t delta = decode_signed(data + position, offset_size);
        position += offset_size;
        if (delta < 0 && (uint64_t)(-delta) > previous_lcn) {
            ntfs_set_error(error, "invalid negative NTFS logical cluster number");
            return -1;
        }
        uint64_t lcn = delta < 0 ? previous_lcn - (uint64_t)(-delta) : previous_lcn + (uint64_t)delta;
        previous_lcn = lcn;
        if (ntfs_runs_push(runs, lcn, run_length, false) != 0) goto overflow;
    }
    ntfs_set_error(error, "unterminated NTFS mapping-pairs array");
    return -1;
overflow:
    ntfs_set_error(error, "NTFS runlist length overflow");
    return -1;
}

static size_t unsigned_min_bytes(uint64_t value, uint8_t output[9]) {
    if (value == 0) return 0;
    size_t count = 0; uint64_t copy = value;
    while (copy != 0) { output[count++] = (uint8_t)copy; copy >>= 8; }
    if ((output[count - 1U] & 0x80U) != 0U) output[count++] = 0;
    return count;
}
static size_t signed_min_bytes(int64_t value, uint8_t output[8]) {
    for (size_t count = 1; count <= 8U; ++count) {
        uint64_t raw = (uint64_t)value;
        for (size_t index = 0; index < count; ++index) output[index] = (uint8_t)(raw >> (index * 8U));
        int64_t decoded = decode_signed(output, count);
        if (decoded == value) return count;
    }
    return 0;
}
int ntfs_encode_single_run(uint64_t lcn, uint64_t length, uint8_t *output, size_t capacity,
                           size_t *used, char **error) {
    uint8_t length_bytes[9], offset_bytes[8];
    size_t ls = unsigned_min_bytes(length, length_bytes);
    if (ls == 0 || ls > 8U || lcn > INT64_MAX) {
        ntfs_set_error(error, "NTFS contiguous run cannot be encoded"); return -1;
    }
    size_t os = signed_min_bytes((int64_t)lcn, offset_bytes);
    if (os == 0 || 2U + ls + os > capacity) {
        ntfs_set_error(error, "NTFS MFT record lacks mapping-pair capacity for the canonical run"); return -1;
    }
    output[0] = (uint8_t)((os << 4U) | ls);
    memcpy(output + 1U, length_bytes, ls);
    memcpy(output + 1U + ls, offset_bytes, os);
    output[1U + ls + os] = 0;
    *used = 2U + ls + os;
    return 0;
}

/*
 * The Update Sequence Array is a torn-sector detector. Validate the on-disk
 * sequence value at every sector tail before restoring the original tail words
 * into the parsed copy; a mismatch prevents the record being trusted.
 */
int ntfs_apply_fixups(const uint8_t *raw, size_t length, uint32_t sector_size,
                      uint8_t *fixed, char **error) {
    if (sector_size == 0 || length % sector_size != 0) { ntfs_set_error(error, "invalid NTFS update-sequence geometry"); return -1; }
    memcpy(fixed, raw, length);
    uint16_t usa_off = ntfs_u16(fixed, 4), usa_count = ntfs_u16(fixed, 6);
    size_t expected = length / sector_size + 1U;
    if (usa_count != expected || (size_t)usa_off + (size_t)usa_count * 2U > length) {
        ntfs_set_error(error, "invalid NTFS update-sequence array"); return -1;
    }
    uint16_t usn = ntfs_u16(fixed, usa_off);
    for (size_t index = 1; index < usa_count; ++index) {
        size_t end = index * sector_size;
        if (ntfs_u16(fixed, end - 2U) != usn) { ntfs_set_error(error, "NTFS MFT update-sequence mismatch"); return -1; }
        ntfs_put_u16(fixed, end - 2U, ntfs_u16(fixed, usa_off + index * 2U));
    }
    return 0;
}
/*
 * Inverse USA transform for a record about to be written: preserve each sector
 * tail in the USA, advance the sequence number (skipping zero), then stamp the
 * new sequence value into every protected tail.
 */
int ntfs_prepare_fixups(const uint8_t *fixed, size_t length, uint32_t sector_size,
                        uint8_t *raw, char **error) {
    if (sector_size == 0 || length % sector_size != 0) { ntfs_set_error(error, "invalid NTFS update-sequence geometry"); return -1; }
    memcpy(raw, fixed, length);
    uint16_t usa_off = ntfs_u16(raw, 4), usa_count = ntfs_u16(raw, 6);
    size_t expected = length / sector_size + 1U;
    if (usa_count != expected || (size_t)usa_off + (size_t)usa_count * 2U > length) {
        ntfs_set_error(error, "invalid NTFS update-sequence array"); return -1;
    }
    uint16_t usn = (uint16_t)(ntfs_u16(raw, usa_off) + 1U); if (usn == 0) usn = 1;
    ntfs_put_u16(raw, usa_off, usn);
    for (size_t index = 1; index < usa_count; ++index) {
        size_t end = index * sector_size;
        ntfs_put_u16(raw, usa_off + index * 2U, ntfs_u16(raw, end - 2U));
        ntfs_put_u16(raw, end - 2U, usn);
    }
    return 0;
}

int ntfs_open_volume(const char *path, bool write, NtfsVolume *volume, char **error) {
    memset(volume, 0, sizeof(*volume)); volume->fd = -1;
    char *real = realpath(path, NULL);
    if (real == NULL) { ntfs_set_error(error, "cannot resolve NTFS target %s: %s", path, strerror(errno)); return -1; }
    struct stat status;
    if (stat(real, &status) != 0 || (!S_ISREG(status.st_mode) && !S_ISBLK(status.st_mode))) {
        ntfs_set_error(error, "NTFS target must be a block device or regular image"); free(real); return -1;
    }
    if (write && S_ISBLK(status.st_mode)) {
        if (ld_path_is_mounted(real)) {
            ntfs_set_error(error, "NTFS target is mounted; raw mutation requires an unmounted filesystem");
            free(real);
            return -1;
        }
    }
    int flags = (write ? O_RDWR : O_RDONLY) | O_CLOEXEC;
    if (write && S_ISBLK(status.st_mode)) flags |= O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(real, flags);
    if (fd < 0) { ntfs_set_error(error, "cannot open NTFS target: %s", strerror(errno)); free(real); return -1; }
    struct stat opened;
    if (fstat(fd, &opened) != 0 ||
        (status.st_mode & S_IFMT) != (opened.st_mode & S_IFMT) ||
        status.st_dev != opened.st_dev || status.st_ino != opened.st_ino ||
        (S_ISBLK(status.st_mode) && status.st_rdev != opened.st_rdev)) {
        ntfs_set_error(error, "NTFS target identity changed between validation and open");
        close(fd); free(real); return -1;
    }
    if (write && S_ISBLK(opened.st_mode) &&
        ld_device_number_is_mounted(opened.st_rdev)) {
        ntfs_set_error(error, "NTFS target became mounted while opening for raw mutation");
        close(fd); free(real); return -1;
    }
    status = opened;
    if (write && flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ntfs_set_error(error, "cannot lock NTFS target exclusively: %s", strerror(errno)); close(fd); free(real); return -1;
    }
    uint8_t boot[512];
    ssize_t got = ld_pread_full(fd, boot, sizeof(boot), 0);
    if (got != (ssize_t)sizeof(boot) || memcmp(boot + 3, "NTFS    ", 8) != 0 || boot[510] != 0x55 || boot[511] != 0xaa) {
        ntfs_set_error(error, "target does not contain a valid NTFS boot sector"); close(fd); free(real); return -1;
    }
    uint32_t bps = ntfs_u16(boot, 11), spc = boot[13];
    if (bps < 256U || bps > 4096U || (bps & (bps - 1U)) != 0U || spc == 0U || (spc & (spc - 1U)) != 0U) {
        ntfs_set_error(error, "invalid NTFS sector or cluster geometry"); close(fd); free(real); return -1;
    }
    uint32_t cluster = bps * spc;
    if (cluster > 65536U) { ntfs_set_error(error, "unsupported NTFS cluster size"); close(fd); free(real); return -1; }
    uint64_t sectors = ntfs_u64(boot, 40), clusters = sectors / spc;
    uint64_t device_size;
    if (S_ISREG(status.st_mode)) device_size = (uint64_t)status.st_size;
    else {
        LdDevice d = ld_device_open(real, false); device_size = d.size_bytes; ld_device_close(&d);
    }
    uint64_t volume_bytes = 0U;
    if (sectors == 0 || clusters == 0 ||
        !infiltratr_u64_multiply_checked(sectors, bps, &volume_bytes) ||
        volume_bytes > device_size) {
        ntfs_set_error(error, "NTFS volume boundary exceeds the target device"); close(fd); free(real); return -1;
    }
    int8_t encoded_record = (int8_t)boot[64];
    uint64_t record_size = encoded_record < 0 ? (UINT64_C(1) << (unsigned)(-encoded_record)) : (uint64_t)(uint8_t)encoded_record * cluster;
    if (record_size < 512U || record_size > 1024U * 1024U || record_size % bps != 0U) {
        ntfs_set_error(error, "invalid NTFS MFT record size"); close(fd); free(real); return -1;
    }
    volume->path=real; volume->fd=fd; volume->device_size=device_size; volume->volume_bytes=volume_bytes;
    volume->bytes_per_sector=bps; volume->sectors_per_cluster=spc; volume->cluster_size=cluster;
    volume->total_clusters=clusters; volume->mft_lcn=ntfs_u64(boot,48); volume->mftmirr_lcn=ntfs_u64(boot,56);
    volume->record_size=(uint32_t)record_size; memcpy(volume->serial,boot+72,8);
    return 0;
}
void ntfs_close_volume(NtfsVolume *volume) {
    if (volume == NULL) return;
    if (volume->fd >= 0) close(volume->fd);
    free(volume->path); memset(volume,0,sizeof(*volume)); volume->fd=-1;
}

static int stream_segments_rw(const NtfsVolume *volume, const NtfsRunVec *runs,
                              uint64_t logical_offset, void *buffer, size_t length,
                              bool write, char **error) {
    uint64_t cursor=0, wanted=logical_offset; size_t remaining=length, consumed=0;
    for (size_t index=0; index<runs->count && remaining>0; ++index) {
        const NtfsRun *run=&runs->items[index];
        uint64_t run_bytes=run->length * volume->cluster_size;
        if (wanted >= cursor + run_bytes) { cursor += run_bytes; continue; }
        uint64_t within = wanted > cursor ? wanted-cursor : 0;
        uint64_t available = run_bytes-within;
        size_t take = available < remaining ? (size_t)available : remaining;
        if (run->sparse) { ntfs_set_error(error,"sparse run encountered in an NTFS metadata stream"); return -1; }
        uint64_t physical = run->lcn * volume->cluster_size + within;
        ssize_t count = write ? ld_pwrite_full(volume->fd,(uint8_t*)buffer+consumed,take,physical)
                              : ld_pread_full(volume->fd,(uint8_t*)buffer+consumed,take,physical);
        if (count < 0 || (size_t)count != take) { ntfs_set_error(error,"short NTFS metadata-stream %s",write?"write":"read"); return -1; }
        consumed += take; remaining -= take; wanted += take; cursor += run_bytes;
    }
    if (remaining != 0) { ntfs_set_error(error,"NTFS stream runlist is shorter than requested range"); return -1; }
    return 0;
}
int ntfs_read_stream(const NtfsVolume *volume, const NtfsRunVec *runs,
                     uint64_t logical_offset, void *buffer, size_t length, char **error) {
    return stream_segments_rw(volume,runs,logical_offset,buffer,length,false,error);
}
int ntfs_write_stream(const NtfsVolume *volume, const NtfsRunVec *runs,
                      uint64_t logical_offset, const void *buffer, size_t length, char **error) {
    return stream_segments_rw(volume,runs,logical_offset,(void*)buffer,length,true,error);
}

int ntfs_read_record(const NtfsVolume *volume, const NtfsRunVec *mft_runs,
                     uint64_t record_number, uint8_t **raw, uint8_t **fixed, char **error) {
    *raw=ld_xmalloc(volume->record_size); *fixed=ld_xmalloc(volume->record_size);
    uint64_t record_offset = 0U;
    if (!infiltratr_u64_multiply_checked(record_number, volume->record_size,
                                         &record_offset) ||
        ntfs_read_stream(volume,mft_runs,record_offset,*raw,volume->record_size,error)!=0 ||
        ntfs_apply_fixups(*raw,volume->record_size,volume->bytes_per_sector,*fixed,error)!=0) {
        free(*raw); free(*fixed); *raw=NULL; *fixed=NULL; return -1;
    }
    return 0;
}
int ntfs_write_record(const NtfsVolume *volume, const NtfsRunVec *mft_runs,
                      uint64_t record_number, const uint8_t *raw, char **error) {
    uint64_t record_offset = 0U;
    if (!infiltratr_u64_multiply_checked(record_number, volume->record_size,
                                         &record_offset)) {
        ntfs_set_error(error,"NTFS MFT record offset overflow");
        return -1;
    }
    return ntfs_write_stream(volume,mft_runs,record_offset,raw,volume->record_size,error);
}

static void attr_free(NtfsAttribute *attr) { ntfs_runs_free(&attr->runs); }
void ntfs_attributes_free(NtfsAttributeVec *attributes) {
    if (attributes == NULL) return;
    for(size_t i=0;i<attributes->count;++i) attr_free(&attributes->items[i]);
    free(attributes->items); memset(attributes,0,sizeof(*attributes));
}
static NtfsAttribute *attr_push(NtfsAttributeVec *vec) {
    if (!infiltratr_array_reserve((void **)&vec->items, &vec->capacity,
                                  sizeof(*vec->items), vec->count + 1U, 8U))
        ld_die("cannot grow NTFS attribute list");
    NtfsAttribute *a=&vec->items[vec->count++]; memset(a,0,sizeof(*a)); return a;
}
static void decode_utf16_ascii(const uint8_t *data, size_t chars, char *out, size_t cap) {
    if (cap == 0) return;
    size_t w = 0;
    for(size_t i=0;i<chars && w+1U<cap;++i){uint16_t ch=ntfs_u16(data,i*2U);out[w++]=(ch>=32U&&ch<127U)?(char)ch:'?';}
    out[w]='\0';
}
int ntfs_parse_attributes(const uint8_t *fixed, size_t record_size,
                          NtfsAttributeVec *attributes, char **error) {
    uint32_t position=ntfs_u16(fixed,20), in_use=ntfs_u32(fixed,24);
    if(in_use>record_size)in_use=(uint32_t)record_size;
    while((uint64_t)position+16U<=in_use){
        uint32_t type=ntfs_u32(fixed,position); if(type==UINT32_C(0xffffffff))return 0;
        uint32_t length=ntfs_u32(fixed,position+4U);
        if(length<24U || (uint64_t)position+length>in_use){ntfs_set_error(error,"invalid NTFS attribute record");goto fail;}
        NtfsAttribute *a=attr_push(attributes); a->offset=position;a->length=length;a->type=type;a->nonresident=fixed[position+8U]!=0;a->flags=ntfs_u16(fixed,position+12U);
        unsigned name_chars=fixed[position+9U]; uint16_t name_off=ntfs_u16(fixed,position+10U);
        if(name_chars!=0){if(name_off<16U || (uint32_t)name_off+(uint32_t)name_chars*2U>length){ntfs_set_error(error,"invalid NTFS attribute name");goto fail;}decode_utf16_ascii(fixed+position+name_off,name_chars,a->name,sizeof(a->name));}
        if(a->nonresident){
            if(length<64U){ntfs_set_error(error,"truncated NTFS non-resident attribute");goto fail;}
            a->lowest_vcn=ntfs_u64(fixed,position+16U);a->highest_vcn=ntfs_u64(fixed,position+24U);a->run_offset=ntfs_u16(fixed,position+32U);
            a->allocated_size=ntfs_u64(fixed,position+40U);a->data_size=ntfs_u64(fixed,position+48U);a->initialized_size=ntfs_u64(fixed,position+56U);
            if(a->run_offset<64U || a->run_offset>=length){ntfs_set_error(error,"invalid NTFS mapping-pairs offset");goto fail;}
            if(ntfs_decode_runlist(fixed+position+a->run_offset,length-a->run_offset,&a->runs,error)!=0)goto fail;
            uint64_t logical=0;for(size_t i=0;i<a->runs.count;++i)logical+=a->runs.items[i].length;
            if(a->highest_vcn<a->lowest_vcn || logical!=a->highest_vcn-a->lowest_vcn+1U){ntfs_set_error(error,"NTFS runlist length does not match its VCN range");goto fail;}
        }
        position+=length;
    }
    return 0;
fail: ntfs_attributes_free(attributes); return -1;
}

static NtfsAttribute *find_unnamed_nonresident(NtfsAttributeVec *attrs,uint32_t type){
    NtfsAttribute *found=NULL;for(size_t i=0;i<attrs->count;++i){NtfsAttribute *a=&attrs->items[i];if(a->type==type&&a->nonresident&&a->name[0]=='\0'){if(found!=NULL)return NULL;found=a;}}return found;
}
static int read_record0(NtfsVolume *volume,NtfsRunVec *mft_runs,uint64_t *data_size,char **error){
    uint8_t *raw=ld_xmalloc(volume->record_size),*fixed=ld_xmalloc(volume->record_size);
    ssize_t got=ld_pread_full(volume->fd,raw,volume->record_size,volume->mft_lcn*volume->cluster_size);
    if(got<0||(size_t)got!=volume->record_size||memcmp(raw,"FILE",4)!=0||ntfs_apply_fixups(raw,volume->record_size,volume->bytes_per_sector,fixed,error)!=0){if(error&&*error==NULL)ntfs_set_error(error,"NTFS $MFT record zero was not found");free(raw);free(fixed);return -1;}
    NtfsAttributeVec attrs={0};if(ntfs_parse_attributes(fixed,volume->record_size,&attrs,error)!=0){free(raw);free(fixed);return -1;}
    NtfsAttribute *a=find_unnamed_nonresident(&attrs,NTFS_ATTR_DATA);
    if(a==NULL||a->lowest_vcn!=0){ntfs_set_error(error,"unsupported split $MFT data attribute");ntfs_attributes_free(&attrs);free(raw);free(fixed);return -1;}
    for(size_t i=0;i<a->runs.count;++i)if(ntfs_runs_push(mft_runs,a->runs.items[i].lcn,a->runs.items[i].length,a->runs.items[i].sparse)!=0){ntfs_set_error(error,"MFT runlist overflow");ntfs_attributes_free(&attrs);free(raw);free(fixed);return -1;}
    *data_size=a->data_size;ntfs_attributes_free(&attrs);free(raw);free(fixed);return 0;
}
static int resident_volume_flags(const uint8_t *fixed,size_t record_size,uint16_t *flags,char **error){
    NtfsAttributeVec attrs={0};if(ntfs_parse_attributes(fixed,record_size,&attrs,error)!=0)return -1;int result=-1;
    for(size_t i=0;i<attrs.count;++i){NtfsAttribute *a=&attrs.items[i];if(a->type!=NTFS_ATTR_VOLUME_INFORMATION||a->nonresident)continue;uint32_t len=ntfs_u32(fixed,a->offset+16U);uint16_t off=ntfs_u16(fixed,a->offset+20U);if(len<12U||(uint32_t)off+len>a->length){ntfs_set_error(error,"invalid NTFS volume-information attribute");break;}*flags=ntfs_u16(fixed,a->offset+off+10U);result=0;break;}
    if (result != 0 && error != NULL && *error == NULL)
        ntfs_set_error(error, "NTFS volume-information attribute was not found");
    ntfs_attributes_free(&attrs);
    return result;
}
static int validate_flags(uint16_t flags,bool allow_dirty,char **error){uint16_t unknown=(uint16_t)(flags&~NTFS_VOLUME_ACCEPTED_MASK);if(unknown){ntfs_set_error(error,"unsupported NTFS volume flags are set (0x%04x)",flags);return -1;}uint16_t unsafe=(uint16_t)(flags&NTFS_VOLUME_UNSAFE_MASK);if(allow_dirty)unsafe=(uint16_t)(unsafe&~NTFS_VOLUME_DIRTY);if(unsafe){ntfs_set_error(error,"NTFS volume has an active unsafe maintenance state (0x%04x)",flags);return -1;}if((flags&NTFS_VOLUME_DIRTY)&&!allow_dirty){ntfs_set_error(error,"NTFS dirty flag is set; complete Windows filesystem checking first");return -1;}return 0;}

int ntfs_read_layout(NtfsVolume *volume, bool allow_dirty, NtfsLayout *layout, char **error) {
    memset(layout,0,sizeof(*layout));
    if(read_record0(volume,&layout->mft_runs,&layout->mft_data_size,error)!=0)return -1;
    uint8_t *raw=NULL,*fixed=NULL;
    if(ntfs_read_record(volume,&layout->mft_runs,3,&raw,&fixed,error)!=0)goto fail;
    uint16_t flags=0;if(resident_volume_flags(fixed,volume->record_size,&flags,error)!=0||validate_flags(flags,allow_dirty,error)!=0){free(raw);free(fixed);goto fail;}free(raw);free(fixed);
    layout->volume_dirty = (flags & NTFS_VOLUME_DIRTY) != 0U;
    raw=fixed=NULL;
    if(ntfs_read_record(volume,&layout->mft_runs,6,&raw,&fixed,error)!=0)goto fail;
    NtfsAttributeVec attrs={0};if(ntfs_parse_attributes(fixed,volume->record_size,&attrs,error)!=0){free(raw);free(fixed);goto fail;}
    NtfsAttribute *bitmap=find_unnamed_nonresident(&attrs,NTFS_ATTR_DATA);
    if(bitmap==NULL||bitmap->lowest_vcn!=0){ntfs_set_error(error,"unsupported split or resident NTFS $Bitmap stream");ntfs_attributes_free(&attrs);free(raw);free(fixed);goto fail;}
    for(size_t i=0;i<bitmap->runs.count;++i)if(ntfs_runs_push(&layout->bitmap_runs,bitmap->runs.items[i].lcn,bitmap->runs.items[i].length,bitmap->runs.items[i].sparse)!=0){ntfs_set_error(error,"bitmap runlist overflow");ntfs_attributes_free(&attrs);free(raw);free(fixed);goto fail;}
    layout->bitmap_data_size=bitmap->data_size;layout->bitmap_bytes=(size_t)bitmap->data_size;layout->bitmap=ld_xmalloc(layout->bitmap_bytes);
    ntfs_attributes_free(&attrs);free(raw);free(fixed);
    if(ntfs_read_stream(volume,&layout->bitmap_runs,0,layout->bitmap,layout->bitmap_bytes,error)!=0)goto fail;
    if(layout->bitmap_bytes*8U<volume->total_clusters){ntfs_set_error(error,"NTFS $Bitmap is shorter than the volume");goto fail;}
    return 0;
fail:ntfs_layout_free(layout);return -1;
}
void ntfs_layout_free(NtfsLayout *layout){if(layout==NULL)return;ntfs_runs_free(&layout->mft_runs);ntfs_runs_free(&layout->bitmap_runs);free(layout->bitmap);memset(layout,0,sizeof(*layout));}
int ntfs_bitmap_bit(const NtfsLayout *layout,uint64_t cluster){if(cluster>=layout->bitmap_bytes*8U)return 1;return (layout->bitmap[cluster>>3U]&(uint8_t)(1U<<(cluster&7U)))!=0;}
void ntfs_bitmap_set(NtfsLayout *layout,uint64_t cluster,bool used){if(cluster>=layout->bitmap_bytes*8U)return;uint8_t mask=(uint8_t)(1U<<(cluster&7U));if(used)layout->bitmap[cluster>>3U]|=mask;else layout->bitmap[cluster>>3U]&=(uint8_t)~mask;}
int ntfs_write_bitmap(const NtfsVolume *volume,const NtfsLayout *layout,char **error){return ntfs_write_stream(volume,&layout->bitmap_runs,0,layout->bitmap,layout->bitmap_bytes,error);}

int ntfs_set_volume_dirty(NtfsVolume *volume,NtfsLayout *layout,bool dirty,char **error){
    uint8_t *raw=NULL,*fixed=NULL;if(ntfs_read_record(volume,&layout->mft_runs,3,&raw,&fixed,error)!=0)return -1;NtfsAttributeVec attrs={0};if(ntfs_parse_attributes(fixed,volume->record_size,&attrs,error)!=0){free(raw);free(fixed);return -1;}int result=-1;
    for(size_t i=0;i<attrs.count;++i){NtfsAttribute *a=&attrs.items[i];if(a->type!=NTFS_ATTR_VOLUME_INFORMATION||a->nonresident)continue;uint16_t off=ntfs_u16(fixed,a->offset+20U);uint32_t len=ntfs_u32(fixed,a->offset+16U);if(len<12U||(uint32_t)off+len>a->length){ntfs_set_error(error,"invalid NTFS volume-information attribute");break;}size_t flag_off=(size_t)a->offset+off+10U;uint16_t flags=ntfs_u16(fixed,flag_off);if(!dirty&&validate_flags(flags,true,error)!=0)break;flags=dirty?(uint16_t)(flags|NTFS_VOLUME_DIRTY):(uint16_t)(flags&~NTFS_VOLUME_DIRTY);ntfs_put_u16(fixed,flag_off,flags);if(ntfs_prepare_fixups(fixed,volume->record_size,volume->bytes_per_sector,raw,error)!=0)break;if(ntfs_write_record(volume,&layout->mft_runs,3,raw,error)!=0)break;
        uint64_t mirror_off=volume->mftmirr_lcn*volume->cluster_size+3U*volume->record_size;uint8_t *mirror_raw=ld_xmalloc(volume->record_size),*mirror_fixed=ld_xmalloc(volume->record_size);ssize_t got=ld_pread_full(volume->fd,mirror_raw,volume->record_size,mirror_off);if(got<0||(size_t)got!=volume->record_size||ntfs_apply_fixups(mirror_raw,volume->record_size,volume->bytes_per_sector,mirror_fixed,error)!=0){free(mirror_raw);free(mirror_fixed);break;}NtfsAttributeVec ma={0};if(ntfs_parse_attributes(mirror_fixed,volume->record_size,&ma,error)!=0){free(mirror_raw);free(mirror_fixed);break;}bool found=false;for(size_t j=0;j<ma.count;++j){NtfsAttribute *m=&ma.items[j];if(m->type==NTFS_ATTR_VOLUME_INFORMATION&&!m->nonresident){uint16_t mo=ntfs_u16(mirror_fixed,m->offset+20U);size_t mf=(size_t)m->offset+mo+10U;uint16_t mv=ntfs_u16(mirror_fixed,mf);mv=dirty?(uint16_t)(mv|NTFS_VOLUME_DIRTY):(uint16_t)(mv&~NTFS_VOLUME_DIRTY);ntfs_put_u16(mirror_fixed,mf,mv);found=true;break;}}if(!found){ntfs_set_error(error,"NTFS $MFTMirr lacks $Volume record");ntfs_attributes_free(&ma);free(mirror_raw);free(mirror_fixed);break;}if(ntfs_prepare_fixups(mirror_fixed,volume->record_size,volume->bytes_per_sector,mirror_raw,error)!=0){ntfs_attributes_free(&ma);free(mirror_raw);free(mirror_fixed);break;}ssize_t wrote=ld_pwrite_full(volume->fd,mirror_raw,volume->record_size,mirror_off);ntfs_attributes_free(&ma);free(mirror_raw);free(mirror_fixed);if(wrote<0||(size_t)wrote!=volume->record_size){ntfs_set_error(error,"writing NTFS mirrored volume state failed");break;}if(fsync(volume->fd)!=0){ntfs_set_error(error,"syncing NTFS volume state failed: %s",strerror(errno));break;}result=0;break;}
    if (result != 0 && error != NULL && *error == NULL)
        ntfs_set_error(error, "NTFS volume-information attribute was not found");
    ntfs_attributes_free(&attrs);
    free(raw);
    free(fixed);
    return result;
}
