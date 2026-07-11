// pitch: pure pitch detection for the instrument tuner (no ESP dependencies,
// host-tested in test/host/test_pitch.c).
//
// Algorithm: YIN (cumulative mean normalized difference + absolute threshold
// + parabolic interpolation). Chosen over a spectral peak because it finds
// the period even when the fundamental is weak or missing, which is the case
// for a double bass low E (41 Hz) through a small electret mic.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Samples pitch_detect needs per call. At 16 kHz this is 128 ms, giving a
// ~31 Hz floor (PITCH_TAU_MAX), below the double bass low E (41.2 Hz).
#define PITCH_BUF_SAMPLES 2048
#define PITCH_TAU_MAX     512

// Detect the fundamental frequency of a mono 16-bit PCM buffer of exactly
// PITCH_BUF_SAMPLES samples at fs Hz. Returns true and writes *freq_hz when
// a pitch is confidently found; false on silence, noise, or ambiguity.
// Also writes the buffer RMS (0..32768 scale) to *rms if non-NULL, so the
// caller can log input level while tuning the mic gain.
bool pitch_detect(const int16_t *pcm, float fs, float *freq_hz, float *rms);

// Map a frequency to the nearest equal-tempered note, A4 = 440 Hz.
// *midi gets the MIDI note number (69 = A4, 40 = E2, 28 = E1),
// *cents the signed deviation from that note, -50..+50.
void pitch_note_from_freq(float freq_hz, int *midi, float *cents);

#ifdef __cplusplus
}
#endif
