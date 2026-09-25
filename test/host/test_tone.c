// Host unit tests for the metronome / reference-tone generator (tone.c).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "tone.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define RATE 22050

// Generate one sample at a time and record every click onset (a sample where
// beats_started increments), with the beat index and the click frequency.
typedef struct { long pos; int beat; float hz; } onset_t;

static int metro_onsets(tone_state_t *st, long samples, int bpm, int beats,
                        onset_t *out, int max)
{
    int n = 0;
    for (long i = 0; i < samples; i++) {
        uint32_t before = st->beats_started;
        int16_t s;
        tone_metro_fill(st, &s, 1, bpm, beats);
        if (st->beats_started != before && n < max) {
            out[n].pos = i;
            out[n].beat = st->beat;
            out[n].hz = st->click_hz;
            n++;
        }
    }
    return n;
}

static void test_metro_grid(void)
{
    tone_state_t st;
    tone_init(&st, RATE);
    onset_t o[8];
    // 120 BPM at 22050 Hz: exactly 11025 samples per beat, first click at 0.
    int n = metro_onsets(&st, 4 * 11025 + 1, 120, 4, o, 8);
    CHECK(n == 5, "120 bpm: %d onsets in 4 beats + 1 sample", n);
    for (int i = 0; i < n; i++) {
        CHECK(o[i].pos == i * 11025L, "onset %d at %ld", i, o[i].pos);
        CHECK(o[i].beat == i % 4, "onset %d beat %d", i, o[i].beat);
        CHECK(o[i].hz == (i % 4 == 0 ? 1500.0f : 1000.0f), "onset %d hz %.0f", i, o[i].hz);
    }
}

static void test_metro_no_drift(void)
{
    // 113 BPM: 11707.96... samples per beat. After 1000 beats the onset must
    // sit within one sample of the ideal position.
    tone_state_t st;
    tone_init(&st, RATE);
    static onset_t o[1001];
    double period = RATE * 60.0 / 113.0;
    long len = (long)(1000 * period) + 2;
    int n = metro_onsets(&st, len, 113, 3, o, 1001);
    CHECK(n == 1001, "113 bpm: %d onsets", n);
    double err = fabs((double)o[1000].pos - 1000 * period);
    CHECK(err <= 1.0, "drift after 1000 beats: %.2f samples", err);
}

static void test_metro_click_shape(void)
{
    // The click is ~20 ms, then silence until the next beat.
    tone_state_t st;
    tone_init(&st, RATE);
    int16_t *buf = calloc(11025, sizeof(int16_t));
    tone_metro_fill(&st, buf, 11025, 120, 4);
    int peak = 0;
    for (int i = 0; i < 441; i++) if (abs(buf[i]) > peak) peak = abs(buf[i]);
    CHECK(peak > 8000, "click peak %d", peak);
    int loud_after = 0;
    for (int i = 450; i < 11025; i++) if (buf[i] != 0) loud_after++;
    CHECK(loud_after == 0, "%d non-zero samples after the click", loud_after);
    free(buf);
}

static void test_metro_one_beat_no_accent(void)
{
    tone_state_t st;
    tone_init(&st, RATE);
    onset_t o[4];
    int n = metro_onsets(&st, 3 * 11025 + 1, 120, 1, o, 4);
    CHECK(n == 4, "1 beat: %d onsets", n);
    for (int i = 0; i < n; i++) CHECK(o[i].hz == 1000.0f, "1 beat onset %d hz %.0f", i, o[i].hz);
}

static void test_metro_live_changes(void)
{
    // 4 beats, then switch to 2 beats mid-bar: the index wraps into range.
    tone_state_t st;
    tone_init(&st, RATE);
    onset_t o[8];
    int n = metro_onsets(&st, 3 * 11025 + 1, 120, 4, o, 8);   // beats 0,1,2,3
    CHECK(n == 4 && o[3].beat == 3, "before change: n %d", n);
    n = metro_onsets(&st, 2 * 11025, 120, 2, o, 8);
    CHECK(n == 2, "after change: %d onsets", n);
    CHECK(n == 2 && o[0].beat == 0 && o[1].beat == 1, "beats %d %d", o[0].beat, o[1].beat);
    // Tempo change applies from the next beat: 60 BPM = 22050 samples.
    tone_init(&st, RATE);
    n = metro_onsets(&st, 1, 120, 4, o, 8);                    // click at 0
    n = metro_onsets(&st, 11025 + 22050, 60, 4, o, 8);         // next onsets
    CHECK(n == 2, "tempo change: %d onsets", n);
    CHECK(n == 2 && o[0].pos == 11024 && o[1].pos == 11024 + 22050,
          "tempo change positions %ld %ld", o[0].pos, o[1].pos);
    // Bounds: out-of-range BPM and beats are clamped, never divide by zero.
    tone_init(&st, RATE);
    int16_t s;
    tone_metro_fill(&st, &s, 1, 0, 0);
    tone_metro_fill(&st, &s, 1, 100000, 99);
    CHECK(st.beats_started >= 1, "clamped params still click");
}

static void test_drone(void)
{
    tone_state_t st;
    tone_init(&st, RATE);
    int16_t *buf = calloc(RATE, sizeof(int16_t));
    tone_drone_fill(&st, buf, RATE, 440.0f, true);
    // Fade in: silent start, full amplitude after 10 ms.
    CHECK(abs(buf[0]) < 100, "first sample %d", buf[0]);
    CHECK(st.amp >= 0.999f, "amp after 1 s %.3f", st.amp);
    int crossings = 0;
    for (int i = 1; i < RATE; i++) if ((buf[i - 1] < 0) != (buf[i] < 0)) crossings++;
    CHECK(crossings >= 876 && crossings <= 882, "440 Hz: %d sign changes in 1 s", crossings);
    // Fade out: silent within 10 ms of on=false, and stays silent.
    tone_drone_fill(&st, buf, 512, 440.0f, false);
    int tail = 0;
    for (int i = 230; i < 512; i++) if (buf[i] != 0) tail++;
    CHECK(tail == 0, "%d samples after the 10 ms fade-out", tail);
    CHECK(st.amp == 0.0f, "amp after fade-out %.3f", st.amp);
    free(buf);
}

static void test_midi_hz(void)
{
    CHECK(fabsf(tone_midi_hz(69) - 440.0f) < 0.01f, "A4 %.3f", tone_midi_hz(69));
    CHECK(fabsf(tone_midi_hz(57) - 220.0f) < 0.01f, "A3 %.3f", tone_midi_hz(57));
    CHECK(fabsf(tone_midi_hz(60) - 261.626f) < 0.01f, "C4 %.3f", tone_midi_hz(60));
    CHECK(fabsf(tone_midi_hz(28) - 41.203f) < 0.01f, "E1 %.3f", tone_midi_hz(28));
}

int main(void)
{
    test_metro_grid();
    test_metro_no_drift();
    test_metro_click_shape();
    test_metro_one_beat_no_accent();
    test_metro_live_changes();
    test_drone();
    test_midi_hz();
    if (g_fail) { printf("tone: %d failure(s)\n", g_fail); return 1; }
    puts("tone: metronome grid, drift, click shape, accent, live changes, drone fades, midi->Hz passed");
    return 0;
}
