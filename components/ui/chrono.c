// Stopwatch and countdown timer arithmetic, see include/chrono.h.
#include "chrono.h"

#include <stdio.h>

void chrono_sw_reset(chrono_sw_t *sw)
{
    sw->acc_us = 0;
    sw->start_us = 0;
    sw->running = false;
}

void chrono_sw_start(chrono_sw_t *sw, int64_t now_us)
{
    if (sw->running) return;
    sw->start_us = now_us;
    sw->running = true;
}

void chrono_sw_pause(chrono_sw_t *sw, int64_t now_us)
{
    if (!sw->running) return;
    sw->acc_us = chrono_sw_elapsed(sw, now_us);
    sw->running = false;
}

int64_t chrono_sw_elapsed(const chrono_sw_t *sw, int64_t now_us)
{
    int64_t run = (sw->running && now_us > sw->start_us) ? now_us - sw->start_us : 0;
    return sw->acc_us + run;
}

int chrono_tmr_clamp(int seconds)
{
    if (seconds < CHRONO_TIMER_MIN_S) return CHRONO_TIMER_MIN_S;
    if (seconds > CHRONO_TIMER_MAX_S) return CHRONO_TIMER_MAX_S;
    return seconds;
}

int chrono_tmr_step(int seconds, bool sec_field, int delta)
{
    int m = seconds / 60, s = seconds % 60;
    if (sec_field) s = ((s + delta) % 60 + 60) % 60;
    else m += delta;
    return chrono_tmr_clamp(m * 60 + s);
}

void chrono_tmr_start(chrono_tmr_t *t, int seconds, int64_t now_us)
{
    t->end_us = now_us + (int64_t)chrono_tmr_clamp(seconds) * 1000000;
    t->left_us = 0;
    t->state = CHRONO_TMR_RUNNING;
}

void chrono_tmr_pause(chrono_tmr_t *t, int64_t now_us)
{
    if (t->state != CHRONO_TMR_RUNNING) return;
    t->left_us = chrono_tmr_left(t, now_us);
    t->state = CHRONO_TMR_PAUSED;
}

void chrono_tmr_resume(chrono_tmr_t *t, int64_t now_us)
{
    if (t->state != CHRONO_TMR_PAUSED) return;
    t->end_us = now_us + t->left_us;
    t->state = CHRONO_TMR_RUNNING;
}

void chrono_tmr_cancel(chrono_tmr_t *t)
{
    t->state = CHRONO_TMR_SET;
    t->end_us = 0;
    t->left_us = 0;
}

int64_t chrono_tmr_left(const chrono_tmr_t *t, int64_t now_us)
{
    switch (t->state) {
    case CHRONO_TMR_RUNNING: return t->end_us > now_us ? t->end_us - now_us : 0;
    case CHRONO_TMR_PAUSED:  return t->left_us;
    default:                 return 0;
    }
}

bool chrono_tmr_expired(const chrono_tmr_t *t, int64_t now_us)
{
    return t->state == CHRONO_TMR_RUNNING && now_us >= t->end_us;
}

void chrono_fmt_sw(char *dst, size_t size, int64_t us)
{
    if (us < 0) us = 0;
    us %= CHRONO_SW_MAX_US;
    long tenths = (long)(us / 100000);
    long s = tenths / 10;
    if (s >= 3600) {
        snprintf(dst, size, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    } else {
        snprintf(dst, size, "%02ld:%02ld.%ld", s / 60, s % 60, tenths % 10);
    }
}

void chrono_fmt_tmr(char *dst, size_t size, int64_t us)
{
    if (us < 0) us = 0;
    long s = (long)((us + 999999) / 1000000);  // round up
    snprintf(dst, size, "%02ld:%02ld", s / 60, s % 60);
}
