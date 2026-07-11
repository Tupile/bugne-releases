// source_stream: web radio and podcast streaming over HTTP and HTTPS.
//
// Resolves .m3u/.pls playlists to the real stream URL, follows redirects, and
// feeds the body to the decoder. Chunked transfer encoding is handled by the
// HTTP client. The same decoder as the SD source is used; only the byte source
// differs (an HTTP socket instead of a file).
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

// No heavy setup needed. Kept for boot-order symmetry.
esp_err_t source_stream_init(void);

// Copy the current web radio ICY "now playing" title (StreamTitle, often
// "Artist - Title") into buf. Empty if the station sends no ICY metadata or
// nothing is streaming. Returns the length.
size_t source_stream_title(char *buf, size_t size);

// Stream and play a URL (blocking until the stream ends, errors, or
// source_stream_stop() is called). Acquires the audio arbiter for the duration.
esp_err_t source_stream_play(const char *url);

// Ask the current stream to stop. The play call then returns.
void source_stream_stop(void);

// Whether the last source_stream_play() ended because the stream reached its end
// (a finite podcast episode), as opposed to being stopped or failing. Live radio
// never ends on its own, so this stays false for it. Used to auto-advance to the
// next episode.
bool source_stream_completed(void);

// Enable a throwaway decoy connection before the next source_stream_play() opens
// the real one. One-shot, consumed (and cleared) at the start of that call. Used
// for stations that inject a pre-roll ad into the first of several near-
// simultaneous connections from one IP (e.g. OUI FM): the decoy absorbs it so the
// real connection joins the live stream instantly.
void source_stream_set_preroll_decoy(bool enable);
