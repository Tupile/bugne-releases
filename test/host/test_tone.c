// Host unit tests for the metronome / reference-tone generator (tone.c).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "tone.h"

#include <math.h>
#include <stdbool.h>
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
        tone_metro_fill(st, &s, 1, bpm, beats, 1);
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
    tone_metro_fill(&st, buf, 11025, 120, 4, 1);
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
    tone_metro_fill(&st, &s, 1, 0, 0, 1);
    tone_metro_fill(&st, &s, 1, 100000, 99, 1);
    CHECK(st.beats_started >= 1, "clamped params still click");
}

// ---- Subdivisions ----

// The generator as it was before subdivisions existed, copied verbatim
// (2026-11-10). Subdivision 1 must reproduce it sample for sample.
#define REF_CLICK_AMP 20000.0f
#define REF_TWO_PI    6.28318530718f

static float ref_step_phase(tone_state_t *st, float hz)
{
    float s = sinf(REF_TWO_PI * st->phase);
    st->phase += hz / (float)st->rate;
    if (st->phase >= 1.0f) st->phase -= 1.0f;
    return s;
}

static void ref_metro_fill(tone_state_t *st, int16_t *out, int n, int bpm, int beats)
{
    if (bpm < TONE_BPM_MIN) bpm = TONE_BPM_MIN;
    if (bpm > TONE_BPM_MAX) bpm = TONE_BPM_MAX;
    if (beats < 1) beats = 1;
    if (beats > TONE_BEATS_MAX) beats = TONE_BEATS_MAX;
    const int click_len = (int)(st->rate / 50);  // 20 ms
    for (int i = 0; i < n; i++) {
        if (st->pos_mq >= st->next_mq) {
            st->beat = (st->beat + 1) % beats;
            st->beats_started++;
            st->click_hz = (st->beat == 0 && beats > 1) ? 1500.0f : 1000.0f;
            st->click_left = click_len;
            st->phase = 0.0f;
            st->next_mq += (uint64_t)st->rate * 60000u / (uint64_t)bpm;
        }
        float v = 0.0f;
        if (st->click_left > 0) {
            float env = (float)st->click_left / (float)click_len;
            v = ref_step_phase(st, st->click_hz) * env * env * REF_CLICK_AMP;
            st->click_left--;
        }
        out[i] = (int16_t)lrintf(v);
        st->pos_mq += 1000;
    }
}

static void check_same_as_reference(int bpm, int beats)
{
    static const int CHUNKS[] = { 512, 1, 37, 300, 511, 2 };
    tone_state_t a, b;
    tone_init(&a, RATE);
    tone_init(&b, RATE);
    static int16_t ba[512], bb[512];
    long done = 0, diff = 0;
    for (int c = 0; done < 10L * RATE; c++) {
        int n = CHUNKS[c % 6];
        tone_metro_fill(&a, ba, n, bpm, beats, 1);
        ref_metro_fill(&b, bb, n, bpm, beats);
        for (int i = 0; i < n; i++) if (ba[i] != bb[i]) diff++;
        if (a.beat != b.beat || a.beats_started != b.beats_started) diff++;
        done += n;
    }
    CHECK(diff == 0, "subdivision 1 vs reference at %d bpm, %d beats: %ld differences", bpm, beats, diff);
    CHECK(a.clicks_started == a.beats_started, "subdivision 1: clicks %u beats %u",
          (unsigned)a.clicks_started, (unsigned)a.beats_started);
}

static void test_subdiv_one_is_unchanged(void)
{
    check_same_as_reference(113, 4);
    check_same_as_reference(250, 1);
    check_same_as_reference(30, 7);
}

// Every click onset (beats and sub-clicks): a sample where clicks_started
// increments. `left_before` is what remained of the previous click.
typedef struct { long pos; float hz; int beat; int left_before; bool is_beat; } click_t;

static int metro_clicks(tone_state_t *st, long samples, int bpm, int beats, int subdiv,
                        click_t *out, int max)
{
    int n = 0;
    for (long i = 0; i < samples; i++) {
        uint32_t clicks = st->clicks_started, beats_before = st->beats_started;
        int left = st->click_left;
        int16_t s;
        tone_metro_fill(st, &s, 1, bpm, beats, subdiv);
        if (st->clicks_started != clicks && n < max) {
            out[n].pos = i;
            out[n].hz = st->click_hz;
            out[n].beat = st->beat;
            out[n].left_before = left;
            out[n].is_beat = st->beats_started != beats_before;
            n++;
        }
    }
    return n;
}

