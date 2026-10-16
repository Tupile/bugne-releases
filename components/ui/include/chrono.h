// chrono: stopwatch and countdown timer arithmetic for the Chrono screens.
// Pure (no ESP includes): the caller passes a monotonic microsecond clock
// (esp_timer_get_time on the device), so the state survives leaving the
// screen and the screen sleeping. Host-tested in test/host/test_chrono.c.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHRONO_TIMER_MIN_S     5     // timer setting bounds, seconds
#define CHRONO_TIMER_MAX_S     3600  // 60:00
#define CHRONO_SW_MAX_US       (100LL * 3600 * 1000000)  // display wraps past 99:59:59

// ---- stopwatch ----
typedef struct {
    int64_t acc_us;     // time accumulated before the current run
    int64_t start_us;   // clock at the current run's start (valid when running)
    bool    running;
} chrono_sw_t;

void    chrono_sw_reset(chrono_sw_t *sw);
void    chrono_sw_start(chrono_sw_t *sw, int64_t now_us);   // no-op when running
void    chrono_sw_pause(chrono_sw_t *sw, int64_t now_us);   // no-op when stopped
int64_t chrono_sw_elapsed(const chrono_sw_t *sw, int64_t now_us);

// ---- countdown timer ----
typedef enum { CHRONO_TMR_SET, CHRONO_TMR_RUNNING, CHRONO_TMR_PAUSED } chrono_tmr_state_t;

typedef struct {
    chrono_tmr_state_t state;
    int64_t end_us;     // clock at which it expires (RUNNING)
    int64_t left_us;    // remaining time (PAUSED)
} chrono_tmr_t;

void    chrono_tmr_start(chrono_tmr_t *t, int seconds, int64_t now_us);
void    chrono_tmr_pause(chrono_tmr_t *t, int64_t now_us);   // RUNNING -> PAUSED
void    chrono_tmr_resume(chrono_tmr_t *t, int64_t now_us);  // PAUSED -> RUNNING
void    chrono_tmr_cancel(chrono_tmr_t *t);                  // any -> SET
int64_t chrono_tmr_left(const chrono_tmr_t *t, int64_t now_us);  // >= 0; 0 in SET
bool    chrono_tmr_expired(const chrono_tmr_t *t, int64_t now_us); // RUNNING and left == 0

// Clamp a timer setting (seconds) to CHRONO_TIMER_MIN_S..CHRONO_TIMER_MAX_S.
int chrono_tmr_clamp(int seconds);
// One +/- step on the setting. Minutes field: +/-1 min, seconds kept.
// Seconds field: +/-1 s, wrapping 59 <-> 00 without touching the minutes.
// The result is clamped.
int chrono_tmr_step(int seconds, bool sec_field, int delta);

// "MM:SS.d" below one hour, "H:MM:SS" from one hour (tenths dropped).
void chrono_fmt_sw(char *dst, size_t size, int64_t us);
// Countdown "MM:SS", rounded UP to the second (shows 00:01 until it is over).
void chrono_fmt_tmr(char *dst, size_t size, int64_t us);

#ifdef __cplusplus
}
#endif
