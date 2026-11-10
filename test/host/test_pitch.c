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

    pitch_note_from_freq(440.0f, 440.0f, &midi, &cents);
    CHECK(midi == 69 && fabsf(cents) < 0.01f, "A4: midi %d cents %.2f", midi, cents);

    pitch_note_from_freq(82.41f, 440.0f, &midi, &cents);
    CHECK(midi == 40 && fabsf(cents) < 0.2f, "E2: midi %d cents %.2f", midi, cents);

    pitch_note_from_freq(41.20f, 440.0f, &midi, &cents);
    CHECK(midi == 28 && fabsf(cents) < 0.5f, "E1: midi %d cents %.2f", midi, cents);

    // 20 cents sharp of A4.
    pitch_note_from_freq(440.0f * powf(2.0f, 20.0f / 1200.0f), 440.0f, &midi, &cents);
    CHECK(midi == 69 && fabsf(cents - 20.0f) < 0.1f,
          "A4+20c: midi %d cents %.2f", midi, cents);

    // 30 cents flat of E2.
    pitch_note_from_freq(82.41f * powf(2.0f, -30.0f / 1200.0f), 440.0f, &midi, &cents);
    CHECK(midi == 40 && fabsf(cents + 30.0f) < 0.3f,
          "E2-30c: midi %d cents %.2f", midi, cents);
}

static void test_reference_pitch(void)
{
    int midi;
    float cents;

    // With the reference at 442 Hz, a 440 Hz tone is a flat A4.
    pitch_note_from_freq(440.0f, 442.0f, &midi, &cents);
    CHECK(midi == 69 && fabsf(cents + 7.85f) < 0.05f, "440 at ref 442: midi %d cents %.2f", midi, cents);
    pitch_note_from_freq(442.0f, 442.0f, &midi, &cents);
    CHECK(midi == 69 && fabsf(cents) < 0.01f, "442 at ref 442: midi %d cents %.2f", midi, cents);
    pitch_note_from_freq(430.0f, 430.0f, &midi, &cents);
    CHECK(midi == 69 && fabsf(cents) < 0.01f, "430 at ref 430: midi %d cents %.2f", midi, cents);
}

static float midi_hz(int midi, float a4)
{
    return a4 * powf(2.0f, (float)(midi - 69) / 12.0f);
}

static void test_nearest_string(void)
{
    static const uint8_t guitar[6] = { 40, 45, 50, 55, 59, 64 };  // E2 A2 D3 G3 B3 E4
    static const uint8_t bass[4] = { 28, 33, 38, 43 };            // E1 A1 D2 G2
    int idx;
    float cents;

    for (int i = 0; i < 6; i++) {
        idx = -1;
        bool ok = pitch_nearest_string(midi_hz(guitar[i], 440.0f), 440.0f, guitar, 6, &idx, &cents);
        CHECK(ok && idx == i && fabsf(cents) < 0.5f, "guitar string %d: idx %d cents %.2f", i, idx, cents);
    }
    for (int i = 0; i < 4; i++) {
        idx = -1;
        bool ok = pitch_nearest_string(midi_hz(bass[i], 440.0f), 440.0f, bass, 4, &idx, &cents);
        CHECK(ok && idx == i && fabsf(cents) < 0.5f, "bass string %d: idx %d cents %.2f", i, idx, cents);
    }

    // Detuned strings keep their sign.
    pitch_nearest_string(midi_hz(45, 440.0f) * powf(2.0f, 20.0f / 1200.0f), 440.0f, guitar, 6, &idx, &cents);
    CHECK(idx == 1 && fabsf(cents - 20.0f) < 0.3f, "A2 +20c: idx %d cents %.2f", idx, cents);
    pitch_nearest_string(midi_hz(28, 440.0f) * powf(2.0f, -30.0f / 1200.0f), 440.0f, bass, 4, &idx, &cents);
    CHECK(idx == 0 && fabsf(cents + 30.0f) < 0.3f, "E1 -30c: idx %d cents %.2f", idx, cents);

    // A string tuned far off still reads as itself until a neighbour is nearer.
    pitch_nearest_string(midi_hz(40, 440.0f) * powf(2.0f, -180.0f / 1200.0f), 440.0f, guitar, 6, &idx, &cents);
    CHECK(idx == 0 && fabsf(cents + 180.0f) < 0.5f, "E2 -180c: idx %d cents %.2f", idx, cents);

    // Accepted behavior, no octave folding: the octave harmonic of the low E
    // (E3) is nearest D3, two semitones sharp of it.
    pitch_nearest_string(midi_hz(52, 440.0f), 440.0f, guitar, 6, &idx, &cents);
    CHECK(idx == 2 && fabsf(cents - 200.0f) < 0.5f, "E3 on guitar: idx %d cents %.2f", idx, cents);

    // Either side of the midpoint between E2 and A2, and an exact tie.
    float mid = midi_hz(40, 440.0f) * powf(2.0f, 250.0f / 1200.0f);
    pitch_nearest_string(mid * 0.999f, 440.0f, guitar, 6, &idx, &cents);
    CHECK(idx == 0 && cents > 0.0f, "below the midpoint: idx %d cents %.2f", idx, cents);
    pitch_nearest_string(mid * 1.001f, 440.0f, guitar, 6, &idx, &cents);
    CHECK(idx == 1 && cents < 0.0f, "above the midpoint: idx %d cents %.2f", idx, cents);
    static const uint8_t twice[2] = { 40, 40 };
    pitch_nearest_string(90.0f, 440.0f, twice, 2, &idx, &cents);
    CHECK(idx == 0, "equal entries: the first one wins: idx %d", idx);

    // The reference shifts the strings with it.
    pitch_nearest_string(midi_hz(45, 440.0f), 442.0f, guitar, 6, &idx, &cents);
    CHECK(idx == 1 && fabsf(cents + 7.85f) < 0.05f, "A2 at ref 442: idx %d cents %.2f", idx, cents);

    // Nothing to match.
    CHECK(!pitch_nearest_string(110.0f, 440.0f, guitar, 0, &idx, &cents), "n = 0");
    CHECK(!pitch_nearest_string(110.0f, 440.0f, NULL, 6, &idx, &cents), "NULL table");
    CHECK(!pitch_nearest_string(0.0f, 440.0f, guitar, 6, &idx, &cents), "freq 0");
    CHECK(!pitch_nearest_string(-5.0f, 440.0f, guitar, 6, &idx, &cents), "negative freq");
    CHECK(!pitch_nearest_string(NAN, 440.0f, guitar, 6, &idx, &cents), "NaN freq");
    CHECK(!pitch_nearest_string(INFINITY, 440.0f, guitar, 6, &idx, &cents), "infinite freq");
    CHECK(!pitch_nearest_string(110.0f, 0.0f, guitar, 6, &idx, &cents), "reference 0");
}

int main(void)
{
    test_sines();
    test_quiet_sine();
    test_harmonic_rich();
    test_missing_fundamental();
    test_rejects();
    test_note_mapping();
    test_reference_pitch();
    test_nearest_string();

    if (g_fail) {
        printf("%d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("OK: all pitch host tests passed\n");
    return 0;
}
