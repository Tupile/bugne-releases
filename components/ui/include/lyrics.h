// lyrics: song lyrics from LRCLIB (https://lrclib.net) and from sidecar files
// on the SD card. This header has two halves: the PURE helpers (lyrics.c, no
// ESP, no LVGL, host-tested in test/host/test_lyrics.c) and, further down, the
// fetch task (lyrics_fetch.c, ESP only).
//
// Every text here is untrusted (a web service, or a file copied from a PC):
// every copy is bounded and ends on a whole UTF-8 sequence.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LYRICS_TEXT_MAX    8192  // bytes kept per form (synced, plain), NUL included
#define LYRICS_LINES_MAX   400   // synced lines kept
#define LYRICS_URL_MAX     768   // two 96-byte fields fully percent-encoded fit
#define LYRICS_SYNC_TOL_MS 3000  // our duration vs the record's, for the synced view

typedef struct {
    uint32_t ms;       // when the line starts
    const char *text;  // points into the parsed buffer, "" for a pause
} lyrics_line_t;

// Percent-encode a query VALUE: everything but A-Z a-z 0-9 - _ . ~ becomes
// %XX. url_escape (http_util) is not enough here, it leaves & = + ? # alone.
// False when out is too small (out is then empty).
bool lyrics_qenc(const char *in, char *out, size_t cap);

// The LRCLIB lookup URL. dur_s is sent only when 1..3600 (the service's own
// range). False on an empty title or artist, or when out is too small.
bool lyrics_query_url(const char *title, const char *artist, uint32_t dur_s,
                      char *out, size_t cap);

// Split a radio "ARTIST - TITLE" on the first " - ". False when there is no
// separator or a side is empty (a talk program has no song).
bool lyrics_icy_split(const char *icy, char *artist, size_t asz, char *title, size_t tsz);

// Parse LRC text IN PLACE (the buffer is cut into strings the lines point
// to). Accepts [mm:ss], [mm:ss.x], [mm:ss.xx], [mm:ss.xxx], several stamps on
// one line, CRLF and a BOM. Lines without a valid stamp ([ar:...] tags
// included) are dropped. The result is sorted by time. Returns the count.
size_t lyrics_parse_lrc(char *text, lyrics_line_t *lines, size_t max);

// Index of the line being sung at pos_ms, -1 before the first line.
int lyrics_line_at(const lyrics_line_t *lines, size_t n, uint32_t pos_ms);

// True when a synced record can be trusted to follow OUR file: both durations
// known and within LYRICS_SYNC_TOL_MS. Another version of the song would run
// at the wrong time, so it gets the plain view instead.
bool lyrics_use_synced(uint32_t our_dur_ms, uint32_t rec_dur_ms);

// Identity of a song: FNV-1a 64 of title and artist.
uint64_t lyrics_key(const char *title, const char *artist);

// Bounded copy of lyric text: drops a BOM and every CR, ends on a whole
// UTF-8 sequence. Returns the length written.
size_t lyrics_copy_text(char *out, size_t cap, const char *in);

// What to do with an HTTP answer. status < 0 is a transport failure.
typedef enum {
    LYRICS_ACT_USE,                // parse the body
    LYRICS_ACT_RETRY_NO_DURATION,  // 404 with a duration: ask once more without
    LYRICS_ACT_NOT_FOUND,
    LYRICS_ACT_BACKOFF,            // 429: no request before lyrics_backoff_s
    LYRICS_ACT_ERROR,
} lyrics_action_t;
lyrics_action_t lyrics_next_action(int status, bool had_duration, bool body_over_cap);

// Seconds to stay silent after a 429, from its Retry-After header: 60 when
// missing or not a plain number of seconds, 600 at most.
uint32_t lyrics_backoff_s(const char *retry_after);

// Sidecar path of a track: same folder, same base name, extension replaced
// by ext (".lrc" or ".txt"). False when it does not fit in out.
bool lyrics_sidecar_path(const char *track, const char *ext, char *out, size_t cap);

// Which sidecar a network result is saved as. A synced text is saved as .lrc
// only when its version was trusted (lyrics_use_synced): a sidecar .lrc is
// later shown synced without any check. A record that HAS synced lines we
// could not trust is not saved at all: a .txt would be found first at every
// later listening and the synced version would never be asked for again.
typedef enum { LYRICS_SAVE_NONE, LYRICS_SAVE_LRC, LYRICS_SAVE_TXT } lyrics_save_t;
lyrics_save_t lyrics_save_form(bool has_synced, bool synced_trusted, bool has_plain);

// Cheap look at a JSON body before it goes to the recursive parser, which
// runs on a small task stack: it must be an object and must not nest deeper
// than max_depth (the real answer nests 1 level).
bool lyrics_json_shape_ok(const char *json, int max_depth);

// ---- Fetch task (lyrics_fetch.c, ESP only) ---------------------------------
//
// One background task looks a song up: first the sidecar files next to an SD
// track, then LRCLIB. The LVGL task asks with lyrics_request and polls
// lyrics_state; the result is handed over ONCE by lyrics_take, after which the
// caller is its only owner (same idea as art_take).

typedef enum {
    LYRICS_IDLE,          // nothing asked for this song
    LYRICS_LOADING,
    LYRICS_READY,         // lyrics_take has a result
    LYRICS_NOT_FOUND,
    LYRICS_INSTRUMENTAL,
    LYRICS_OFFLINE,       // no sidecar and no Wi-Fi
    LYRICS_ERROR,
} lyrics_state_t;

typedef struct {
    uint64_t key;          // lyrics_key of the song
    bool synced;           // lines[] is valid; otherwise text is the plain text
    size_t line_count;
    lyrics_line_t *lines;
    char *text;            // plain text, or the buffer lines[] point into
} lyrics_doc_t;

// Create the lock. Call once from app_main before ui_start.
void lyrics_init(void);

// Ask for a song. sd_path is the absolute path of the playing file when it
// is on the SD card (sidecars are read and written next to it), NULL
// otherwise. dur_ms is 0 when unknown. The newest request wins; a running
// lookup is not aborted, its result is dropped.
void lyrics_request(const char *title, const char *artist, uint32_t dur_ms, const char *sd_path);

// State of the lookup for this song. LYRICS_IDLE when the module is working
// on, or holds, another song.
lyrics_state_t lyrics_state(uint64_t key);

// Take the result for this song, or NULL. Free it with lyrics_doc_free.
lyrics_doc_t *lyrics_take(uint64_t key);

// Forget the current request: a running lookup's result is dropped.
void lyrics_cancel(void);

void lyrics_doc_free(lyrics_doc_t *doc);
