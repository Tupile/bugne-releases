// buttons: debounce and auto-repeat for the external buttons (config
// "buttons"), plus the volume step. Pure C, no ESP-IDF, host-tested.
//
// The UI task samples each pin every tick (50 ms) and feeds the raw level
// here. A level must hold BUTTONS_DEBOUNCE_MS to count (so two samples in a
// row); the action fires on the press edge, then, for repeating actions
// (volume), again after BUTTONS_REPEAT_DELAY_MS and every BUTTONS_REPEAT_MS
// while held. Release never fires.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BUTTONS_DEBOUNCE_MS     30
#define BUTTONS_REPEAT_DELAY_MS 500
#define BUTTONS_REPEAT_MS       300
#define BUTTONS_VOLUME_STEP     5

typedef struct {
    bool     stable;       // debounced state: true = pressed
    bool     raw;          // last raw sample
    uint32_t raw_since;    // when raw last changed (ms)
    uint32_t next_repeat;  // next auto-repeat time while held (ms)
} buttons_state_t;

typedef enum {
    BUTTONS_EV_NONE = 0,
    BUTTONS_EV_PRESS,   // debounced press edge
    BUTTONS_EV_REPEAT,  // held long enough for another step
} buttons_event_t;

// Zeroed state = released. now_ms may wrap (unsigned differences).
buttons_event_t buttons_update(buttons_state_t *s, bool pressed, uint32_t now_ms, bool repeat);

// New volume after one step: dir > 0 up, < 0 down, clamped to 0..limit.
int buttons_volume_step(int volume, int dir, int limit);