static void test_subdiv_grid(void)
{
    // 120 BPM: a beat every 11025 samples exactly.
    for (int sub = 2; sub <= 4; sub++) {
        tone_state_t st;
        tone_init(&st, RATE);
        click_t c[32];
        int n = metro_clicks(&st, 3 * 11025, 120, 4, sub, c, 32);
        CHECK(n == 3 * sub, "x%d: %d clicks in 3 beats", sub, n);
        for (int i = 0; i < n; i++) {
            int beat = i / sub, k = i % sub;
            double want = beat * 11025.0 + 11025.0 * k / sub;
            CHECK(fabs((double)c[i].pos - want) <= 1.0, "x%d click %d at %ld, want %.2f", sub, i, c[i].pos, want);
            CHECK(c[i].is_beat == (k == 0), "x%d click %d beat flag", sub, i);
            CHECK(c[i].beat == beat, "x%d click %d beat index %d", sub, i, c[i].beat);
            float hz = k ? 800.0f : (beat == 0 ? 1500.0f : 1000.0f);
            CHECK(c[i].hz == hz, "x%d click %d hz %.0f", sub, i, c[i].hz);
            CHECK(c[i].left_before == 0, "x%d click %d starts with %d samples left", sub, i, c[i].left_before);
        }
        CHECK(st.beats_started == 3 && st.clicks_started == (uint32_t)(3 * sub),
              "x%d counters: beats %u clicks %u", sub, (unsigned)st.beats_started, (unsigned)st.clicks_started);
    }
    // Fastest setting: 250 BPM x 4, clicks 60 ms apart, each 20 ms long.
    tone_state_t st;
    tone_init(&st, RATE);
    static click_t f[64];
    int n = metro_clicks(&st, 3 * RATE, 250, 4, 4, f, 64);
    CHECK(n >= 48, "250 x4: %d clicks in 3 s", n);
    for (int i = 1; i < n; i++) {
        CHECK(f[i].left_before == 0, "250 x4 click %d overlaps the previous", i);
        CHECK(f[i].pos - f[i - 1].pos >= 1322 && f[i].pos - f[i - 1].pos <= 1324,
              "250 x4 interval %ld, want 1323", f[i].pos - f[i - 1].pos);
    }
}

static void test_subdiv_chunk_invariance(void)
{
    // The output does not depend on how the caller slices its buffers.
    static const int CHUNKS[] = { 512, 1, 37, 300, 511, 2 };
    tone_state_t a, b;
    tone_init(&a, RATE);
    tone_init(&b, RATE);
    static int16_t ba[512];
    long done = 0, diff = 0;
    for (int c = 0; done < 5L * RATE; c++) {
        int n = CHUNKS[c % 6];
        tone_metro_fill(&a, ba, n, 113, 4, 3);
        for (int i = 0; i < n; i++) {
            int16_t s;
            tone_metro_fill(&b, &s, 1, 113, 4, 3);
            if (s != ba[i]) diff++;
        }
        done += n;
    }
    CHECK(diff == 0, "x3 chunked vs sample by sample: %ld differences", diff);
    CHECK(a.clicks_started == b.clicks_started && a.beats_started == b.beats_started, "x3 chunk counters");
}

static void test_subdiv_level(void)
{
    // The sub-click is about half a beat click.
    tone_state_t st;
    tone_init(&st, RATE);
    int16_t *buf = malloc(11025 * sizeof(int16_t));
    tone_metro_fill(&st, buf, 11025, 120, 4, 2);
    int beat_peak = 0, sub_peak = 0;
    for (int i = 0; i < 441; i++) if (abs(buf[i]) > beat_peak) beat_peak = abs(buf[i]);
    for (int i = 5513; i < 5513 + 441; i++) if (abs(buf[i]) > sub_peak) sub_peak = abs(buf[i]);
    CHECK(beat_peak > 8000, "beat peak %d", beat_peak);
    CHECK(sub_peak * 10 > beat_peak * 4 && sub_peak * 10 < beat_peak * 6,
          "sub peak %d against beat peak %d", sub_peak, beat_peak);
    for (int i = 5513 + 441; i < 11025; i++) {
        if (buf[i] != 0) { CHECK(0, "sample %d not silent after the sub-click", i); break; }
    }
    free(buf);
}

static void test_subdiv_no_drift(void)
{
    // 113 BPM x 3 over 1000 beats: the last beat and its sub-clicks sit within
    // one sample of the ideal positions (sub-clicks never move the beat grid).
    tone_state_t st;
    tone_init(&st, RATE);
    static click_t c[3100];
    const double period = RATE * 60.0 / 113.0;
    int n = metro_clicks(&st, (long)(1001 * period), 113, 4, 3, c, 3100);
    CHECK(n >= 3003, "113 x3: %d clicks", n);
    for (int i = 2997; i < 3003 && i < n; i++) {
        double want = (i / 3) * period + period * (i % 3) / 3.0;
        CHECK(fabs((double)c[i].pos - want) <= 1.0, "113 x3 click %d at %ld, want %.2f", i, c[i].pos, want);
    }
}

