#pragma once
// Per-podcast episode limit ("keep the N latest episodes on the SD card").
// Pure logic, no ESP-IDF dependency, so the host tests cover it.

#include <stdbool.h>
#include <stddef.h>

// Read an episode publication date: RFC 822 "[Day, ]DD Mon YYYY ..." (the RSS
// pubDate) or ISO "YYYY-MM-DD...". Tolerant on purpose, because one unreadable
// date sends a whole feed back to the positional window (see below): leading
// whitespace of any kind is skipped (pretty-printed XML), the month may be
// spelled longer than 3 letters ("Sept", "September"), and a 2-digit year is
// 20yy. Month is 0..11. The time of day is ignored. False when unreadable.
bool podcast_date_parse(const char *s, int *day, int *mon, int *year);

// Sort key of a publication date: yyyymmdd, or 0 when unreadable.
int podcast_date_key(const char *date);

// Is manifest entry i one of the `keep` latest episodes? key[] holds each
// entry's podcast_date_key, in manifest order (feed order; carried-over
// episodes the feed dropped come last). The window is the `keep` entries with
// the most recent date; on the same date the earlier position wins, so a
// newest-first feed gives exactly the positional window 0..keep-1.
// ALL OR NOTHING: when key is NULL or ANY key is 0, the whole feed uses the
// positional window (i < keep). A half-dated feed is never ranked by date:
// this drives file deletion. keep <= 0 means no limit: always true.
bool podcast_keep_in_window(const int *key, size_t n, int keep, size_t i);

// Mark the downloaded episodes that a "keep `keep`" limit removes from the
// card: any entry on the card and outside the window (podcast_keep_in_window,
// the same one the download job fetches) is marked in del[], unless
// protect[i] (playing now, a favorite, an alarm track). protect and key may be
// NULL. keep <= 0 means no limit: nothing is marked. del may alias no input.
// Returns the number marked.
size_t podcast_keep_select(const bool *cached, const bool *protect, const int *key,
                           size_t n, int keep, bool *del);

// Size of the window for a feed of `count` episodes (the progress total).
static inline size_t podcast_keep_window(size_t count, int keep)
{
    return (keep > 0 && count > (size_t)keep) ? (size_t)keep : count;
}
