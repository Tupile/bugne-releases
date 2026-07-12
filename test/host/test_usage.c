// Host unit tests for the daily usage accumulator (usage.c).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "usage.h"

#include <stdio.h>

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define CHECK_INT(got, want, what) \
    CHECK((got) == (want), "%s: got %d, want %d", (what), (int)(got), (int)(want))

#define DAY1 20260712
#define DAY2 20260713

static void test_invalid_date(void)
{
    // No valid clock: ticks and queries are inert.
    CHECK_INT(usage_tick(0), false, "tick date 0 dropped");
    CHECK_INT(usage_tick(-1), false, "tick date -1 dropped");
    CHECK_INT(usage_today(0), 0, "today date 0 is 0");
    CHECK_INT(usage_date(), 0, "no day tracked yet");
}

static void test_accumulate_and_save_signal(void)
{
    // First tick starts the day (rollover signal), then one signal every 60.
    CHECK_INT(usage_tick(DAY1), true, "first tick signals persist");
    CHECK_INT(usage_today(DAY1), 1, "1 s after first tick");
    int signals = 0;
    for (int i = 0; i < 119; i++) {
        if (usage_tick(DAY1)) signals++;
    }
    CHECK_INT(usage_today(DAY1), 120, "120 s accumulated");
    CHECK_INT(signals, 2, "persist signal every 60 ticks");
    CHECK_INT(usage_today(DAY2), 0, "other day reads 0");
    CHECK_INT(usage_date(), DAY1, "tracked day");
    CHECK_INT(usage_seconds(), 120, "tracked seconds");
}

static void test_rollover(void)
{
    // A new date resets the count and signals a persist promptly.
    CHECK_INT(usage_tick(DAY2), true, "rollover signals persist");
    CHECK_INT(usage_today(DAY2), 1, "fresh day restarts at 1");
    CHECK_INT(usage_today(DAY1), 0, "previous day reads 0");
}

static void test_seed(void)
{
    // Boot: persisted state resumes the same day's count.
    usage_seed(DAY2, 3600);
    CHECK_INT(usage_today(DAY2), 3600, "seeded seconds");
    usage_tick(DAY2);
    CHECK_INT(usage_today(DAY2), 3601, "tick continues from seed");

    // A stale persisted day does not leak into the new day.
    usage_seed(DAY1, 500);
    CHECK_INT(usage_today(DAY2), 0, "seed replaces the tracked day");
    CHECK_INT(usage_tick(DAY2), true, "next day rolls over the stale seed");
    CHECK_INT(usage_today(DAY2), 1, "count restarts after stale seed");

    // Bad seeds are ignored.
    usage_seed(0, 999);
    CHECK_INT(usage_date(), DAY2, "seed date 0 ignored");
    usage_seed(DAY2, -5);
    CHECK_INT(usage_today(DAY2), 0, "negative seconds clamp to 0");
}

int main(void)
{
    test_invalid_date();
    test_accumulate_and_save_signal();
    test_rollover();
    test_seed();

    if (g_fail) {
        printf("%d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("all usage tests passed\n");
    return 0;
}
