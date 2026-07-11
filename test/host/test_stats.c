// Host unit tests for the pure stats accumulation logic (stats.c).
// Build and run with test/host/run.sh. No ESP-IDF needed: the persistence
// (stats_init/flush/reset) is compiled out on the host (ESP_PLATFORM undefined).
#include "stats.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

// Tick `n` seconds of one source/title on one date.
static void tick_n(int date, stats_source_t src, const char *title, int n)
{
    for (int i = 0; i < n; i++) stats_tick(date, src, title);
}

static const stats_title_t *find_title(const stats_day_t *d, const char *t)
{
    for (int i = 0; i < d->top_count; i++) {
        if (strcmp(d->top[i].title, t) == 0) return &d->top[i];
    }
    return NULL;
}

static void test_accumulation(void)
{
    stats_clear();
    tick_n(20260704, STATS_SRC_RADIO,   "FIP",     10);
    tick_n(20260704, STATS_SRC_SD,      "Song A",   5);
    tick_n(20260704, STATS_SRC_PODCAST, "Ep 1",     3);
    tick_n(20260704, STATS_SRC_SENDSPIN,"MA track", 7);

    CHECK(stats_day_count() == 1, "one day after single-date ticks (got %d)", stats_day_count());
    const stats_day_t *d = stats_day_at(0);
    CHECK(d->date == 20260704, "date recorded (got %d)", d->date);
    CHECK(d->src_s[STATS_SRC_RADIO] == 10, "radio seconds (got %d)", d->src_s[STATS_SRC_RADIO]);
    CHECK(d->src_s[STATS_SRC_SD] == 5, "sd seconds (got %d)", d->src_s[STATS_SRC_SD]);
    CHECK(d->src_s[STATS_SRC_PODCAST] == 3, "podcast seconds (got %d)", d->src_s[STATS_SRC_PODCAST]);
    CHECK(d->src_s[STATS_SRC_SENDSPIN] == 7, "sendspin seconds (got %d)", d->src_s[STATS_SRC_SENDSPIN]);

    const stats_title_t *fip = find_title(d, "FIP");
    CHECK(fip && fip->seconds == 10, "FIP title seconds");
    const stats_title_t *sa = find_title(d, "Song A");
    CHECK(sa && sa->seconds == 5, "Song A title seconds");
    CHECK(d->top_count == 4, "four distinct titles (got %d)", d->top_count);

    // A tick with no valid date is dropped.
    stats_tick(0, STATS_SRC_RADIO, "FIP");
    CHECK(d->src_s[STATS_SRC_RADIO] == 10, "date<=0 tick dropped (got %d)", d->src_s[STATS_SRC_RADIO]);
}

static void test_title_eviction(void)
{
    stats_clear();
    char name[8];
    // 16 titles, decreasing seconds: title 0 = 20 s ... title 15 = 5 s.
    for (int i = 0; i < STATS_MAX_TITLES; i++) {
        snprintf(name, sizeof(name), "T%d", i);
        tick_n(20260704, STATS_SRC_RADIO, name, 20 - i);
    }
    const stats_day_t *d = stats_day_at(0);
    CHECK(d->top_count == STATS_MAX_TITLES, "16 titles fill the map (got %d)", d->top_count);
    // T15 (5 s) is the smallest. A 17th title must evict it.
    CHECK(find_title(d, "T15") != NULL, "T15 present before eviction");
    stats_tick(20260704, STATS_SRC_RADIO, "NEW");
    CHECK(d->top_count == STATS_MAX_TITLES, "still 16 titles after eviction (got %d)", d->top_count);
    CHECK(find_title(d, "T15") == NULL, "smallest title T15 evicted");
    CHECK(find_title(d, "NEW") != NULL, "new title inserted");
    const stats_title_t *nw = find_title(d, "NEW");
    CHECK(nw && nw->seconds == 1, "new title starts at 1 s");
    CHECK(find_title(d, "T0") != NULL, "largest title kept");
}

static void test_day_rollover(void)
{
    stats_clear();
    tick_n(20260704, STATS_SRC_RADIO, "day1", 30);
    tick_n(20260705, STATS_SRC_SD,    "day2", 40);

    CHECK(stats_day_count() == 2, "two days after rollover (got %d)", stats_day_count());
    const stats_day_t *d1 = stats_day_at(0);
    const stats_day_t *d2 = stats_day_at(1);
    CHECK(d1->date == 20260704 && d1->src_s[STATS_SRC_RADIO] == 30, "day 1 preserved");
    CHECK(d2->date == 20260705 && d2->src_s[STATS_SRC_SD] == 40, "day 2 fresh counters");
    // The new day did not inherit day 1's radio seconds.
    CHECK(d2->src_s[STATS_SRC_RADIO] == 0, "new day radio counter reset (got %d)",
          d2->src_s[STATS_SRC_RADIO]);
    CHECK(d2->top_count == 1, "new day title map reset (got %d)", d2->top_count);
}

static void test_ring_drops_oldest(void)
{
    stats_clear();
    // 31 distinct days: the ring keeps the newest 30, dropping the first.
    for (int i = 0; i < STATS_MAX_DAYS + 1; i++) {
        stats_tick(20260701 + i, STATS_SRC_RADIO, "x");
    }
    CHECK(stats_day_count() == STATS_MAX_DAYS, "ring capped at 30 (got %d)", stats_day_count());
    const stats_day_t *first = stats_day_at(0);
    const stats_day_t *last = stats_day_at(STATS_MAX_DAYS - 1);
    CHECK(first->date == 20260702, "oldest day (20260701) dropped, first is 20260702 (got %d)",
          first->date);
    CHECK(last->date == 20260701 + STATS_MAX_DAYS, "newest day is last (got %d)", last->date);
}

static void test_title_truncation(void)
{
    stats_clear();
    // A title longer than STATS_TITLE_LEN-1 must be stored truncated.
    char longt[200];
    memset(longt, 'A', sizeof(longt) - 1);
    longt[sizeof(longt) - 1] = '\0';
    stats_tick(20260704, STATS_SRC_RADIO, longt);
    const stats_day_t *d = stats_day_at(0);
    CHECK(d->top_count == 1, "one title stored");
    CHECK((int)strlen(d->top[0].title) == STATS_TITLE_LEN - 1,
          "title truncated to %d chars (got %d)", STATS_TITLE_LEN - 1,
          (int)strlen(d->top[0].title));
    // A second tick of the same long title matches the truncated entry.
    stats_tick(20260704, STATS_SRC_RADIO, longt);
    CHECK(d->top_count == 1, "long title matches its truncation, no duplicate (got %d)",
          d->top_count);
    CHECK(d->top[0].seconds == 2, "long title accumulates (got %d)", d->top[0].seconds);
}

int main(void)
{
    test_accumulation();
    test_title_eviction();
    test_day_rollover();
    test_ring_drops_oldest();
    test_title_truncation();
    if (g_fail == 0) {
        printf("OK: all stats host tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", g_fail);
    return 1;
}
