// Host unit tests for the tag parsers (components/decode/tags.c). Tag data is
// untrusted input from an SD file or a podcast enclosure, so most of these cases
// are malformed on purpose. Two of them are regression tests for the 32-bit wrap
// bugs found in the 2026-07-17 review (they had no test until now).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "tags.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define CHECK_STR(got, want, what) \
    CHECK(strcmp((got), (want)) == 0, "%s: got \"%s\", want \"%s\"", (what), (got), (want))

// ---- ID3v2 builders ----------------------------------------------------------

// Write a v2.3/v2.4 tag header. syncsafe picks the frame-size encoding version.
static size_t id3_header(uint8_t *b, bool v24, uint8_t flags)
{
    memcpy(b, "ID3", 3);
    b[3] = v24 ? 4 : 3;
    b[4] = 0;
    b[5] = flags;
    b[6] = b[7] = b[8] = b[9] = 0;  // tag size: unused by the parser
    return 10;
}

// One v2.3/v2.4 text frame: 4-char id, size, flags, encoding byte, payload.
static size_t id3_frame(uint8_t *b, const char *id, bool v24,
                        uint8_t enc, const void *text, size_t tlen)
{
    memcpy(b, id, 4);
    uint32_t fsize = (uint32_t)(tlen + 1);  // + the encoding byte
    if (v24) {
        b[4] = (fsize >> 21) & 0x7F; b[5] = (fsize >> 14) & 0x7F;
        b[6] = (fsize >> 7) & 0x7F;  b[7] = fsize & 0x7F;
    } else {
        b[4] = fsize >> 24; b[5] = fsize >> 16; b[6] = fsize >> 8; b[7] = fsize;
    }
    b[8] = b[9] = 0;  // frame flags
    b[10] = enc;
    memcpy(b + 11, text, tlen);
    return 11 + tlen;
}

static void test_id3v23_basic(void)
{
    uint8_t b[512] = {0};
    size_t n = id3_header(b, false, 0);
    n += id3_frame(b + n, "TIT2", false, 0, "Song", 4);
    n += id3_frame(b + n, "TPE1", false, 0, "Band", 4);
    n += id3_frame(b + n, "TALB", false, 0, "Record", 6);
    n += id3_frame(b + n, "TRCK", false, 0, "7/12", 4);

    tags_t t = {0};
    tags_parse_id3v2(&t, b, n);
    CHECK_STR(t.title, "Song", "v2.3 title");
    CHECK_STR(t.artist, "Band", "v2.3 artist");
    CHECK_STR(t.album, "Record", "v2.3 album");
    CHECK(t.track == 7, "v2.3 track: got %d, want 7", t.track);
}

static void test_id3v24_syncsafe(void)
{
    uint8_t b[512] = {0};
    size_t n = id3_header(b, true, 0);
    n += id3_frame(b + n, "TIT2", true, 3, "Syncsafe", 8);  // enc 3 = UTF-8
    tags_t t = {0};
    tags_parse_id3v2(&t, b, n);
    CHECK_STR(t.title, "Syncsafe", "v2.4 syncsafe size + UTF-8");
}

static void test_id3v22(void)
{
    // v2.2: 6-byte frame header (3-char id + 3-byte size), no frame flags.
    uint8_t b[64] = {0};
    size_t n = id3_header(b, false, 0);
    b[3] = 2;  // version 2.2
    memcpy(b + n, "TT2", 3);
    b[n + 3] = 0; b[n + 4] = 0; b[n + 5] = 5;  // size = enc byte + "Deux"
    b[n + 6] = 0;                              // ISO-8859-1
    memcpy(b + n + 7, "Deux", 4);
    n += 6 + 5;
    tags_t t = {0};
    tags_parse_id3v2(&t, b, n);
    CHECK_STR(t.title, "Deux", "v2.2 short frame id");
}

