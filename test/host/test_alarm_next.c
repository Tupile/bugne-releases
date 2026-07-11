// Host unit tests for alarm_next_fire (alarm_next.c). Build and run with
// test/host/run.sh. No ESP-IDF needed (config_store.h's esp_err.h/podcast.h
// dependencies are satisfied by tiny stubs in test/host/stubs, see run.sh).
#include "alarm_next.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define ALL 127  // every day
#define BIT(d) (1 << (d))

// Day indices are Monday-based (0=Mon..6=Sun), matching the days bitmask.
#define MON 0
#define WED 2
#define SAT 5
#define SUN 6

// struct tm's tm_wday is Sunday-based (0=Sun..6=Sat). wday here is that
// standard C value, picked so alarm_next_fire's internal (tm_wday+6)%7
// conversion lands on the Monday-based day intended by each test.
static struct tm mktm(int wday, int hour, int min)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_wday = wday;
    t.tm_hour = hour;
    t.tm_min = min;
    return t;
}

static config_alarm_t alarm_at(int enabled, int hour, int minute, int days)
{
    config_alarm_t a;
    memset(&a, 0, sizeof(a));
    a.enabled = enabled;
    a.hour = hour;
    a.minute = minute;
    a.days = days;
    return a;
}

static void test_same_day_later(void)
{
    config_alarm_t a[1] = { alarm_at(1, 14, 0, ALL) };
    struct tm now = mktm(1 /* Monday */, 8, 0);  // tm_wday=1 -> today=MON
    int mins = -1;
    int idx = alarm_next_fire(a, 1, &now, &mins);
    CHECK(idx == 0, "same-day later: idx got %d want 0", idx);
    CHECK(mins == 360, "same-day later: mins got %d want 360", mins);
}

static void test_tomorrow_wrap(void)
{
    config_alarm_t a[1] = { alarm_at(1, 7, 0, ALL) };
    struct tm now = mktm(1 /* Monday */, 22, 0);
    int mins = -1;
    int idx = alarm_next_fire(a, 1, &now, &mins);
    CHECK(idx == 0, "tomorrow wrap: idx got %d want 0", idx);
    CHECK(mins == 540, "tomorrow wrap: mins got %d want 540", mins);
}

static void test_day_bitmask_exclusion(void)
{
    // Weekend-only alarm, checked on a Wednesday: no occurrence in the next
    // 24h regardless of the time of day (today is Wed, tomorrow is Thu, both
    // excluded by the Sat+Sun mask).
    config_alarm_t a[1] = { alarm_at(1, 7, 0, BIT(SAT) | BIT(SUN)) };
    struct tm now = mktm(3 /* Wednesday */, 6, 0);  // tm_wday=3 -> today=WED
    int mins = -1;
    int idx = alarm_next_fire(a, 1, &now, &mins);
    CHECK(idx == -1, "day-bitmask exclusion: idx got %d want -1", idx);
}

static void test_all_disabled(void)
{
    config_alarm_t a[3] = {
        alarm_at(0, 7, 0, ALL),
        alarm_at(0, 8, 0, ALL),
        alarm_at(0, 9, 0, ALL),
    };
    struct tm now = mktm(1, 6, 0);
    int mins = -1;
    int idx = alarm_next_fire(a, 3, &now, &mins);
    CHECK(idx == -1, "all disabled: idx got %d want -1", idx);
}

static void test_midnight_wrap(void)
{
    // Monday 23:50, alarm at 00:10 fires 20 min later, on Tuesday: only
    // Tuesday's bit gates it (the day the window is entered by the deadline).
    config_alarm_t a[1] = { alarm_at(1, 0, 10, BIT(1 /* Tuesday, Monday-based */)) };
    struct tm now = mktm(1 /* Monday */, 23, 50);
    int mins = -1;
    int idx = alarm_next_fire(a, 1, &now, &mins);
    CHECK(idx == 0, "midnight wrap: idx got %d want 0", idx);
    CHECK(mins == 20, "midnight wrap: mins got %d want 20", mins);
}

static void test_midnight_wrap_wrong_day_excluded(void)
{
    // Same setup, but only Monday's bit is set (not Tuesday's): the
    // post-midnight occurrence must NOT count.
    config_alarm_t a[1] = { alarm_at(1, 0, 10, BIT(MON)) };
    struct tm now = mktm(1, 23, 50);
    int mins = -1;
    int idx = alarm_next_fire(a, 1, &now, &mins);
    CHECK(idx == -1, "midnight wrap wrong day: idx got %d want -1", idx);
}

static void test_nearest_wins(void)
{
    config_alarm_t a[3] = {
        alarm_at(1, 20, 0, ALL),  // far: 12h away
        alarm_at(1, 9, 0, ALL),   // nearest: 1h away
        alarm_at(1, 12, 0, ALL),  // middle: 4h away
    };
    struct tm now = mktm(1, 8, 0);
    int mins = -1;
    int idx = alarm_next_fire(a, 3, &now, &mins);
    CHECK(idx == 1, "nearest wins: idx got %d want 1", idx);
    CHECK(mins == 60, "nearest wins: mins got %d want 60", mins);
}

static void test_tie_lowest_index_wins(void)
{
    config_alarm_t a[3] = {
        alarm_at(1, 9, 0, ALL),
        alarm_at(1, 9, 0, ALL),  // same time as index 0
        alarm_at(1, 10, 0, ALL),
    };
    struct tm now = mktm(1, 8, 0);
    int mins = -1;
    int idx = alarm_next_fire(a, 3, &now, &mins);
    CHECK(idx == 0, "tie lowest index: idx got %d want 0", idx);
    CHECK(mins == 60, "tie lowest index: mins got %d want 60", mins);
}

static void test_fires_exactly_now(void)
{
    config_alarm_t a[1] = { alarm_at(1, 8, 0, ALL) };
    struct tm now = mktm(1, 8, 0);
    int mins = -1;
    int idx = alarm_next_fire(a, 1, &now, &mins);
    CHECK(idx == 0, "fires exactly now: idx got %d want 0", idx);
    CHECK(mins == 0, "fires exactly now: mins got %d want 0", mins);
}

int main(void)
{
    test_same_day_later();
    test_tomorrow_wrap();
    test_day_bitmask_exclusion();
    test_all_disabled();
    test_midnight_wrap();
    test_midnight_wrap_wrong_day_excluded();
    test_nearest_wins();
    test_tie_lowest_index_wins();
    test_fires_exactly_now();
    if (g_fail == 0) {
        printf("OK: all alarm_next host tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", g_fail);
    return 1;
}
