// buttons: debounce, auto-repeat and volume step. See buttons.h.
#include "buttons.h"

buttons_event_t buttons_update(buttons_state_t *s, bool pressed, uint32_t now_ms, bool repeat)
{
    if (pressed != s->raw) {
        s->raw = pressed;
        s->raw_since = now_ms;
    }
    if (s->raw != s->stable && (uint32_t)(now_ms - s->raw_since) >= BUTTONS_DEBOUNCE_MS) {
        s->stable = s->raw;
        if (s->stable) {
            s->next_repeat = now_ms + BUTTONS_REPEAT_DELAY_MS;
            return BUTTONS_EV_PRESS;
        }
        return BUTTONS_EV_NONE;
    }
    if (repeat && s->stable && (int32_t)(now_ms - s->next_repeat) >= 0) {
        s->next_repeat = now_ms + BUTTONS_REPEAT_MS;
        return BUTTONS_EV_REPEAT;
    }
    return BUTTONS_EV_NONE;
}

int buttons_volume_step(int volume, int dir, int limit)
{
    if (limit < 0) limit = 0;
    if (limit > 100) limit = 100;
    int v = volume + (dir > 0 ? BUTTONS_VOLUME_STEP : dir < 0 ? -BUTTONS_VOLUME_STEP : 0);
    if (v < 0) v = 0;
    if (v > limit) v = limit;
    return v;
}