static void test_id3_encodings(void)
{
    // ISO-8859-1 high byte becomes 2-byte UTF-8.
    uint8_t latin[] = { 0xC9, 't', 'e' };   // "Éte"
    char out[TAGS_MAX];
    uint8_t frame[8];
    frame[0] = 0;
    memcpy(frame + 1, latin, sizeof(latin));
    tags_id3_text(out, sizeof(out), frame, 1 + sizeof(latin));
    CHECK_STR(out, "\xC3\x89te", "latin1 to UTF-8");

    // UTF-16 little endian with BOM.
    uint8_t u16le[] = { 1, 0xFF, 0xFE, 'O', 0, 'K', 0 };
    tags_id3_text(out, sizeof(out), u16le, sizeof(u16le));
    CHECK_STR(out, "OK", "UTF-16LE with BOM");

    // UTF-16 big endian with BOM.
    uint8_t u16be[] = { 1, 0xFE, 0xFF, 0, 'O', 0, 'K' };
    tags_id3_text(out, sizeof(out), u16be, sizeof(u16be));
    CHECK_STR(out, "OK", "UTF-16BE with BOM");

    // Encoding byte 2 = UTF-16BE without BOM.
    uint8_t u16nobom[] = { 2, 0, 'H', 0, 'i' };
    tags_id3_text(out, sizeof(out), u16nobom, sizeof(u16nobom));
    CHECK_STR(out, "Hi", "UTF-16BE without BOM");

    // Unknown encoding and an empty body must produce an empty string, not junk.
    uint8_t weird[] = { 9, 'x', 'y' };
    tags_id3_text(out, sizeof(out), weird, sizeof(weird));
    CHECK_STR(out, "", "unknown encoding");
    uint8_t empty[] = { 0 };
    tags_id3_text(out, sizeof(out), empty, 0);
    CHECK_STR(out, "", "empty frame body");
}

static void test_id3_truncates(void)
{
    uint8_t b[512] = {0};
    char big[200];
    memset(big, 'a', sizeof(big));
    size_t n = id3_header(b, false, 0);
    n += id3_frame(b + n, "TIT2", false, 0, big, sizeof(big));
    tags_t t = {0};
    tags_parse_id3v2(&t, b, n);
    CHECK(strlen(t.title) == TAGS_MAX - 1, "long title truncated to %d: got %zu",
          TAGS_MAX - 1, strlen(t.title));
}

static void test_id3_first_wins(void)
{
    uint8_t b[512] = {0};
    size_t n = id3_header(b, false, 0);
    n += id3_frame(b + n, "TIT2", false, 0, "First", 5);
    n += id3_frame(b + n, "TIT2", false, 0, "Second", 6);
    tags_t t = {0};
    tags_parse_id3v2(&t, b, n);
    CHECK_STR(t.title, "First", "first frame wins");

    // A field already filled by another source is never overwritten.
    tags_t pre = {0};
    snprintf(pre.title, sizeof(pre.title), "Kept");
    tags_parse_id3v2(&pre, b, n);
    CHECK_STR(pre.title, "Kept", "pre-filled field kept");
}

static void test_id3_extended_header(void)
{
    // Flags bit 0x40 = extended header; its 4-byte size must be skipped so the
    // first real frame is still found.
    uint8_t b[512] = {0};
    size_t n = id3_header(b, false, 0x40);
    b[n] = 0; b[n+1] = 0; b[n+2] = 0; b[n+3] = 6;  // ext header size
    n += 4 + 6;
    n += id3_frame(b + n, "TIT2", false, 0, "Ext", 3);
    tags_t t = {0};
    tags_parse_id3v2(&t, b, n);
    CHECK_STR(t.title, "Ext", "extended header skipped");
}

// Frame-size bounds (2026-07-17 finding M1). Honest limitation: the original bug
// was `pos + hdrlen + fsize > size` wrapping in 32-bit size_t arithmetic, and it
// CANNOT be reproduced here because size_t is 64 bits on the host (and this VM
// has no 32-bit libc to build against). What this pins instead is the guard's
// observable contract, which any rewrite must keep: a frame size beyond the rest
// of the tag, or zero, stops the walk and reads nothing.
static void test_id3_wrap_frame_size(void)
{
    uint8_t b[64] = {0};
    size_t n = id3_header(b, false, 0);
    memcpy(b + n, "TIT2", 4);
    b[n+4] = 0xFF; b[n+5] = 0xFF; b[n+6] = 0xFF; b[n+7] = 0xF0;  // huge fsize
    b[n+8] = b[n+9] = 0;
    b[n+10] = 0;
    memcpy(b + n + 11, "x", 1);
    n += 12;
    tags_t t = {0};
    tags_parse_id3v2(&t, b, n);   // must stop, not read out of bounds
    CHECK_STR(t.title, "", "wrapping frame size rejected");

    // A frame size of exactly 0 must also stop the walk (not loop forever).
    uint8_t z[32] = {0};
    size_t zn = id3_header(z, false, 0);
    memcpy(z + zn, "TIT2", 4);   // size bytes stay 0
    zn += 10;
    tags_t t2 = {0};
    tags_parse_id3v2(&t2, z, zn);
    CHECK_STR(t2.title, "", "zero frame size stops the walk");

    // Truncated tags of every length must be safe.
    for (size_t k = 0; k <= n; k++) {
        tags_t tk = {0};
        tags_parse_id3v2(&tk, b, k);
    }
    CHECK(1, "truncated tags survive");
}

