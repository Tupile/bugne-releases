// Host unit tests for the daily update-check decision (updcheck.c).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "updcheck.h"

#include <stdio.h>

static int g_fail;

#define CHECK(got, want, what) do { \
    if ((got) != (want)) { g_fail++; \
        printf("FAIL: %s: got %d, want %d\n", (what), (int)(got), (int)(want)); } \
} while (0)

#define DAY_US (24LL * 60 * 60 * 1000000)
#define MIN_US (60LL * 1000000)

int main(void)
{
    // Never checked since boot, everything ready: due.
    CHECK(update_check_due(true, true, true, true, false, 5 * MIN_US, 0, DAY_US), true, "first check");
    // Each missing condition blocks it on its own.
    CHECK(update_check_due(false, true, true, true, false, 5 * MIN_US, 0, DAY_US), false, "no check function");
    CHECK(update_check_due(true, false, true, true, false, 5 * MIN_US, 0, DAY_US), false, "no Wi-Fi");
    CHECK(update_check_due(true, true, false, true, false, 5 * MIN_US, 0, DAY_US), false, "audio not idle");
    CHECK(update_check_due(true, true, true, false, false, 5 * MIN_US, 0, DAY_US), false, "device busy");
    // Already checked: throttled until the interval has elapsed.
    CHECK(update_check_due(true, true, true, true, true, 10 * MIN_US, 5 * MIN_US, DAY_US), false, "five minutes later");
    CHECK(update_check_due(true, true, true, true, true, 5 * MIN_US + DAY_US - 1, 5 * MIN_US, DAY_US), false, "just under a day");
    CHECK(update_check_due(true, true, true, true, true, 5 * MIN_US + DAY_US, 5 * MIN_US, DAY_US), true, "exactly a day");
    CHECK(update_check_due(true, true, true, true, true, 3 * DAY_US, 5 * MIN_US, DAY_US), true, "long after");
    // A dropped request clears "checked" but leaves a recent timestamp
    // behind: the stale timestamp must not throttle the retry.
    CHECK(update_check_due(true, true, true, true, false, 10 * MIN_US, 9 * MIN_US, DAY_US), true, "retry after a drop");
    // Throttled and busy at once: still not due.
    CHECK(update_check_due(true, true, true, false, true, 3 * DAY_US, 5 * MIN_US, DAY_US), false, "due but busy");

    if (g_fail) { printf("test_updcheck: %d FAILED\n", g_fail); return 1; }
    printf("test_updcheck: all passed\n");
    return 0;
}
