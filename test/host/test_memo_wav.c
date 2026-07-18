// Host unit tests for the memo WAV header build/parse (memo_wav.c).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "memo.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void test_roundtrip(void)
{
    uint8_t h[MEMO_WAV_HEADER_BYTES];
    memo_wav_header(h, 32000);
    uint32_t off = 0, len = 0;
    CHECK(memo_wav_parse(h, sizeof(h), &off, &len), "own header parses");
    CHECK(off == 44, "data offset 44, got %u", (unsigned)off);
    CHECK(len == 32000, "data len 32000, got %u", (unsigned)len);
}

static void test_extra_chunk(void)
{
    // ffmpeg-style: a LIST chunk between fmt and data must be skipped.
    uint8_t h[MEMO_WAV_HEADER_BYTES];
    memo_wav_header(h, 1000);
    uint8_t buf[80];
    memcpy(buf, h, 36);                       // RIFF..fmt chunk
    memcpy(buf + 36, "LIST", 4);
    buf[40] = 10; buf[41] = 0; buf[42] = 0; buf[43] = 0;
    memset(buf + 44, 0, 10);
    memcpy(buf + 54, h + 36, 8);              // data chunk header
    uint32_t off = 0, len = 0;
    CHECK(memo_wav_parse(buf, sizeof(buf), &off, &len), "LIST chunk skipped");
    CHECK(off == 62, "data offset 62, got %u", (unsigned)off);
    CHECK(len == 1000, "data len 1000, got %u", (unsigned)len);
}

static void test_rejects(void)
{
    uint8_t h[MEMO_WAV_HEADER_BYTES];
    uint32_t off, len;

    memo_wav_header(h, 100);
    h[0] = 'X';
    CHECK(!memo_wav_parse(h, sizeof(h), &off, &len), "bad RIFF rejected");

    memo_wav_header(h, 100);
    h[22] = 2;                                // stereo
    CHECK(!memo_wav_parse(h, sizeof(h), &off, &len), "stereo rejected");

    memo_wav_header(h, 100);
    h[24] = 0x44; h[25] = 0xAC;               // 44100 Hz
    CHECK(!memo_wav_parse(h, sizeof(h), &off, &len), "wrong rate rejected");

    memo_wav_header(h, 100);
    h[34] = 8;                                // 8-bit
    CHECK(!memo_wav_parse(h, sizeof(h), &off, &len), "8-bit rejected");

    memo_wav_header(h, 0);
    CHECK(!memo_wav_parse(h, sizeof(h), &off, &len), "empty data rejected");

    CHECK(!memo_wav_parse(h, 8, &off, &len), "truncated rejected");
}

// Write a small memo WAV with n samples into a temp FILE and return it (r+b).
static FILE *make_wav(const int16_t *pcm, size_t n)
{
    FILE *f = tmpfile();
    uint8_t h[MEMO_WAV_HEADER_BYTES];
    memo_wav_header(h, (uint32_t)(n * 2));
    fwrite(h, 1, sizeof(h), f);
    fwrite(pcm, 2, n, f);
    return f;
}

static int16_t wav_peak(FILE *f)
{
    fseek(f, MEMO_WAV_HEADER_BYTES, SEEK_SET);
    int16_t s, peak = 0;
    while (fread(&s, 2, 1, f) == 1) {
        if (s < 0) s = -s;
        if (s > peak) peak = s;
    }
    return peak;
}

static void test_normalize(void)
{
    uint8_t buf[512];
    int16_t pcm[1024];

    // Moderately quiet: peak 7000 -> gain 28000*256/7000 = 1024 (x4) -> 28000.
    for (size_t i = 0; i < 1024; i++) pcm[i] = (int16_t)((i % 2) ? 7000 : -3500);
    FILE *f = make_wav(pcm, 1024);
    CHECK(memo_wav_normalize(f, buf, sizeof(buf)), "normalize quiet ok");
    CHECK(wav_peak(f) == 28000, "peak scaled to target, got %d", wav_peak(f));
    fclose(f);

    // Very quiet: peak 1000 -> wanted gain > x24, capped -> 24000.
    for (size_t i = 0; i < 1024; i++) pcm[i] = (int16_t)((i % 2) ? 1000 : -500);
    f = make_wav(pcm, 1024);
    CHECK(memo_wav_normalize(f, buf, sizeof(buf)), "normalize very quiet ok");
    CHECK(wav_peak(f) == 24000, "gain capped at x24, got %d", wav_peak(f));
    fclose(f);

    // Already at level: untouched.
    for (size_t i = 0; i < 1024; i++) pcm[i] = (int16_t)((i % 2) ? 30000 : -30000);
    f = make_wav(pcm, 1024);
    CHECK(memo_wav_normalize(f, buf, sizeof(buf)), "normalize loud ok");
    CHECK(wav_peak(f) == 30000, "loud file untouched, got %d", wav_peak(f));
    fclose(f);

    // All-zero (silence): untouched, no divide by zero.
    memset(pcm, 0, sizeof(pcm));
    f = make_wav(pcm, 1024);
    CHECK(memo_wav_normalize(f, buf, sizeof(buf)), "normalize silence ok");
    CHECK(wav_peak(f) == 0, "silence untouched");
    fclose(f);

    // Truncated data chunk (header claims more): must fail, not loop.
    f = make_wav(pcm, 16);
    fseek(f, 40, SEEK_SET);
    uint8_t big[4] = {0x00, 0x10, 0x00, 0x00};  // data size 4096 > real 32
    fwrite(big, 1, 4, f);
    CHECK(!memo_wav_normalize(f, buf, sizeof(buf)), "truncated file rejected");
    fclose(f);
}

int main(void)
{
    test_roundtrip();
    test_extra_chunk();
    test_rejects();
    test_normalize();
    if (g_fail) { printf("%d FAILURES\n", g_fail); return 1; }
    printf("all memo_wav tests passed\n");
    return 0;
}