static void test_subdiv_live_change(void)
{
    tone_state_t st;
    tone_init(&st, RATE);
    click_t c[32];
    // Beat at 0 with x1, then x4 is asked for mid-beat: nothing before the
    // next beat, which then carries its three sub-clicks.
    int n = metro_clicks(&st, 100, 120, 4, 1, c, 32);
    CHECK(n == 1 && c[0].is_beat, "first beat: %d clicks", n);
    n = metro_clicks(&st, 11025 - 100, 120, 4, 4, c, 32);
    CHECK(n == 0, "x4 asked mid-beat: %d clicks before the next beat", n);
    n = metro_clicks(&st, 3000, 120, 4, 4, c, 32);  // beat at 0 (rel.), sub at 2757
    CHECK(n == 2 && c[0].is_beat && !c[1].is_beat, "x4 beat: %d clicks", n);
    // x1 asked mid-beat: the running beat keeps its latched x4.
    n = metro_clicks(&st, 11025 - 3000, 120, 4, 1, c, 32);
    CHECK(n == 2 && !c[0].is_beat && !c[1].is_beat, "x1 asked mid-beat: %d sub-clicks still play", n);
    n = metro_clicks(&st, 11025, 120, 4, 1, c, 32);
    CHECK(n == 1 && c[0].is_beat, "next beat is back to x1: %d clicks", n);
    // A tempo change mid-beat: the running beat keeps its latched period, so
    // its sub-clicks and the next beat stay on the 120 BPM grid.
    tone_init(&st, RATE);
    n = metro_clicks(&st, 100, 120, 4, 3, c, 32);
    CHECK(n == 1 && c[0].is_beat, "x3 first beat: %d clicks", n);
    n = metro_clicks(&st, 11025, 60, 4, 3, c, 32);  // positions are relative to sample 100
    CHECK(n == 3 && !c[0].is_beat && !c[1].is_beat && c[2].is_beat, "tempo change mid-beat: %d clicks", n);
    CHECK(n == 3 && c[0].pos == 3675 - 100 && c[1].pos == 7350 - 100 && c[2].pos == 11025 - 100,
          "tempo change positions %ld %ld %ld", c[0].pos, c[1].pos, c[2].pos);
    n = metro_clicks(&st, 22050, 60, 4, 3, c, 32);  // the new beat runs at 60 BPM
    CHECK(n == 3 && c[0].pos == 7350 - 100 && c[1].pos == 14700 - 100 && c[2].pos == 22050 - 100,
          "after the change: %d clicks at %ld %ld %ld", n, c[0].pos, c[1].pos, c[2].pos);
    // Out of range values are clamped: 99 behaves as 4, 0 and -3 as 1.
    tone_init(&st, RATE);
    n = metro_clicks(&st, 11025, 120, 4, 99, c, 32);
    CHECK(n == TONE_SUBDIV_MAX, "x99 clamps to %d clicks per beat: %d", TONE_SUBDIV_MAX, n);
    tone_init(&st, RATE);
    n = metro_clicks(&st, 11025, 120, 4, 0, c, 32);
    CHECK(n == 1, "x0 clamps to 1: %d", n);
    tone_init(&st, RATE);
    n = metro_clicks(&st, 11025, 120, 4, -3, c, 32);
    CHECK(n == 1, "x-3 clamps to 1: %d", n);
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
    CHECK(fabsf(tone_midi_hz(69, 440.0f) - 440.0f) < 0.01f, "A4 %.3f", tone_midi_hz(69, 440.0f));
    CHECK(fabsf(tone_midi_hz(57, 440.0f) - 220.0f) < 0.01f, "A3 %.3f", tone_midi_hz(57, 440.0f));
    CHECK(fabsf(tone_midi_hz(60, 440.0f) - 261.626f) < 0.01f, "C4 %.3f", tone_midi_hz(60, 440.0f));
    CHECK(fabsf(tone_midi_hz(28, 440.0f) - 41.203f) < 0.01f, "E1 %.3f", tone_midi_hz(28, 440.0f));
    // The whole scale follows the reference.
    CHECK(fabsf(tone_midi_hz(69, 442.0f) - 442.0f) < 0.01f, "A4 at 442 %.3f", tone_midi_hz(69, 442.0f));
    CHECK(fabsf(tone_midi_hz(57, 442.0f) - 221.0f) < 0.01f, "A3 at 442 %.3f", tone_midi_hz(57, 442.0f));
    CHECK(fabsf(tone_midi_hz(69, 430.0f) - 430.0f) < 0.01f, "A4 at 430 %.3f", tone_midi_hz(69, 430.0f));
}

int main(void)
{
    test_metro_grid();
    test_metro_no_drift();
    test_metro_click_shape();
    test_metro_one_beat_no_accent();
    test_metro_live_changes();
    test_subdiv_one_is_unchanged();
    test_subdiv_grid();
    test_subdiv_chunk_invariance();
    test_subdiv_level();
    test_subdiv_no_drift();
    test_subdiv_live_change();
    test_drone();
    test_midi_hz();
    if (g_fail) { printf("tone: %d failure(s)\n", g_fail); return 1; }
    puts("tone: metronome grid, drift, click shape, accent, live changes, subdivisions, drone fades, midi->Hz passed");
    return 0;
}
