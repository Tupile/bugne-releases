#include "tone.h"

#include <math.h>

#define CLICK_AMP 20000.0f  // same level as the alarm beep
#define DRONE_AMP 12000.0f  // a continuous sine sounds louder than short clicks
#define TWO_PI    6.28318530718f

void tone_init(tone_state_t *st, uint32_t rate)
{
    *st = (tone_state_t){ .rate = rate, .beat = -1 };
}

static float step_phase(tone_state_t *st, float hz)
{
    float s = sinf(TWO_PI * st->phase);
    st->phase += hz / (float)st->rate;
    if (st->phase >= 1.0f) st->phase -= 1.0f;
    return s;
}

void tone_metro_fill(tone_state_t *st, int16_t *out, int n, int bpm, int beats)
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
            // Advance the grid from the previous onset, not from now, so the
            // truncation of a fractional period never accumulates.
            st->next_mq += (uint64_t)st->rate * 60000u / (uint64_t)bpm;
        }
        float v = 0.0f;
        if (st->click_left > 0) {
            float env = (float)st->click_left / (float)click_len;
            v = step_phase(st, st->click_hz) * env * env * CLICK_AMP;
            st->click_left--;
        }
        out[i] = (int16_t)lrintf(v);
        st->pos_mq += 1000;
    }
}

void tone_drone_fill(tone_state_t *st, int16_t *out, int n, float hz, bool on)
{
    const float step = 1.0f / (0.01f * (float)st->rate);  // 10 ms full swing
    for (int i = 0; i < n; i++) {
        if (on) {
            st->amp += step;
            if (st->amp > 1.0f) st->amp = 1.0f;
        } else {
            st->amp -= step;
            if (st->amp < 0.0f) st->amp = 0.0f;
        }
        out[i] = (int16_t)lrintf(step_phase(st, hz) * st->amp * DRONE_AMP);
    }
}

float tone_midi_hz(int midi)
{
    return 440.0f * powf(2.0f, (float)(midi - 69) / 12.0f);
}
