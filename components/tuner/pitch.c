#include "pitch.h"
#include <math.h>

// Integration window of the YIN difference function: the tail PITCH_TAU_MAX
// samples of the buffer are only ever read as x[i + tau].
#define PITCH_WIN (PITCH_BUF_SAMPLES - PITCH_TAU_MAX)

// Below this RMS (int16 full scale, ~-62 dBFS) the input is treated as
// silence. Above it, the YIN threshold does the real signal/noise decision.
// Kept low on purpose: a distant unplugged electric guitar is quiet, and
// aperiodic ambient noise passing the gate is still rejected by YIN.
#define PITCH_RMS_GATE 25.0f

// A CMNDF dip must go below this to count as a period. Noise stays near 1.
#define PITCH_YIN_THRESHOLD 0.15f

bool pitch_detect(const int16_t *pcm, float fs, float *freq_hz, float *rms)
{
    double energy = 0.0;
    for (int i = 0; i < PITCH_BUF_SAMPLES; i++) {
        energy += (double)pcm[i] * pcm[i];
    }
    float level = (float)sqrt(energy / PITCH_BUF_SAMPLES);
    if (rms) {
        *rms = level;
    }
    if (level < PITCH_RMS_GATE) {
        return false;
    }

    // Cumulative mean normalized difference d'(tau). d'(0) = 1 by definition.
    static float dp[PITCH_TAU_MAX + 1];
    dp[0] = 1.0f;
    double dsum = 0.0;
    for (int tau = 1; tau <= PITCH_TAU_MAX; tau++) {
        double d = 0.0;
        for (int i = 0; i < PITCH_WIN; i++) {
            double diff = (double)pcm[i] - pcm[i + tau];
            d += diff * diff;
        }
        dsum += d;
        dp[tau] = (dsum > 0.0) ? (float)(d * tau / dsum) : 1.0f;
    }

    // First dip below the threshold, extended to its local minimum. Taking
    // the FIRST dip (smallest tau) avoids locking onto a subharmonic.
    int tau = -1;
    for (int t = 2; t <= PITCH_TAU_MAX; t++) {
        if (dp[t] < PITCH_YIN_THRESHOLD) {
            while (t + 1 <= PITCH_TAU_MAX && dp[t + 1] < dp[t]) {
                t++;
            }
            tau = t;
            break;
        }
    }
    if (tau < 2) {
        return false;
    }

    // Parabolic interpolation of the minimum for sub-sample period accuracy
    // (a whole sample at tau=48, guitar high E, would be off by ~35 cents).
    float better = (float)tau;
    if (tau > 1 && tau < PITCH_TAU_MAX) {
        float a = dp[tau - 1], b = dp[tau], c = dp[tau + 1];
        float denom = a - 2.0f * b + c;
        if (denom > 0.0f) {
            better += 0.5f * (a - c) / denom;
        }
    }
    if (freq_hz) {
        *freq_hz = fs / better;
    }
    return true;
}

void pitch_note_from_freq(float freq_hz, int *midi, float *cents)
{
    float semis = 69.0f + 12.0f * log2f(freq_hz / 440.0f);
    int note = (int)lroundf(semis);
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    if (midi) {
        *midi = note;
    }
    if (cents) {
        *cents = (semis - note) * 100.0f;
    }
}
