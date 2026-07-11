// played: persistent "already played" markers for podcast episodes.
//
// A fixed in-RAM table of FNV-1a 64-bit hashes of episode_url, capacity
// PLAYED_CAP, kept as a ring buffer (a new mark overwrites the oldest entry
// once full). Persisted to /littlefs/podcasts/played.bin, temp+rename like
// the manifest writes in podcast.c.
//
// Thread safety: played_mark() and played_contains() are called only from the
// UI (LVGL) task (see ui.c: the natural-end mark and the episodes screen both
// run there). No lock is taken; do not call this from another task.
#pragma once

#include <stdbool.h>

// Load the persisted table from flash. Call once, after LittleFS is mounted
// (bg_init_task in bugne_main.c). Before this is called, played_contains()
// returns false (fail open).
void played_init(void);

// Mark an episode as played. No-op if already marked. Saves to flash.
void played_mark(const char *episode_url);

// True if this episode was previously marked played.
bool played_contains(const char *episode_url);
