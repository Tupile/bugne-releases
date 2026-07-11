// Host unit tests for the tuner pitch detector (pitch.c).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "pitch.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define FS 16000.0f

static int16_t g_buf[PITCH_BUF_SAMPLES];

static void synth_sine(float freq, float amp)
{
    for (int i = 0; i < PITCH_BUF_SAMPLES; i++) {
        g_buf[i] = (int16_t)(amp * sinf(2.0f * (float)M_PI * freq * i / FS));
    }
}

// Sum of partials, each (freq multiplier, amplitude) pair.
static void synth_partials(float f0, const float *mult, const float *amp, int n)
{
    for (int i = 0; i < PITCH_BUF_SAMPLES; i++) {
        float s = 0.0f;
        for (int p = 0; p < n; p++) {
            s += amp[p] * sinf(2.0f * (float)M_PI * f0 * mult[p] * i / FS);
        }
        g_buf[i] = (int16_t)s;
    }
}

// Deterministic white noise (LCG), zero-mean.
static void synth_noise(float amp)
{
    unsigned s = 12345;
    for (int i = 0; i < PITCH_BUF_SAMPLES; i++) {
        s = s * 1103515245u + 12345u;
        g_buf[i] = (int16_t)(amp * (((s >> 16) & 0x7fff) / 16384.0f - 1.0f));
    }
}

static float cents_between(float got, float want)
{
    return 1200.0f * log2f(got / want);
}

static void expect_pitch(const char *what, float want, float tol_cents)
{
    float freq = 0.0f;
    bool ok = pitch_detect(g_buf, FS, &freq, NULL);
    CHECK(ok, "%s: no pitch detected (want %.2f Hz)", what, want);
    if (ok) {
        float err = cents_between(freq, want);
        CHECK(fabsf(err) <= tol_cents,
              "%s: got %.3f Hz, want %.3f Hz (%.2f cents off)", what, freq, want, err);
    }
}

static void test_sines(void)
{
    // Both open strings sets: double bass E1 A1 D2 G2, guitar E2 A2 D3 G3 B3 E4,
    // plus A4 reference. All must land within 1 cent.
    const float freqs[] = {41.20f, 55.0f, 73.42f, 82.41f, 98.0f, 110.0f,
                           146.83f, 196.0f, 246.94f, 329.63f, 440.0f};
    char what[32];
    for (unsigned i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
        synth_sine(freqs[i], 8000.0f);
        snprintf(what, sizeof(what), "sine %.2f Hz", freqs[i]);
        expect_pitch(what, freqs[i], 1.0f);
    }
}

static void test_quiet_sine(void)
{
    // A faint but real signal (about -40 dBFS) must still be detected.
    synth_sine(82.41f, 300.0f);
    expect_pitch("quiet sine 82.41 Hz", 82.41f, 1.0f);
}

static void test_harmonic_rich(void)
{
    // Sawtooth-like spectrum: fundamental plus decaying harmonics, as a real
    // plucked string. Detune-free detection of the fundamental expected.
    const float mult[] = {1, 2, 3, 4, 5, 6};
    const float amp[]  = {6000, 3000, 2000, 1500, 1200, 1000};
    synth_partials(82.41f, mult, amp, 6);
    expect_pitch("harmonic-rich 82.41 Hz", 82.41f, 1.0f);
    synth_partials(41.20f, mult, amp, 6);
    expect_pitch("harmonic-rich 41.20 Hz", 41.20f, 1.0f);
}

static void test_missing_fundamental(void)
{
    // A small mic can cut a double bass fundamental entirely: only harmonics
    // 2/3/4 remain. The period (and thus the note) is still that of f0.
    const float mult[] = {2, 3, 4};
    const float amp[]  = {5000, 3500, 2500};
    synth_partials(41.20f, mult, amp, 3);
    expect_pitch("missing fundamental 41.20 Hz", 41.20f, 1.0f);
}

static void test_rejects(void)
{
    float freq;

    memset(g_buf, 0, sizeof(g_buf));
    CHECK(!pitch_detect(g_buf, FS, &freq, NULL), "silence detected as pitch");

    synth_noise(8000.0f);
    CHECK(!pitch_detect(g_buf, FS, &freq, NULL), "white noise detected as pitch");

    float rms = -1.0f;
    synth_sine(110.0f, 8000.0f);
    pitch_detect(g_buf, FS, &freq, &rms);
    CHECK(rms > 5000.0f && rms < 6500.0f, "rms of 8000-amp sine: got %.0f", rms);
}

static void test_note_mapping(void)
{
    int midi;
    float cents;

    pitch_note_from_freq(440.0f, &midi, &cents);
    CHECK(midi == 69 && fabsf(cents) < 0.01f, "A4: midi %d cents %.2f", midi, cents);

    pitch_note_from_freq(82.41f, &midi, &cents);
    CHECK(midi == 40 && fabsf(cents) < 0.2f, "E2: midi %d cents %.2f", midi, cents);

    pitch_note_from_freq(41.20f, &midi, &cents);
    CHECK(midi == 28 && fabsf(cents) < 0.5f, "E1: midi %d cents %.2f", midi, cents);

    // 20 cents sharp of A4.
    pitch_note_from_freq(440.0f * powf(2.0f, 20.0f / 1200.0f), &midi, &cents);
    CHECK(midi == 69 && fabsf(cents - 20.0f) < 0.1f,
          "A4+20c: midi %d cents %.2f", midi, cents);

    // 30 cents flat of E2.
    pitch_note_from_freq(82.41f * powf(2.0f, -30.0f / 1200.0f), &midi, &cents);
    CHECK(midi == 40 && fabsf(cents + 30.0f) < 0.3f,
          "E2-30c: midi %d cents %.2f", midi, cents);
}

int main(void)
{
    test_sines();
    test_quiet_sine();
    test_harmonic_rich();
    test_missing_fundamental();
    test_rejects();
    test_note_mapping();

    if (g_fail) {
        printf("%d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("OK: all pitch host tests passed\n");
    return 0;
}