// ---- Vorbis comments ---------------------------------------------------------

static size_t put_u32(uint8_t *b, uint32_t v)
{
    b[0] = v & 0xFF; b[1] = (v >> 8) & 0xFF; b[2] = (v >> 16) & 0xFF; b[3] = v >> 24;
    return 4;
}

// vendor string + comment count + each "KEY=value".
static size_t vorbis_block(uint8_t *b, const char **comments, int count)
{
    size_t n = 0;
    n += put_u32(b + n, 6);
    memcpy(b + n, "vendor", 6);
    n += 6;
    n += put_u32(b + n, (uint32_t)count);
    for (int i = 0; i < count; i++) {
        size_t len = strlen(comments[i]);
        n += put_u32(b + n, (uint32_t)len);
        memcpy(b + n, comments[i], len);
        n += len;
    }
    return n;
}

static void test_vorbis_basic(void)
{
    const char *c[] = { "TITLE=Chanson", "ARTIST=Groupe", "ALBUM=Disque",
                        "ALBUMARTIST=Various", "TRACKNUMBER=4" };
    uint8_t b[256] = {0};
    size_t n = vorbis_block(b, c, 5);
    tags_t t = {0};
    tags_parse_vorbis(&t, b, (uint32_t)n);
    CHECK_STR(t.title, "Chanson", "vorbis title");
    CHECK_STR(t.artist, "Groupe", "vorbis artist");
    CHECK_STR(t.album, "Disque", "vorbis album");
    CHECK_STR(t.album_artist, "Various", "vorbis album artist");
    CHECK(t.track == 4, "vorbis track: got %d, want 4", t.track);

    // Keys are case-insensitive, and an unknown key is ignored.
    const char *c2[] = { "title=Minuscule", "GENRE=Jazz" };
    uint8_t b2[128] = {0};
    size_t n2 = vorbis_block(b2, c2, 2);
    tags_t t2 = {0};
    tags_parse_vorbis(&t2, b2, (uint32_t)n2);
    CHECK_STR(t2.title, "Minuscule", "vorbis lowercase key");
    CHECK_STR(t2.artist, "", "unknown key ignored");
}

// REGRESSION (2026-07-17 finding M2): the vendor length and each comment length
// are attacker-controlled 32-bit values. Both offsets are uint32_t, so these two
// cases wrap on ANY host, not only on the 32-bit target. Each one is built so the
// unfixed parser produces a visible wrong title ("pwned") rather than merely
// reading out of bounds, which keeps the test meaningful without a sanitizer.
static void test_vorbis_wrap_lengths(void)
{
    // Vendor length chosen so `off += 4 + vlen` wraps back to 0. The unfixed
    // parser then reads the comment count from the vendor length itself and
    // parses the bytes below as a comment list.
    uint8_t b[64] = {0};
    size_t n = 0;
    n += put_u32(b + n, 0xFFFFFFFCu);   // vlen: 0 + 4 + vlen == 2^32 -> off = 0
    n += put_u32(b + n, 11);            // read as clen by the unfixed parser
    memcpy(b + n, "TITLE=pwned", 11);
    n += 11;
    tags_t t = {0};
    tags_parse_vorbis(&t, b, (uint32_t)n);
    CHECK_STR(t.title, "", "wrapping vendor length rejected");

    // Comment length chosen so `off + clen` wraps below n, which the old
    // `off + clen > n` test accepted; copy_utf8 then ran past the value.
    uint8_t c[128] = {0};
    size_t m = 0;
    m += put_u32(c + m, 0);             // empty vendor
    m += put_u32(c + m, 1);             // one comment
    m += put_u32(c + m, 0xFFFFFFFFu);   // 12 + clen wraps to 11, below n
    memcpy(c + m, "TITLE=", 6);
    memcpy(c + m + 6, "pwned", 5);      // what an unbounded copy would pick up
    m += 11;
    tags_t t2 = {0};
    tags_parse_vorbis(&t2, c, (uint32_t)m);
    CHECK_STR(t2.title, "", "wrapping comment length rejected");

    // A comment count far larger than the data must stop at the block end.
    uint8_t d[64] = {0};
    size_t k = 0;
    k += put_u32(d + k, 0);
    k += put_u32(d + k, 1000000);
    k += put_u32(d + k, 7);
    memcpy(d + k, "TITLE=y", 7);
    k += 7;
    tags_t t3 = {0};
    tags_parse_vorbis(&t3, d, (uint32_t)k);
    CHECK_STR(t3.title, "y", "oversized count stops at the end");

    // Every truncation of a valid block must be safe.
    const char *cc[] = { "TITLE=Chanson", "ARTIST=Groupe" };
    uint8_t e[256] = {0};
    size_t en = vorbis_block(e, cc, 2);
    for (size_t i = 0; i <= en; i++) {
        tags_t ti = {0};
        tags_parse_vorbis(&ti, e, (uint32_t)i);
    }
    CHECK(1, "truncated vorbis blocks survive");
}

