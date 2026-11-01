// podcast_new: persistent "new episode" markers for downloaded podcast episodes.
//
// An episode is NEW when the background download job fetched it while the
// ui.podcast_new_badge setting was on, and nobody has launched it since. It
// drives the red dots: Podcasts home tile, podcast row, episode row (ui.c) and
// the "N new" badge of the web page (GET /api/podcasts/new).
//
// A fixed table of {podcast id, FNV-1a 64-bit hash of episode_url}, capacity
// PODCAST_NEW_CAP, kept as a ring buffer (a new entry overwrites the oldest
// once full). Persisted to /littlefs/podcasts/new.bin, temp+rename.
// Episodes already on the card when the feature is turned on are never NEW:
// only a fresh download adds an entry.
//
// Thread safety: unlike played.c, this is written by the download worker
// (add), the UI task (clear, retain) and read by the httpd task (count), so
// every call takes an internal mutex. Never call it with that mutex held.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Load the persisted table. Call once after LittleFS is mounted. Before it
// runs, every call is a no-op and lookups answer false / 0 (fail closed: no dot).
void podcast_new_init(void);

// Hash used for the table (FNV-1a 64 of the episode URL). Exposed for retain.
uint64_t podcast_new_hash(const char *episode_url);

// Mark a just-downloaded episode of podcast `pod_id` as new. No-op if present.
void podcast_new_add(int pod_id, const char *episode_url);

// The episode was launched: forget it (any podcast). No-op if absent.
void podcast_new_clear(const char *episode_url);

// True if this episode is currently marked new.
bool podcast_new_contains(const char *episode_url);

// Number of new episodes of podcast `pod_id`.
int podcast_new_count(int pod_id);

// Drop every entry of `pod_id` whose hash is not in keep[0..n-1] (episode left
// the feed or the card). n == 0 drops all of that podcast's entries.
void podcast_new_retain(int pod_id, const uint64_t *keep, size_t n);
