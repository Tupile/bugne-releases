// Host unit tests for the quiet-hours predicate (quiet.c).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "quiet.h"

#include <stdio.h>

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define CHECK_BOOL(got, want, what) \
    CHECK((got) == (want), "%s: got %d, want %d", (what), (int)(got), (int)(want))

#define ALL 127  // every day
#define BIT(d) (1 << (d))

// Day indices are Monday-based: 0=Mon .. 6=Sun.
#define MON 0
#define TUE 1
#define SUN 6

static void test_disabled(void)
{
    // enabled=0 is false even inside the window.
    CHECK_BOOL(quiet_window_hit(0, ALL, 13*60, 14*60, 13*60 + 30, MON), false,
               "disabled inside window");
}

static void test_same_day(void)
{
    // 13:00-14:00, today's bit set. Half-open [start, end).
    int s = 13*60, e = 14*60, day = BIT(MON);
    CHECK_BOOL(quiet_window_hit(1, day, s, e, 12*60 + 59, MON), false, "12:59 before");
    CHECK_BOOL(quiet_window_hit(1, day, s, e, 13*60,      MON), true,  "13:00 start incl");
    CHECK_BOOL(quiet_window_hit(1, day, s, e, 13*60 + 30, MON), true,  "13:30 inside");
    CHECK_BOOL(quiet_window_hit(1, day, s, e, 13*60 + 59, MON), true,  "13:59 inside");
    CHECK_BOOL(quiet_window_hit(1, day, s, e, 14*60,      MON), false, "14:00 end excl");

    // Wrong day bit -> false even inside the time range.
    CHECK_BOOL(quiet_window_hit(1, BIT(TUE), s, e, 13*60 + 30, MON), false,
               "same-day wrong day bit");

    // days=127 sanity: any day works.
    CHECK_BOOL(quiet_window_hit(1, ALL, s, e, 13*60 + 30, MON), true, "same-day all days");
}

static void test_wrap(void)
{
    // 20:30-07:00 wrap. Day bit = the day the window STARTS (evening).
    int s = 20*60 + 30, e = 7*60;

    // Evening part (now >= start) uses today's bit.
    CHECK_BOOL(quiet_window_hit(1, BIT(MON), s, e, 21*60, MON), true,
               "21:00 today's bit");
    CHECK_BOOL(quiet_window_hit(1, BIT(SUN), s, e, 21*60, MON), false,
               "21:00 only yesterday's bit");
    CHECK_BOOL(quiet_window_hit(1, BIT(MON), s, e, 20*60 + 30, MON), true,
               "20:30 start incl");

    // Post-midnight part (now < end) uses yesterday's bit.
    CHECK_BOOL(quiet_window_hit(1, BIT(SUN), s, e, 3*60, MON), true,
               "03:00 yesterday's bit");
    CHECK_BOOL(quiet_window_hit(1, BIT(MON), s, e, 3*60, MON), false,
               "03:00 only today's bit");

    // End is exclusive.
    CHECK_BOOL(quiet_window_hit(1, ALL, s, e, 7*60, MON), false, "07:00 end excl");

    // Midday gap: outside both parts.
    CHECK_BOOL(quiet_window_hit(1, ALL, s, e, 12*60, MON), false, "12:00 midday gap");

    // days=127 sanity for the wrap shape.
    CHECK_BOOL(quiet_window_hit(1, ALL, s, e, 21*60, MON), true, "wrap all days evening");
    CHECK_BOOL(quiet_window_hit(1, ALL, s, e, 3*60,  MON), true, "wrap all days post-midnight");
}

static void test_weekday_wrap(void)
{
    // today=Monday (0), post-midnight part must check Sunday (bit6).
    int s = 20*60 + 30, e = 7*60;
    CHECK_BOOL(quiet_window_hit(1, BIT(SUN), s, e, 3*60, MON), true,
               "Mon 03:00 checks Sunday bit");
    CHECK_BOOL(quiet_window_hit(1, BIT(MON), s, e, 3*60, MON), false,
               "Mon 03:00 not Monday bit");
}

static void test_start_equals_end(void)
{
    // start == end: window off, even enabled with all days.
    CHECK_BOOL(quiet_window_hit(1, ALL, 8*60, 8*60, 8*60, MON), false,
               "start==end off");
    CHECK_BOOL(quiet_window_hit(1, ALL, 8*60, 8*60, 9*60, MON), false,
               "start==end off (other time)");
}

int main(void)
{
    test_disabled();
    test_same_day();
    test_wrap();
    test_weekday_wrap();
    test_start_equals_end();
    if (g_fail == 0) {
        printf("OK: all quiet host tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", g_fail);
    return 1;
}
