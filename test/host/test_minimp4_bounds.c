// Host tests for the sample-table bounds in the vendored minimp4 demuxer.
// A table's declared entry count must fit in the bytes left in its box:
// on the 32-bit target a larger count wrapped the allocation size and the
// fill loop wrote past the block (code review 2026-11-10, A1).
// Honest limit: the size_t truncation itself needs a 32-bit host; these
// cases pin the contract (count checked against the payload).
#define _POSIX_C_SOURCE 200809L  // strdup, used by minimp4's muxer half
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP4_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "minimp4.h"
#pragma GCC diagnostic pop

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

typedef struct { uint8_t d[4096]; size_t n; } buf_t;

static void put32(buf_t *b, uint32_t v)
{
    b->d[b->n++] = v >> 24; b->d[b->n++] = v >> 16; b->d[b->n++] = v >> 8; b->d[b->n++] = v;
}
static size_t box_begin(buf_t *b, const char *name)
{
    size_t at = b->n;
    put32(b, 0);
    memcpy(b->d + b->n, name, 4); b->n += 4;
    return at;
}
static void box_end(buf_t *b, size_t at)
{
    uint32_t sz = (uint32_t)(b->n - at);
    b->d[at] = sz >> 24; b->d[at + 1] = sz >> 16; b->d[at + 2] = sz >> 8; b->d[at + 3] = sz;
}

static int rd(int64_t off, void *out, size_t size, void *tok)
{
    buf_t *b = tok;
    if (off < 0 || (uint64_t)off + size > b->n) return 1;
    memcpy(out, b->d + off, size);
    return 0;
}

// moov/trak/mdia/minf/stbl with one table box. `count` is the declared
// entry count, `entries` the number actually present.
enum { T_STSZ, T_STSC, T_STCO, T_CO64, T_STTS };
static void build(buf_t *b, int table, uint32_t count, unsigned entries)
{
    static const char *names[] = { "stsz", "stsc", "stco", "co64", "stts" };
    b->n = 0;
    size_t moov = box_begin(b, "moov"), trak = box_begin(b, "trak");
    size_t mdia = box_begin(b, "mdia"), minf = box_begin(b, "minf");
    size_t stbl = box_begin(b, "stbl");
    size_t t = box_begin(b, names[table]);
    put32(b, 0);                       // version + flags
    if (table == T_STSZ) put32(b, 0);  // sample_size 0: per-sample table
    put32(b, count);
    for (unsigned i = 0; i < entries; i++) {
        switch (table) {
        case T_STSZ: put32(b, 100); break;
        case T_STSC: put32(b, 1); put32(b, 1); put32(b, 1); break;
        case T_STCO: put32(b, 64); break;
        case T_CO64: put32(b, 0); put32(b, 64); break;
        case T_STTS: put32(b, 1); put32(b, 1024); break;
        }
    }
    box_end(b, t); box_end(b, stbl); box_end(b, minf); box_end(b, mdia);
    box_end(b, trak); box_end(b, moov);
}

static int open_it(buf_t *b, MP4D_demux_t *mp4)
{
    return MP4D_open(mp4, rd, b, (int64_t)b->n);
}

int main(void)
{
    static buf_t b;
    MP4D_demux_t mp4;
    static const char *names[] = { "stsz", "stsc", "stco", "co64", "stts" };

    // An honest table still opens.
    for (int t = T_STSZ; t <= T_STTS; t++) {
        build(&b, t, 2, 2);
        int ok = open_it(&b, &mp4);
        if (!ok) printf("valid %s rejected\n", names[t]);
        CHECK(ok);
        if (ok) {
            if (t == T_STSZ) CHECK(mp4.track[0].sample_count == 2 && mp4.track[0].entry_size[1] == 100);
            if (t == T_STCO) CHECK(mp4.track[0].chunk_count == 2 && mp4.track[0].chunk_offset[1] == 64);
            MP4D_close(&mp4);
        }
    }

    // A count larger than the box can hold is refused, whatever its size:
    // one entry too many, a mid-size lie, and the 32-bit wrap values.
    static const uint32_t lies[] = { 3, 100000, 0x15555556u, 0x20000001u, 0x40000001u };
    for (int t = T_STSZ; t <= T_STTS; t++) {
        for (size_t k = 0; k < sizeof(lies) / sizeof(lies[0]); k++) {
            build(&b, t, lies[k], 2);
            int ok = open_it(&b, &mp4);
            if (ok) { printf("%s count %u accepted\n", names[t], (unsigned)lies[k]); MP4D_close(&mp4); }
            CHECK(!ok);
        }
    }

    if (fails) { printf("minimp4 bounds: %d FAILED\n", fails); return 1; }
    printf("minimp4 bounds: valid tables open, oversized counts refused: passed\n");
    return 0;
}
