// tags: see tags.h. Pure tag parsing, host-tested in test/host/test_tags.c.
#include "tags.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Append one Unicode code point as UTF-8 (BMP only), bounded by size.
static void utf8_put(char *out, size_t size, size_t *pos, uint32_t cp)
{
    if (cp < 0x80) {
        if (*pos + 1 >= size) return;
        out[(*pos)++] = (char)cp;
    } else if (cp < 0x800) {
        if (*pos + 2 >= size) return;
        out[(*pos)++] = (char)(0xC0 | (cp >> 6));
        out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (*pos + 3 >= size) return;
        out[(*pos)++] = (char)(0xE0 | (cp >> 12));
        out[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
    }
}

static void latin1_to_utf8(char *out, size_t size, const uint8_t *s, size_t n)
{
    size_t pos = 0;
    for (size_t i = 0; i < n && s[i]; i++) utf8_put(out, size, &pos, s[i]);
    out[pos] = '\0';
}

static void utf16_to_utf8(char *out, size_t size, const uint8_t *s, size_t n, bool be)
{
    size_t pos = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t u = be ? ((uint32_t)s[i] << 8 | s[i + 1]) : ((uint32_t)s[i + 1] << 8 | s[i]);
        if (u == 0) break;
        if (u >= 0xD800 && u <= 0xDFFF) continue;  // skip surrogate pairs (no SMP)
        utf8_put(out, size, &pos, u);
    }
    out[pos] = '\0';
}

// Decode an ID3v2 text frame body (leading encoding byte + text) into UTF-8.
void tags_id3_text(char *out, size_t size, const uint8_t *p, size_t len)
{
    out[0] = '\0';
    if (len < 1) return;
    uint8_t enc = p[0];
    const uint8_t *t = p + 1;
    size_t tn = len - 1;
    switch (enc) {
    case 0:  // ISO-8859-1
        latin1_to_utf8(out, size, t, tn);
        break;
    case 3: {  // UTF-8
        size_t k = tn < size - 1 ? tn : size - 1, j = 0;
        while (j < k && t[j]) { out[j] = (char)t[j]; j++; }
        out[j] = '\0';
        break;
    }
    case 1: {  // UTF-16 with BOM
        bool be = false;
        if (tn >= 2 && t[0] == 0xFE && t[1] == 0xFF)      { be = true;  t += 2; tn -= 2; }
        else if (tn >= 2 && t[0] == 0xFF && t[1] == 0xFE) { be = false; t += 2; tn -= 2; }
        utf16_to_utf8(out, size, t, tn, be);
        break;
    }
    case 2:  // UTF-16BE
        utf16_to_utf8(out, size, t, tn, true);
        break;
    default:
        break;
    }
}

