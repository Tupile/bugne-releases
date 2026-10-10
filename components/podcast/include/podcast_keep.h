#pragma once
// Per-podcast episode limit ("keep the N latest episodes on the SD card").
// Pure logic, no ESP-IDF dependency, so the host tests cover it.

#include <stdbool.h>
#include <stddef.h>

// Mark the downloaded episodes that a "keep `keep`" limit removes from the card.
// The arrays are in manifest order, which is the feed order (newest first in
// practice; carried-over episodes the feed dropped come last). The window is
// positional, the same one the download job fetches: entries 0..keep-1 stay,
// any later entry that is on the card is marked in del[], unless protect[i]
// (playing now, a favorite, an alarm track). keep <= 0 means no limit: nothing
// is marked. del may alias neither input. Returns the number marked.
size_t podcast_keep_select(const bool *cached, const bool *protect, size_t n, int keep, bool *del);

// Number of entries the download job fetches for a feed of `count` episodes.
static inline size_t podcast_keep_window(size_t count, int keep)
{
    return (keep > 0 && count > (size_t)keep) ? (size_t)keep : count;
}
