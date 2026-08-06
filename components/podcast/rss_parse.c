// rss_parse: streaming podcast RSS parser core (yxml + libc only).
#include "rss_parse.h"

#include <stdlib.h>
#include <string.h>

#define RSS_STACK_MAX  16
#define RSS_NAME_MAX   24

static void copy_bounded(char *dst, const char *src, size_t size)
{
    size_t i = 0;
    for (; i + 1 < size && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void set_target(rss_parser_t *r, char *buf, size_t max)
{
    r->target = buf;
    r->target_max = max;
    r->target_len = 0;
    if (buf && max) buf[0] = '\0';
}

// Set the element's content-capture destination and make it the active target.
// Attributes may redirect the active target and then restore it (see ATTREND).
static void set_content_target(rss_parser_t *r, char *buf, size_t max)
{
    r->content_target = buf;
    r->content_max = max;
    set_target(r, buf, max);
}

static void append_target(rss_parser_t *r, const char *s)
{
    if (!r->target) return;
    while (*s && r->target_len < r->target_max - 1) {
        r->target[r->target_len++] = *s++;
    }
    r->target[r->target_len] = '\0';
}

// One numeric field of a duration. Saturating: feed text is untrusted, so a
// 40-digit number must not wrap into a negative or nonsense value (atoi, used
// here before, is undefined on overflow). Returns -1 when there is no digit.
#define RSS_DURATION_MAX (24 * 3600)  // any real episode is far under a day

static int duration_field(const char **pp)
{
    const char *p = *pp;
    while (*p == ' ' || *p == '\t') p++;
    long v = 0;
    bool digit = false;
    while (*p >= '0' && *p <= '9') {
        digit = true;
        if (v <= RSS_DURATION_MAX) v = v * 10 + (*p - '0');  // saturate, never wrap
        p++;
    }
    *pp = p;
    if (!digit) return -1;
    return v > RSS_DURATION_MAX ? RSS_DURATION_MAX : (int)v;
}

int rss_parse_duration(const char *s)
{
    int parts[3] = {0, 0, 0};
    int n = 0;
    const char *p = s;
    while (*p && n < 3) {
        int v = duration_field(&p);
        parts[n++] = (v < 0) ? 0 : v;  // a malformed field counts as 0, as before
        if (*p != ':') break;          // "12:abc" stays 12 minutes, not 12 seconds
        p++;
    }
    long total = 0;
    if (n == 3)      total = (long)parts[0] * 3600 + (long)parts[1] * 60 + parts[2];
    else if (n == 2) total = (long)parts[0] * 60 + parts[1];
    else if (n == 1) total = parts[0];
    return total > RSS_DURATION_MAX ? RSS_DURATION_MAX : (int)total;
}

static void on_token(rss_parser_t *r, yxml_t *x, yxml_ret_t t)
{
    switch (t) {
    case YXML_ELEMSTART:
        if (r->depth < RSS_STACK_MAX) copy_bounded(r->stack[r->depth], x->elem, RSS_NAME_MAX);
        r->depth++;
        if (strcmp(x->elem, "item") == 0) {
            r->in_item = true;
            memset(&r->cur, 0, sizeof(r->cur));
        } else if (strcmp(x->elem, "enclosure") == 0) {
            r->cur_is_enclosure = true;
        } else if (strcmp(x->elem, "title") == 0) {
            if (r->in_item) set_content_target(r, r->cur.title, sizeof(r->cur.title));
            else if (!r->got_podcast_title) set_content_target(r, r->podcast_title, sizeof(r->podcast_title));
        } else if (r->in_item && strcmp(x->elem, "pubDate") == 0) {
            set_content_target(r, r->cur.date, sizeof(r->cur.date));
        } else if (r->in_item && strcmp(x->elem, "itunes:duration") == 0) {
            set_content_target(r, r->dur_buf, sizeof(r->dur_buf));
        } else if (!r->in_item && !r->got_image_url &&
                   strcmp(x->elem, "itunes:image") == 0) {
            // Channel artwork, carried by an href attribute (captured below).
            r->cur_is_itunes_image = true;
        } else if (!r->in_item && !r->got_image_url && strcmp(x->elem, "url") == 0 &&
                   r->depth >= 2 && strcmp(r->stack[r->depth - 2], "image") == 0) {
            // Plain RSS <image><url>: element content, and the only place a
            // parent check is needed (a bare "url" appears elsewhere too).
            set_content_target(r, r->image_url, sizeof(r->image_url));
        }
        break;
    case YXML_CONTENT:
        append_target(r, x->data);
        break;
    case YXML_ATTRSTART:
        // Capture the enclosure url attribute; for any other attribute, drop its
        // value (target = NULL) so it cannot bleed into the element content.
        if (r->cur_is_enclosure && strcmp(x->attr, "url") == 0) {
            set_target(r, r->cur.url, sizeof(r->cur.url));
        } else if (r->cur_is_itunes_image && strcmp(x->attr, "href") == 0) {
            set_target(r, r->image_url, sizeof(r->image_url));
        } else {
            r->target = NULL;
        }
        break;
    case YXML_ATTRVAL:
        append_target(r, x->data);
        break;
    case YXML_ATTREND:
        // Resume content capture (content always follows the start tag's
        // attributes), so a preceding attribute does not truncate the content.
        set_target(r, r->content_target, r->content_max);
        break;
    case YXML_ELEMEND: {
        // yxml reports the parent in x->elem here, so use our own stack to know
        // which element actually closed.
        const char *closed = "";
        if (r->depth > 0) {
            r->depth--;
            if (r->depth < RSS_STACK_MAX) closed = r->stack[r->depth];
        }
        if (strcmp(closed, "itunes:duration") == 0) {
            r->cur.duration_seconds = rss_parse_duration(r->dur_buf);
        } else if (strcmp(closed, "title") == 0 && !r->in_item) {
            r->got_podcast_title = r->podcast_title[0] != '\0';
        } else if (strcmp(closed, "enclosure") == 0) {
            r->cur_is_enclosure = false;
        } else if (strcmp(closed, "itunes:image") == 0) {
            r->cur_is_itunes_image = false;
            r->got_image_url = r->image_url[0] != '\0';  // first one wins
        } else if (strcmp(closed, "url") == 0 && !r->in_item) {
            r->got_image_url = r->image_url[0] != '\0';
        } else if (strcmp(closed, "item") == 0) {
            if (r->cur.url[0] && r->emitted < RSS_MAX_EPISODES) {
                if (r->on_episode) r->on_episode(&r->cur, r->cb_ctx);
                r->emitted++;
            }
            r->in_item = false;
        }
        r->content_target = NULL;
        r->target = NULL;
        break;
    }
    default:
        break;
    }
}

void rss_parse_init(rss_parser_t *p, void *yxml_buf, size_t yxml_buf_size,
                    rss_episode_cb cb, void *cb_ctx)
{
    memset(p, 0, sizeof(*p));
    p->on_episode = cb;
    p->cb_ctx = cb_ctx;
    yxml_init(&p->x, yxml_buf, yxml_buf_size);
}

bool rss_parse_feed(rss_parser_t *p, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (p->emitted >= RSS_MAX_EPISODES) {
            return true;  // safety cap reached
        }
        yxml_ret_t t = yxml_parse(&p->x, (unsigned char)data[i]);
        if (t < 0) {
            return false;  // XML syntax error
        }
        if (t > 0) {
            on_token(p, &p->x, t);
        }
    }
    return true;
}