static void test_vorbis_truncates(void)
{
    char big[200];
    memset(big, 'b', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    char comment[256];
    snprintf(comment, sizeof(comment), "TITLE=%s", big);
    const char *c[] = { comment };
    uint8_t b[512] = {0};
    size_t n = vorbis_block(b, c, 1);
    tags_t t = {0};
    tags_parse_vorbis(&t, b, (uint32_t)n);
    CHECK(strlen(t.title) == TAGS_MAX - 1, "long vorbis title truncated: got %zu",
          strlen(t.title));
}

// A fixed-size copy of an untrusted UTF-8 name can cut a multi-byte sequence in
// half. The stray bytes made GET /api/sd/list return invalid UTF-8, so the web
// Files tab's JSON.parse threw and the folder looked empty (seen on a real card).
static void test_utf8_trim_partial(void)
{
    char s[80];

    // Untouched: pure ASCII, and complete sequences of every length.
    strcpy(s, "plain name.mp3");   tags_utf8_trim_partial(s);
    CHECK_STR(s, "plain name.mp3", "ascii untouched");
    strcpy(s, "caf\xC3\xA9");      tags_utf8_trim_partial(s);
    CHECK_STR(s, "caf\xC3\xA9", "complete 2-byte kept");
    strcpy(s, "prix \xE2\x82\xAC"); tags_utf8_trim_partial(s);
    CHECK_STR(s, "prix \xE2\x82\xAC", "complete 3-byte kept");
    strcpy(s, "hi \xF0\x9F\x8E\xB5"); tags_utf8_trim_partial(s);
    CHECK_STR(s, "hi \xF0\x9F\x8E\xB5", "complete 4-byte kept");

    // Cut sequences: the incomplete tail goes.
    strcpy(s, "caf\xC3");          tags_utf8_trim_partial(s);
    CHECK_STR(s, "caf", "2-byte cut after the lead");
    strcpy(s, "prix \xE2");        tags_utf8_trim_partial(s);
    CHECK_STR(s, "prix ", "3-byte cut after the lead");
    strcpy(s, "prix \xE2\x82");    tags_utf8_trim_partial(s);
    CHECK_STR(s, "prix ", "3-byte cut mid-sequence");
    strcpy(s, "hi \xF0\x9F\x8E");  tags_utf8_trim_partial(s);
    CHECK_STR(s, "hi ", "4-byte cut mid-sequence");

    // The exact shape seen on the card: a no-break space (C2 A0) cut in half by
    // a 63-byte copy.
    strcpy(s, "Le brevet en seconde en 2027\xC2");
    tags_utf8_trim_partial(s);
    CHECK_STR(s, "Le brevet en seconde en 2027", "the real bench case");

    // Degenerate input must not hang or read past the buffer.
    strcpy(s, "");                 tags_utf8_trim_partial(s);
    CHECK_STR(s, "", "empty string");
    strcpy(s, "\x82\x82\x82");     tags_utf8_trim_partial(s);
    CHECK_STR(s, "", "nothing but continuation bytes");
    strcpy(s, "ok\x82");           tags_utf8_trim_partial(s);
    CHECK_STR(s, "ok", "stray continuation byte after ascii");
}

int main(void)
{
    test_id3v23_basic();
    test_id3v24_syncsafe();
    test_id3v22();
    test_id3_encodings();
    test_id3_truncates();
    test_id3_first_wins();
    test_id3_extended_header();
    test_id3_wrap_frame_size();
    test_vorbis_basic();
    test_vorbis_wrap_lengths();
    test_vorbis_truncates();
    test_utf8_trim_partial();

    if (g_fail) {
        printf("test_tags: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("test_tags: all tests passed\n");
    return 0;
}
