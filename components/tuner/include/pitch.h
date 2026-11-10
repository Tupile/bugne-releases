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

// Map a frequency to the nearest equal-tempered note, with A4 = a4_hz
// (440 is the standard). *midi gets the MIDI note number (69 = A4, 40 = E2,
// 28 = E1), *cents the signed deviation from that note, -50..+50.
void pitch_note_from_freq(float freq_hz, float a4_hz, int *midi, float *cents);

// Instrument preset: find the string nearest to freq_hz among `n` MIDI notes
// (ordered low to high), with A4 = a4_hz. *idx gets its index, *cents the
// signed deviation from that string, NOT clamped (a string tuned far off
// reads hundreds of cents). On an exact tie the first (lower) string wins.
// No octave folding: an octave harmonic reads as whatever string is nearest.
// False when there is nothing to match (n <= 0, NULL table, freq <= 0).
bool pitch_nearest_string(float freq_hz, float a4_hz, const uint8_t *midis, int n,
                          int *idx, float *cents);

#ifdef __cplusplus
}
#endif