// Pull TIT2/TPE1 (or TT2/TP1 for ID3v2.2) from a raw ID3v2 tag. All offsets are
// bounded by size; the tag is untrusted.
void tags_parse_id3v2(tags_t *out, const uint8_t *tag, size_t size)
{
    if (size < 10 || tag[0] != 'I' || tag[1] != 'D' || tag[2] != '3') return;
    uint8_t ver = tag[3];
    uint8_t flags = tag[5];
    bool v22 = (ver == 2);
    size_t hdrlen = v22 ? 6 : 10;  // frame id + size (+ flags for v2.3/2.4)
    size_t pos = 10;
    if (!v22 && (flags & 0x40)) {  // skip an extended header
        if (pos + 4 > size) return;
        size_t ext = (ver == 4)
            ? (((size_t)(tag[pos] & 0x7F) << 21) | ((tag[pos+1] & 0x7F) << 14) |
               ((tag[pos+2] & 0x7F) << 7) | (tag[pos+3] & 0x7F))
            : (((size_t)tag[pos] << 24) | ((size_t)tag[pos+1] << 16) |
               ((size_t)tag[pos+2] << 8) | tag[pos+3]);
        pos += 4 + ext;
    }
    while (pos + hdrlen <= size) {
        const uint8_t *id = tag + pos;
        if (id[0] == 0) break;  // reached padding
        size_t fsize;
        if (v22) {
            fsize = ((size_t)id[3] << 16) | ((size_t)id[4] << 8) | id[5];
        } else if (ver == 4) {
            const uint8_t *s = id + 4;
            fsize = ((size_t)(s[0] & 0x7F) << 21) | ((s[1] & 0x7F) << 14) |
                    ((s[2] & 0x7F) << 7) | (s[3] & 0x7F);
        } else {
            const uint8_t *s = id + 4;
            fsize = ((size_t)s[0] << 24) | ((size_t)s[1] << 16) | ((size_t)s[2] << 8) | s[3];
        }
        if (fsize == 0 || fsize > size - pos - hdrlen) break;  // wrap-safe: the
                                        // while-condition guarantees pos + hdrlen <= size
        const uint8_t *data = tag + pos + hdrlen;
        bool title  = v22 ? !memcmp(id, "TT2", 3)  : !memcmp(id, "TIT2", 4);
        bool artist = v22 ? !memcmp(id, "TP1", 3)  : !memcmp(id, "TPE1", 4);
        bool album  = v22 ? !memcmp(id, "TAL", 3)  : !memcmp(id, "TALB", 4);
        bool track  = v22 ? !memcmp(id, "TRK", 3)  : !memcmp(id, "TRCK", 4);
        if (title  && !out->title[0])  tags_id3_text(out->title,  sizeof(out->title),  data, fsize);
        if (artist && !out->artist[0]) tags_id3_text(out->artist, sizeof(out->artist), data, fsize);
        if (album  && !out->album[0])  tags_id3_text(out->album,  sizeof(out->album),  data, fsize);
        if (track  && !out->track) {  // text like "3" or "3/12"; take the leading number
            char t[TAGS_MAX];
            tags_id3_text(t, sizeof(t), data, fsize);
            out->track = atoi(t);
        }
        pos += hdrlen + fsize;
    }
}

void tags_copy_utf8(char *out, size_t size, const char *s, size_t n)
{
    size_t k = n < size - 1 ? n : size - 1;
    memcpy(out, s, k);
    out[k] = '\0';
}

// Parse a Vorbis comment block already read into memory.
void tags_parse_vorbis(tags_t *out, const uint8_t *p, uint32_t n)
{
    uint32_t off = 0;
    if (off + 4 > n) return;
    uint32_t vlen = p[off] | (p[off+1] << 8) | (p[off+2] << 16) | ((uint32_t)p[off+3] << 24);
    if (vlen > n - off - 4) return;  // wrap-safe: skip vendor string within bounds
    off += 4 + vlen;
    if (off + 4 > n) return;
    uint32_t cnt = p[off] | (p[off+1] << 8) | (p[off+2] << 16) | ((uint32_t)p[off+3] << 24);
    off += 4;
    for (uint32_t i = 0; i < cnt; i++) {
        if (off + 4 > n) return;
        uint32_t clen = p[off] | (p[off+1] << 8) | (p[off+2] << 16) | ((uint32_t)p[off+3] << 24);
        off += 4;
        if (clen > n - off) return;  // wrap-safe (off <= n here)
        const char *c = (const char *)(p + off);
        if      (clen > 6  && !strncasecmp(c, "TITLE=", 6)  && !out->title[0])  tags_copy_utf8(out->title,  sizeof(out->title),  c + 6,  clen - 6);
        else if (clen > 12 && !strncasecmp(c, "ALBUMARTIST=", 12) && !out->album_artist[0]) tags_copy_utf8(out->album_artist, sizeof(out->album_artist), c + 12, clen - 12);
        else if (clen > 7  && !strncasecmp(c, "ARTIST=", 7) && !out->artist[0]) tags_copy_utf8(out->artist, sizeof(out->artist), c + 7,  clen - 7);
        else if (clen > 6  && !strncasecmp(c, "ALBUM=", 6)  && !out->album[0])  tags_copy_utf8(out->album,  sizeof(out->album),  c + 6,  clen - 6);
        else if (clen > 12 && !strncasecmp(c, "TRACKNUMBER=", 12) && !out->track) {
            char t[16];
            tags_copy_utf8(t, sizeof(t), c + 12, clen - 12);
            out->track = atoi(t);
        }
        off += clen;
    }
}
