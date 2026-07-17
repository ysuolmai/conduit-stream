#include "dmap.h"
#include <string.h>

#define DMAP_CODE(a,b,c,d) \
  (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))
enum { TAG_minm = DMAP_CODE('m','i','n','m'),   // dmap.itemname   = title
       TAG_asar = DMAP_CODE('a','s','a','r'),   // daap.songartist = artist
       TAG_asal = DMAP_CODE('a','s','a','l'),   // daap.songalbum  = album
       TAG_mlit = DMAP_CODE('m','l','i','t') }; // dmap.listingitem = container

static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

static void copy_str(char *dst, const uint8_t *val, uint32_t vlen) {
    uint32_t n = vlen < (DMAP_STR_MAX - 1) ? vlen : (DMAP_STR_MAX - 1);
    if (n) memcpy(dst, val, n);
    dst[n] = '\0';
}

static int walk(const uint8_t *buf, size_t len, dmap_meta_t *out) {
    int count = 0;
    size_t off = 0;
    while (off + 8 <= len) {                       // need code(4)+len(4)
        uint32_t code = rd_be32(buf + off); off += 4;
        uint32_t vlen = rd_be32(buf + off); off += 4;
        if (vlen > len - off) break;               // overrun guard (off <= len)
        const uint8_t *val = buf + off;
        switch (code) {
            case TAG_mlit: count += walk(val, vlen, out); break;                     // recurse
            case TAG_minm: copy_str(out->title,  val, vlen); out->has_title  = true; count++; break;
            case TAG_asar: copy_str(out->artist, val, vlen); out->has_artist = true; count++; break;
            case TAG_asal: copy_str(out->album,  val, vlen); out->has_album  = true; count++; break;
            default: break;                        // ignore unknown tag
        }
        off += vlen;
    }
    return count;
}

int dmap_parse(const uint8_t *buf, size_t len, dmap_meta_t *out) {
    memset(out, 0, sizeof(*out));
    if (!buf) return 0;
    return walk(buf, len, out);
}
