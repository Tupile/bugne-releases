// Host unit tests for the sleep-timer fade curve (sleep_fade.c).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "sleep_fade.h"

#include <stdio.h>

static int g_fail;

#define CHECK_INT(got, want, what) do { \
    if ((got) != (want)) { g_fail++; \
        printf("FAIL: %s: got %d, want %d\n", (what), (int)(got), (int)(want)); } \
} while (0)

int main(void)
{
    // Outside the window (or no window): the start volume, untouched.
    CHECK_INT(sleep_fade_vol(60, 30000, 30000), 60, "window start");
    CHECK_INT(sleep_fade_vol(60, 45000, 30000), 60, "before window");
    CHECK_INT(sleep_fade_vol(60, 1000, 0), 60, "no fade length");
    // Linear, rounded to nearest.
    CHECK_INT(sleep_fade_vol(60, 15000, 30000), 30, "half way");
    CHECK_INT(sleep_fade_vol(50, 10000, 30000), 17, "a third left rounds up");
    CHECK_INT(sleep_fade_vol(100, 1000, 30000), 3, "last second");
    // Deadline reached or passed: silent.
    CHECK_INT(sleep_fade_vol(60, 0, 30000), 0, "deadline");
    CHECK_INT(sleep_fade_vol(60, -500, 30000), 0, "past deadline");
    // Degenerate start volumes.
    CHECK_INT(sleep_fade_vol(0, 15000, 30000), 0, "muted start");
    CHECK_INT(sleep_fade_vol(-5, 15000, 30000), 0, "negative start");
    // Never increases as time runs out.
    int prev = 100;
    for (int ms = 30000; ms >= 0; ms -= 1000) {
        int v = sleep_fade_vol(100, ms, 30000);
        if (v > prev) { g_fail++; printf("FAIL: rises at %d ms\n", ms); }
        prev = v;
    }
    if (g_fail) { printf("sleep_fade: %d failure(s)\n", g_fail); return 1; }
    puts("sleep_fade: window, linear ramp, deadline, degenerate inputs passed");
    return 0;
}
