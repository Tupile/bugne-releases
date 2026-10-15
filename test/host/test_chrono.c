// Host unit tests for the stopwatch / countdown arithmetic (chrono.c).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "chrono.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define S(x) ((int64_t)(x) * 1000000)

static void check_fmt_sw(int64_t us, const char *want)
{
    char b[16];
    chrono_fmt_sw(b, sizeof(b), us);
    CHECK(strcmp(b, want) == 0, "fmt_sw(%lld) = %s, want %s", (long long)us, b, want);
}

static void check_fmt_tmr(int64_t us, const char *want)
{
    char b[16];
    chrono_fmt_tmr(b, sizeof(b), us);
    CHECK(strcmp(b, want) == 0, "fmt_tmr(%lld) = %s, want %s", (long long)us, b, want);
}

int main(void)
{
    // Stopwatch: start, pause, resume accumulate; reset clears.
    chrono_sw_t sw;
    chrono_sw_reset(&sw);
    CHECK(chrono_sw_elapsed(&sw, S(100)) == 0, "fresh stopwatch not zero");
    chrono_sw_start(&sw, S(10));
    CHECK(chrono_sw_elapsed(&sw, S(15)) == S(5), "running elapsed");
    chrono_sw_start(&sw, S(12));  // second start ignored
    CHECK(chrono_sw_elapsed(&sw, S(15)) == S(5), "double start moved the origin");
    chrono_sw_pause(&sw, S(20));
    CHECK(chrono_sw_elapsed(&sw, S(500)) == S(10), "paused time keeps running");
    chrono_sw_pause(&sw, S(600));  // second pause ignored
    CHECK(chrono_sw_elapsed(&sw, S(600)) == S(10), "double pause changed it");
    chrono_sw_start(&sw, S(1000));
    CHECK(chrono_sw_elapsed(&sw, S(1003)) == S(13), "resume does not accumulate");
    chrono_sw_reset(&sw);
    CHECK(!sw.running && chrono_sw_elapsed(&sw, S(2000)) == 0, "reset");

    // Timer: start, countdown, pause freezes, resume continues, expiry.
    chrono_tmr_t t = {0};
    chrono_tmr_cancel(&t);
    CHECK(t.state == CHRONO_TMR_SET && chrono_tmr_left(&t, 0) == 0, "set state");
    chrono_tmr_start(&t, 1, S(100));
    CHECK(chrono_tmr_left(&t, S(130)) == S(30), "left while running");
    CHECK(!chrono_tmr_expired(&t, S(159)), "expired early");
    chrono_tmr_pause(&t, S(140));
    CHECK(t.state == CHRONO_TMR_PAUSED && chrono_tmr_left(&t, S(900)) == S(20), "paused left");
    CHECK(!chrono_tmr_expired(&t, S(9000)), "a paused timer expired");
    chrono_tmr_resume(&t, S(1000));
    CHECK(chrono_tmr_left(&t, S(1015)) == S(5), "left after resume");
    CHECK(chrono_tmr_expired(&t, S(1020)), "not expired at the end");
    CHECK(chrono_tmr_left(&t, S(1030)) == 0, "negative left");
    chrono_tmr_cancel(&t);
    CHECK(!chrono_tmr_expired(&t, S(5000)), "cancelled timer expired");

    // Setting bounds.
    CHECK(chrono_tmr_clamp(0) == 1 && chrono_tmr_clamp(61) == 60 && chrono_tmr_clamp(7) == 7, "clamp");
    chrono_tmr_start(&t, 500, 0);
    CHECK(chrono_tmr_left(&t, 0) == S(3600), "start clamps to 60 min");

    // Formatting.
    check_fmt_sw(0, "00:00.0");
    check_fmt_sw(S(61) + 230000, "01:01.2");
    check_fmt_sw(S(3599) + 999999, "59:59.9");
    check_fmt_sw(S(3600), "1:00:00");
    check_fmt_sw(S(2 * 3600 + 5 * 60 + 9), "2:05:09");
    check_fmt_sw(-5, "00:00.0");
    check_fmt_tmr(S(300), "05:00");
    check_fmt_tmr(S(299) + 1, "05:00");
    check_fmt_tmr(1, "00:01");
    check_fmt_tmr(0, "00:00");
    check_fmt_tmr(S(3600), "60:00");

    if (g_fail) {
        printf("chrono: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("chrono: stopwatch, timer, clamp, format passed\n");
    return 0;
}
