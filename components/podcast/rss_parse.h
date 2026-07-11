// rss_parse: streaming podcast RSS parser core, built on yxml.
//
// Dependency-free (only yxml and libc) so it can be unit-tested on a host.
// podcast.c feeds it HTTP bytes; the result is a bounded list of episodes. All
// text is length-bounded here; callers still escape on output.
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "yxml.h"

#define RSS_TITLE_MAX     128
#define RSS_DATE_MAX      40
#define RSS_URL_MAX       512   // podcast episode URLs (e.g. Radio France) can be long
// Safety cap, not an architectural limit: episodes are streamed out one at a time
// (never buffered in an array), so this only bounds flash use and UI size against a
// pathological feed. 300 covers years of any real podcast; raising it is cheap.
#define RSS_MAX_EPISODES  300
#define RSS_YXML_BUF_SIZE 4096

typedef struct {
    char title[RSS_TITLE_MAX];
    char date[RSS_DATE_MAX];      // raw RSS pubDate
    int  duration_seconds;
    char url[RSS_URL_MAX];        // enclosure URL
} rss_episode_t;

// Called once per completed <item> (with an enclosure), in feed order. The pointer
// is only valid for the duration of the call: copy what you need.
typedef void (*rss_episode_cb)(const rss_episode_t *ep, void *ctx);

typedef struct {
    char   podcast_title[RSS_TITLE_MAX];
    rss_episode_cb on_episode;    // emitted per item; episodes are not stored here
    void  *cb_ctx;
    size_t emitted;               // count handed to the callback so far

    // internal state
    bool   got_podcast_title;
    bool   in_item;
    bool   cur_is_enclosure;
    rss_episode_t cur;            // the single episode being assembled
    char   dur_buf[16];
    char  *target;
    size_t target_max;
    size_t target_len;
    // Element-name stack: yxml reports the parent (not the closing element) at
    // YXML_ELEMEND, so we track names ourselves to know what just closed.
    char   stack[16][24];
    int    depth;
    yxml_t x;
} rss_parser_t;

// Initialize the parser. yxml_buf must be at least RSS_YXML_BUF_SIZE bytes and
// outlive the parser. `cb` is invoked once per parsed episode (may be NULL).
void rss_parse_init(rss_parser_t *p, void *yxml_buf, size_t yxml_buf_size,
                    rss_episode_cb cb, void *cb_ctx);

// Feed a chunk of XML bytes. Returns false on an XML syntax error (parsing
// should stop). Episodes beyond RSS_MAX_EPISODES are ignored.
bool rss_parse_feed(rss_parser_t *p, const char *data, size_t len);

// Parse an RSS duration ("3600", "62:03", or "1:02:03") to seconds.
int rss_parse_duration(const char *s);
