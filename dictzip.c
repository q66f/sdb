/* https://www.rfc-editor.org/rfc/rfc1952 */
/* https://linux.die.net/man/1/dictzip */

#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib-ng.h>

#include "utf/utf.h"
#include "dictzip.h"

#define nil nullptr

enum {
	Cachesets = 4,
};

typedef struct Cache Cache;

struct Cache {
    u64 last_used;
    u32 chunk_idx;
    u8 *data; 
    bool valid;
};

struct Dictzip {
    const u8 *map;
    size_t map_size;

    u32 chunk_length;
    u32 chunk_count;
    u64 *chunk_offsets; 
    u16 *chunk_sizes;
    u32 uncompressed_size;
    
    Cache cache[Cachesets];
    u64 access_counter;
    
    zng_stream strm;
    bool strm_init;
};

static inline u16 read_u16_le(const u8 *p) {
    return (u16)(p[0] | (u32)p[1] << 8);
}

static inline u32 readu32le(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

void dzclose(Dictzip *dz) {
    if (!dz) return;
    if (dz->map && dz->map != MAP_FAILED) munmap((void *)dz->map, dz->map_size);
    if (dz->chunk_offsets) free(dz->chunk_offsets);
    if (dz->chunk_sizes) free(dz->chunk_sizes);
    for (int i = 0; i < Cachesets; i++) {
        if (dz->cache[i].data) free(dz->cache[i].data);
    }
    if (dz->strm_init) {
        zng_inflateEnd(&dz->strm);
    }
    free(dz);
}

Dictzip *dzopen(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return nil;
    
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 18) {
        close(fd);
        return nil;
    }
    
    Dictzip *dz = calloc(1, sizeof(Dictzip));
    dz->map_size = (size_t)st.st_size;
    dz->map = mmap(nil, dz->map_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if(dz->map == MAP_FAILED) goto err;

    const u8 *map = dz->map;
    if (map[0] != 0x1f || map[1] != 0x8b) goto err;
    
    u8 flg = map[3];
    if (!(flg & 0x04)) goto err;

    u32 pos = 10;
    u16 xlen = read_u16_le(map + pos);
    pos += 2;

    u32 end = pos + xlen;
    bool ra_found = false;
    while (pos + 4 <= end) {
      u16 sublen = read_u16_le(map + pos + 2);
      if (map[pos] == 'R' && map[pos + 1] == 'A') {
        dz->chunk_length = read_u16_le(map + pos + 6);
        dz->chunk_count = read_u16_le(map + pos + 8);
        dz->chunk_sizes = malloc(dz->chunk_count * sizeof(u16));
        dz->chunk_offsets = malloc((dz->chunk_count + 1) * sizeof(u64));

        for (u32 i = 0; i < dz->chunk_count; i++)
          dz->chunk_sizes[i] = read_u16_le(map + pos + 10 + i * 2);
        ra_found = true;
        break;
      }
      pos += 4 + sublen;
    }
    if (!ra_found) goto err;
    pos = end;

    for (u8 m = 0x08; m <= 0x10; m <<= 1) {
        if (flg & m) {
            const u8 *p = memchr(map + pos, 0, dz->map_size - pos);
            if (!p) goto err;
            pos = (u32)(p - map) + 1;
        }
    }
    if (flg & 0x02) { pos += 2; }

    dz->chunk_offsets[0] = pos;
    for (u32 i = 0; i < dz->chunk_count; i++) {
        dz->chunk_offsets[i + 1] = dz->chunk_offsets[i] + dz->chunk_sizes[i];
    }
    
    dz->uncompressed_size = readu32le(map + dz->map_size - 4);
    
    dz->strm.zalloc = Z_NULL;
    dz->strm.zfree = Z_NULL;
    dz->strm.opaque = Z_NULL;
    if (zng_inflateInit2(&dz->strm, -15) != Z_OK) goto err; // -15 for using raw DEFLATE format
    dz->strm_init = true;

    for (u32 i = 0; i < Cachesets; i++) {
        dz->cache[i].data = malloc(dz->chunk_length);
    }
    return dz;
err:
    dzclose(dz);
    return nil;

}
static int get_chunk(Dictzip *dz, u32 idx, u8 **out, u32 *out_len) {
    if (idx >= dz->chunk_count) return -1;

    const u32 expected = (idx == dz->chunk_count - 1) 
                       ? (dz->uncompressed_size - idx * dz->chunk_length) 
                       : dz->chunk_length;

    u32 lru_idx = 0;
    u64 min_used = UINT64_MAX;

    for (u32 i = 0; i < Cachesets; i++) {
        Cache *c = &dz->cache[i];
        if (c->valid && c->chunk_idx == idx) {
            c->last_used = ++dz->access_counter;
            *out = c->data;
            *out_len = expected;
            return 0;
        }
        if (!c->valid || c->last_used < min_used) {
            min_used = c->valid ? c->last_used : 0;
            lru_idx = i;
        }
    }

    Cache *c = &dz->cache[lru_idx];

    zng_inflateReset(&dz->strm);
    dz->strm.next_in   = (u8 *)(dz->map + dz->chunk_offsets[idx]);
    dz->strm.avail_in  = dz->chunk_sizes[idx];
    dz->strm.next_out  = c->data;
    dz->strm.avail_out = dz->chunk_length;
    
    int ret = zng_inflate(&dz->strm, Z_SYNC_FLUSH);
    if(ret < 0 && ret != Z_BUF_ERROR) return -1;

    c->valid     = true;
    c->chunk_idx = idx;
    c->last_used = ++dz->access_counter;
    
    *out = c->data;
    *out_len  = (u32)dz->strm.total_out; 
    return 0;
}

s64 dzread(Dictzip *dz, char *buf, u32 nbytes, u32 seek) {
    s64 bytes_read = 0;
    while (bytes_read < nbytes) {
        u32 current_seek = (u32)(seek + bytes_read);
        u32 chunk_index = current_seek / dz->chunk_length;
        u32 chunk_offset = current_seek % dz->chunk_length;
        
        u8 *chunk_data;
        u32 chunk_length;
        if (get_chunk(dz, chunk_index, &chunk_data, &chunk_length) != 0) break;
        
        u32 available = chunk_length > chunk_offset ? chunk_length - chunk_offset : 0;
        if (available == 0) break;
        
        u32 to_copy = (u32)(nbytes - bytes_read);
        if (to_copy > available) to_copy = available;
        
        memcpy(buf + bytes_read, chunk_data + chunk_offset, to_copy);
        bytes_read += to_copy;
    }
    
    return bytes_read;
}

#ifdef Test
int main(int argc, char **argv) {
    if (argc < 2)
        sysfatal("usage: %s [file.dz] [offset] [length]\n", argv[0]);
    
    Dictzip *dz = dzopen(argv[1]);
    if (dz == nil)
        sysfatal("dzopen %s: failed", argv[1]);
    
    u32 offset = 0;
    u32 length = dz->uncompressed_size;
    
    if (argc >= 3) offset = (u32)strtoul(argv[2], nil, 10);
    if (argc >= 4) length = (u32)strtoul(argv[3], nil, 10);
    
    char *buf = malloc(length);
    s64 read_bytes = dzread(dz, buf, length, offset);

    write(1, buf, read_bytes);
    free(buf);
    
    dzclose(dz);
    return 0;
}
#endif
