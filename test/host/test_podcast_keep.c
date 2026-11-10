// Host tests for podcast_keep_select / podcast_keep_window.
#include <stdio.h>
#include <string.h>
#include "podcast_keep.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main(void)
{
    bool cached[6] = { true, true, true, true, true, true };
    bool prot[6] = { 0 };
    bool del[6];

    CHECK(podcast_keep_select(cached, prot, NULL, 6, 0, del) == 0, "keep 0 = no limit");
    CHECK(podcast_keep_select(cached, prot, NULL, 6, -3, del) == 0, "negative keep = no limit");
    CHECK(podcast_keep_select(cached, prot, NULL, 6, 6, del) == 0, "keep == count marks nothing");
    CHECK(podcast_keep_select(cached, prot, NULL, 6, 10, del) == 0, "keep > count marks nothing");

    CHECK(podcast_keep_select(cached, prot, NULL, 6, 2, del) == 4, "keep 2 of 6 marks the 4 oldest");
    CHECK(!del[0] && !del[1] && del[2] && del[5], "newest (first) entries stay");

    // Not downloaded: nothing to delete, and the window stays positional.
    cached[0] = false; cached[3] = false;
    CHECK(podcast_keep_select(cached, prot, NULL, 6, 2, del) == 3, "absent files are not marked");
    CHECK(!del[1] && del[2] && !del[3], "window is positional, not a count of files");

    // Protected (playing, favorite, alarm) is never deleted.
    prot[4] = true;
    CHECK(podcast_keep_select(cached, prot, NULL, 6, 2, del) == 2, "protected entry is kept");
    CHECK(!del[4] && del[5], "protected skipped, the others still go");
    CHECK(podcast_keep_select(cached, NULL, NULL, 6, 2, del) == 3, "NULL protect list allowed");

    CHECK(podcast_keep_window(30, 0) == 30, "window: no limit");
    CHECK(podcast_keep_window(30, 10) == 10, "window: limited");
    CHECK(podcast_keep_window(4, 10) == 4, "window: short feed");
    CHECK(podcast_keep_window(0, 5) == 0, "window: empty feed");

    // ---- Dates ----
    int dd, mm, yy;
    CHECK(podcast_date_parse("Mon, 01 Sep 2026 08:00:00 GMT", &dd, &mm, &yy) && dd == 1 && mm == 8 && yy == 2026, "RFC 822");
    CHECK(podcast_date_parse("1 Sep 2026", &dd, &mm, &yy) && dd == 1 && mm == 8 && yy == 2026, "no weekday, one-digit day");
    CHECK(podcast_date_parse("2026-09-01T06:00:00Z", &dd, &mm, &yy) && dd == 1 && mm == 8 && yy == 2026, "ISO");
    CHECK(podcast_date_parse("\n\t  Tue, 15 Dec 2026 10:00:00 +0100", &dd, &mm, &yy) && dd == 15 && mm == 11 && yy == 2026, "leading newline and tab");
    CHECK(podcast_date_parse("\n  2026-12-15", &dd, &mm, &yy) && dd == 15 && mm == 11, "ISO after whitespace");
    CHECK(podcast_date_parse("Wed, 02 Sept 2026 08:00:00 GMT", &dd, &mm, &yy) && dd == 2 && mm == 8 && yy == 2026, "four-letter month");
    CHECK(podcast_date_parse("2 September 2026", &dd, &mm, &yy) && mm == 8 && yy == 2026, "full month name");
    CHECK(podcast_date_parse("mon, 03 sep 2026", &dd, &mm, &yy) && dd == 3 && mm == 8, "lowercase");
    CHECK(podcast_date_parse("Mon, 01 Sep 26 08:00:00 GMT", &dd, &mm, &yy) && yy == 2026, "two-digit year");
    CHECK(!podcast_date_parse("garbage", &dd, &mm, &yy), "garbage");
    CHECK(!podcast_date_parse("", &dd, &mm, &yy), "empty");
    CHECK(!podcast_date_parse(NULL, &dd, &mm, &yy), "NULL");
    CHECK(!podcast_date_parse("32 Sep 2026", &dd, &mm, &yy), "day out of range");
    CHECK(!podcast_date_parse("01 Xyz 2026", &dd, &mm, &yy), "unknown month");
    CHECK(!podcast_date_parse("01 Sep", &dd, &mm, &yy), "no year");
    CHECK(!podcast_date_parse("2026-13-01", &dd, &mm, &yy), "ISO month out of range");
    CHECK(!podcast_date_parse("5 Mayhem 2026", &dd, &mm, &yy), "month must be a real name");
    CHECK(!podcast_date_parse("3 Decimal 2026", &dd, &mm, &yy), "month prefix only");
    CHECK(podcast_date_parse("7 Jan. 2026", &dd, &mm, &yy) && mm == 0, "abbreviation dot");
    CHECK(!podcast_date_parse("01 Sep 202", &dd, &mm, &yy), "3-digit year");
    CHECK(!podcast_date_parse("01 Sep 20260", &dd, &mm, &yy), "5-digit year");
    CHECK(!podcast_date_parse("01 Sep 999999999999999999999", &dd, &mm, &yy), "huge year");
    CHECK(podcast_date_parse("2026-9-1", &dd, &mm, &yy) && dd == 1 && mm == 8, "ISO short parts");

    CHECK(podcast_date_key("Mon, 01 Sep 2026 08:00:00 GMT") == 20260901, "key RFC 822");
    CHECK(podcast_date_key("2026-12-15") == 20261215, "key ISO");
    CHECK(podcast_date_key("garbage") == 0 && podcast_date_key(NULL) == 0, "key unreadable = 0");
    CHECK(podcast_date_key("31 Dec 2025") < podcast_date_key("01 Jan 2026"), "keys order across years");

    // ---- Window by date ----
    bool all[6] = { true, true, true, true, true, true };
    bool d6[6];
    // Newest first: same result as the positional rule.
    int newest_first[6] = { 20260906, 20260905, 20260904, 20260903, 20260902, 20260901 };
    CHECK(podcast_keep_select(all, NULL, newest_first, 6, 2, d6) == 4, "newest-first: 4 marked");
    CHECK(!d6[0] && !d6[1] && d6[2] && d6[3] && d6[4] && d6[5], "newest-first equals positional");
    // Oldest first: the two LAST entries are the newest and stay.
    int oldest_first[6] = { 20260901, 20260902, 20260903, 20260904, 20260905, 20260906 };
    CHECK(podcast_keep_select(all, NULL, oldest_first, 6, 2, d6) == 4, "oldest-first: 4 marked");
    CHECK(d6[0] && d6[1] && d6[2] && d6[3] && !d6[4] && !d6[5], "oldest-first keeps the newest");
    CHECK(!podcast_keep_in_window(oldest_first, 6, 2, 0) && podcast_keep_in_window(oldest_first, 6, 2, 5), "in_window oldest-first");
    // Shuffled.
    int shuffled[6] = { 20260903, 20260906, 20260901, 20260905, 20260902, 20260904 };
    CHECK(podcast_keep_select(all, NULL, shuffled, 6, 3, d6) == 3, "shuffled: 3 marked");
    CHECK(d6[0] && !d6[1] && d6[2] && !d6[3] && d6[4] && !d6[5], "shuffled keeps 06, 05, 04");
    // Same date: the earlier position wins.
    int ties[5] = { 20260901, 20260905, 20260905, 20260905, 20260902 };
    CHECK(podcast_keep_select(all, NULL, ties, 5, 2, d6) == 3, "ties: 3 marked");
    CHECK(d6[0] && !d6[1] && !d6[2] && d6[3] && d6[4], "ties: earlier position wins");
    // All dates identical: exactly the positional rule.
    int same[6] = { 20260901, 20260901, 20260901, 20260901, 20260901, 20260901 };
    CHECK(podcast_keep_select(all, NULL, same, 6, 2, d6) == 4, "identical dates: 4 marked");
    CHECK(!d6[0] && !d6[1] && d6[2] && d6[5], "identical dates equal positional");
    // One unreadable date: the WHOLE feed falls back to the positional rule.
    int one_bad[6] = { 20260901, 20260902, 0, 20260904, 20260905, 20260906 };
    CHECK(podcast_keep_select(all, NULL, one_bad, 6, 2, d6) == 4, "one bad date: 4 marked");
    CHECK(!d6[0] && !d6[1] && d6[2] && d6[3] && d6[4] && d6[5], "one bad date: positional for all");
    CHECK(podcast_keep_in_window(one_bad, 6, 2, 1) && !podcast_keep_in_window(one_bad, 6, 2, 5), "in_window fallback");
    // Same day on an oldest-first feed: the earlier (older) position wins.
    int old_ties[4] = { 20260901, 20260905, 20260905, 20260905 };
    CHECK(podcast_keep_in_window(old_ties, 4, 2, 1) && podcast_keep_in_window(old_ties, 4, 2, 2) &&
          !podcast_keep_in_window(old_ties, 4, 2, 3) && !podcast_keep_in_window(old_ties, 4, 2, 0), "oldest-first ties");
    // A carried-over episode (old date, appended last) ranks last.
    int retained[4] = { 20260906, 20260905, 20260904, 20250101 };
    CHECK(!podcast_keep_in_window(retained, 4, 3, 3) && podcast_keep_in_window(retained, 4, 3, 2), "retained row ranks last");
    // Out of range index, and a dated window larger than the feed.
    CHECK(!podcast_keep_in_window(oldest_first, 6, 2, 6), "index past the end");
    CHECK(podcast_keep_in_window(oldest_first, 6, 9, 0) && podcast_keep_in_window(oldest_first, 6, 6, 0), "dated, keep >= n");
    // No limit, and a window larger than the feed: everything is inside.
    CHECK(podcast_keep_in_window(oldest_first, 6, 0, 0) && podcast_keep_in_window(oldest_first, 6, 9, 0), "no limit / big window");
    CHECK(podcast_keep_in_window(NULL, 6, 2, 1) && !podcast_keep_in_window(NULL, 6, 2, 2), "NULL keys positional");
    // Protected and not-downloaded entries with dates.
    bool some[6] = { true, false, true, true, true, true };
    bool pr[6] = { true, false, false, false, false, false };
    CHECK(podcast_keep_select(some, pr, oldest_first, 6, 2, d6) == 2, "dated: protected and absent skipped");
    CHECK(!d6[0] && !d6[1] && d6[2] && d6[3] && !d6[4] && !d6[5], "dated: only 02 and 03 go");

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("podcast_keep: all tests passed\n");
    return 0;
}
