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

int rss_parse_duration(const char *s)
{
    int parts[3] = {0, 0, 0};
    int n = 0;
    const char *p = s;
    while (*p && n < 3) {
        parts[n++] = atoi(p);
        const char *colon = strchr(p, ':');
        if (!colon) break;
        p = colon + 1;
    }
    if (n == 3) return parts[0] * 3600 + parts[1] * 60 + parts[2];
    if (n == 2) return parts[0] * 60 + parts[1];
    return parts[0];
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
