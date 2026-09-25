#pragma once

#include <stdbool.h>
#include <stdint.h>

// Metronome clicks and reference-tone sine, generated as 16-bit mono PCM.
// Pure (no ESP-IDF), host-tested in test/host/test_tone.c. The playback worker
// owns one tone_state_t and calls a fill function per output frame; the
// parameters are passed on every call so a change applies live.

#define TONE_BPM_MIN   30
#define TONE_BPM_MAX   250
#define TONE_BEATS_MAX 7

typedef struct {
    uint32_t rate;
    uint64_t pos_mq;        // samples generated so far, in 1/1000 sample
    uint64_t next_mq;       // next click onset, same unit (exact beat grid)
    int      beat;          // index of the last started beat, 0 = beat 1
    uint32_t beats_started; // click counter (the UI lights a beat dot on change)
    int      click_left;    // samples left in the current click
    float    click_hz;      // 1500 Hz accent on beat 1, 1000 Hz otherwise
    float    phase;         // 0..1, shared by the click and the drone
    float    amp;           // drone envelope, 0..1
} tone_state_t;

void tone_init(tone_state_t *st, uint32_t rate);

// Metronome: a 20 ms decaying sine at each beat, silence in between. Clicks are
// placed by sample count, so the tempo has no timer jitter. bpm is clamped to
// TONE_BPM_MIN..MAX and beats to 1..TONE_BEATS_MAX; both apply from the next
// beat. Beat 1 is accented only when beats > 1.
void tone_metro_fill(tone_state_t *st, int16_t *out, int n, int bpm, int beats);

// Reference tone: sine at hz, the envelope moving toward 1 (on) or 0 (off) in
// 10 ms, so start, stop and note changes do not click (the phase is kept).
void tone_drone_fill(tone_state_t *st, int16_t *out, int n, float hz, bool on);

// Equal temperament, A4 (MIDI 69) = 440 Hz.
float tone_midi_hz(int midi);
