// memo_wav: WAV header build/parse and post-capture normalization (pure,
// host-tested).
#include <string.h>
#include "memo.h"

// Normalization target: peak lands just under full scale (~-1 dBFS).
#define NORM_TARGET   28000
#define NORM_GAIN_MAX (24 * 256)  // x24 cap, fixed-point /256 (x16 was still a bit quiet)

static void put_u16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24;
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void memo_wav_header(uint8_t out[MEMO_WAV_HEADER_BYTES], uint32_t data_bytes)
{
    memcpy(out, "RIFF", 4);
    put_u32(out + 4, 36 + data_bytes);
    memcpy(out + 8, "WAVE", 4);
    memcpy(out + 12, "fmt ", 4);
    put_u32(out + 16, 16);                    // PCM fmt chunk size
    put_u16(out + 20, 1);                     // PCM
    put_u16(out + 22, 1);                     // mono
    put_u32(out + 24, MEMO_RATE_HZ);
    put_u32(out + 28, MEMO_RATE_HZ * 2);      // byte rate
    put_u16(out + 32, 2);                     // block align
    put_u16(out + 34, 16);                    // bits per sample
    memcpy(out + 36, "data", 4);
    put_u32(out + 40, data_bytes);
}

bool memo_wav_parse(const uint8_t *buf, size_t len, uint32_t *data_off, uint32_t *data_len)
{
    if (len < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) return false;
    bool fmt_ok = false;
    size_t off = 12;
    while (off + 8 <= len) {
        const uint8_t *id = buf + off;
        uint32_t sz = get_u32(buf + off + 4);
        if (memcmp(id, "fmt ", 4) == 0) {
            const uint8_t *f = buf + off + 8;
            if (sz < 16 || off + 8 + 16 > len) return false;
            if (get_u16(f) != 1 || get_u16(f + 2) != 1 ||
                get_u32(f + 4) != MEMO_RATE_HZ || get_u16(f + 14) != 16) return false;
            fmt_ok = true;
        } else if (memcmp(id, "data", 4) == 0) {
            if (!fmt_ok || sz == 0) return false;
            *data_off = (uint32_t)(off + 8);
            *data_len = sz;
            return true;
        }
        if (sz > len) return false;           // also guards the += against overflow
        off += 8 + sz + (sz & 1);             // chunks are word-aligned
    }
    return false;
}

bool memo_wav_normalize(FILE *f, uint8_t *buf, size_t buf_len)
{
    if (!f || !buf || buf_len < 512) return false;
    buf_len &= ~(size_t)1;                    // whole 16-bit samples per chunk
    if (fseek(f, 0, SEEK_SET) != 0) return false;
    size_t got = fread(buf, 1, 512, f);
    uint32_t data_off = 0, data_len = 0;
    if (!memo_wav_parse(buf, got, &data_off, &data_len)) return false;

    // Pass 1: peak of the data chunk.
    if (fseek(f, (long)data_off, SEEK_SET) != 0) return false;
    uint32_t left = data_len;
    int peak = 0;
    while (left > 0) {
        size_t want = left < buf_len ? left : buf_len;
        size_t n = fread(buf, 1, want, f) & ~(size_t)1;
        if (n == 0) return false;             // shorter than the header claims
        const int16_t *s = (const int16_t *)buf;
        for (size_t i = 0; i < n / 2; i++) {
            int v = s[i] < 0 ? -s[i] : s[i];
            if (v > peak) peak = v;
        }
        left -= (uint32_t)n;
    }
    if (peak == 0 || peak >= NORM_TARGET) return true;  // silent or already at level

    int gain = NORM_TARGET * 256 / peak;      // fixed-point /256
    if (gain > NORM_GAIN_MAX) gain = NORM_GAIN_MAX;

    // Pass 2: scale in place, chunk by chunk (read, scale, seek back, write).
    long pos = (long)data_off;
    left = data_len;
    while (left > 0) {
        size_t want = left < buf_len ? left : buf_len;
        if (fseek(f, pos, SEEK_SET) != 0) return false;
        size_t n = fread(buf, 1, want, f) & ~(size_t)1;
        if (n == 0) return false;
        int16_t *s = (int16_t *)buf;
        for (size_t i = 0; i < n / 2; i++) {
            int v = (s[i] * gain) >> 8;
            if (v > 32767) v = 32767;         // cap never clips (peak*gain <= target),
            if (v < -32768) v = -32768;       // saturate anyway for safety
            s[i] = (int16_t)v;
        }
        if (fseek(f, pos, SEEK_SET) != 0) return false;
        if (fwrite(buf, 1, n, f) != n) return false;
        pos += (long)n;
        left -= (uint32_t)n;
    }
    return fflush(f) == 0;
}
