// podcast: on-device RSS parsing to a manifest, and manifest reading.
//
// Streaming SAX parsing with yxml (vendored), never loading the whole XML in
// RAM. Caps at the most recent episodes. All RSS text is untrusted: lengths are
// bounded here and JSON-escaped on write. Produces the fixed-schema manifest
// (see docs/manifest_schema.md). The player only ever reads the manifest.
//
// Episode caching to SD is explicit and user-triggered; it is added with the UI
// that triggers it.
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#define PODCAST_TITLE_MAX     128
#define PODCAST_DATE_MAX      40
#define PODCAST_URL_MAX       512   // episode URLs (e.g. Radio France) can be long
#define PODCAST_PATH_MAX      256   // holds /sdcard/podcasts/<title>/<episode>.<ext>
// Safety cap only: the manifest is written and read one episode at a time, so this
// just bounds a pathological feed. Kept in sync with RSS_MAX_EPISODES.
#define PODCAST_MAX_EPISODES  300

typedef struct {
    char title[PODCAST_TITLE_MAX];
    char date[PODCAST_DATE_MAX];      // raw RSS pubDate
    int  duration_seconds;
    char episode_url[PODCAST_URL_MAX];
    char cache_path[PODCAST_PATH_MAX];
    bool cached;
} podcast_episode_t;

// Fetch the RSS feed, parse it, and write the manifest for podcast `id` to
// /littlefs/podcasts/<id>.json (internal flash, so no SD card is needed).
// `name` is the podcast's display title (from the config): it names the SD
// folder for cached episodes. May be "" to fall back to the feed title.
// Blocks on the network fetch; call from a worker task, not the UI task.
esp_err_t podcast_refresh(int id, const char *name, const char *rss_url);

// Read the manifest for podcast `id` into eps (up to `max` entries). Sets
// *count. Returns ESP_ERR_NOT_FOUND if no manifest exists. Each episode's
// `cached` flag reflects whether its file actually exists on the SD card.
esp_err_t podcast_read_manifest(int id, podcast_episode_t *eps, size_t max, size_t *count);

// Number of episodes in podcast `id`'s manifest (0 if none). Streams the file, so
// it does not load the whole manifest into RAM. Use it to size the read buffer.
size_t podcast_manifest_count(int id);

// True if the episode's cache_path file exists on the SD card (non-empty).
bool podcast_episode_cached(const podcast_episode_t *ep);

// Download an episode's audio to its cache_path on SD for offline playback.
// HTTP(S) GET following redirects, written atomically (a .part temp then
// renamed). When the file is MP3 and skip_seconds > 0, the first skip_seconds
// are trimmed off the saved file. *cancel is polled during the transfer: set it
// true to abort (the partial file is removed). Blocks; call from a worker task.
// Returns ESP_OK on success, ESP_ERR_INVALID_STATE if cancelled.
esp_err_t podcast_download_episode(const podcast_episode_t *ep, int skip_seconds, volatile bool *cancel);
