// notif: see notif.h. Pure, host-tested (test/host/test_notif.c).
#include "notif.h"

#include <stdio.h>
#include <string.h>

bool notif_url_ok(const char *url)
{
    if (!url) return false;
    if (strncmp(url, "http://", 7) == 0) return url[7] != '\0';
    if (strncmp(url, "https://", 8) == 0) return url[8] != '\0';
    return false;
}

bool notif_url_split(const char *url, char *host, size_t hsize, const char **rest)
{
    if (!notif_url_ok(url) || !host || hsize == 0 || !rest) return false;
    const char *h = strstr(url, "://") + 3;
    size_t len = strcspn(h, ":/?#");
    if (len == 0 || len >= hsize) return false;
    if (memchr(h, '@', strcspn(h, "/?#")) || h[0] == '[') return false;
    memcpy(host, h, len);
    host[len] = '\0';
    *rest = h + len;
    return true;
}

bool notif_valid(const notif_t *n)
{
    if (!n || !n->message[0]) return false;
    if (n->count < 0 || n->count > NOTIF_ACTIONS_MAX) return false;
    if (n->reply_url[0] && !notif_url_ok(n->reply_url)) return false;
    if (n->count > 0 && !n->reply_url[0]) return false;
    for (int i = 0; i < n->count; i++) {
        if (!n->act[i].id[0] || !n->act[i].label[0]) return false;
    }
    return true;
}

// Append `s` as the inside of a JSON string. False when it does not fit.
static bool put_escaped(char *out, size_t size, size_t *pos, const char *s)
{
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        char esc[8];
        if (c == '"' || c == '\\') snprintf(esc, sizeof(esc), "\\%c", c);
        else if (c < 0x20)         snprintf(esc, sizeof(esc), "\\u%04x", c);
        else                       snprintf(esc, sizeof(esc), "%c", c);
        size_t len = strlen(esc);
        if (*pos + len >= size) return false;
        memcpy(out + *pos, esc, len);
        *pos += len;
    }
    return true;
}

static bool put_raw(char *out, size_t size, size_t *pos, const char *s)
{
    size_t len = strlen(s);
    if (*pos + len >= size) return false;
    memcpy(out + *pos, s, len);
    *pos += len;
    return true;
}

int notif_reply_json(char *out, size_t size, const char *id, const char *action)
{
    if (!out || size == 0) return -1;
    size_t pos = 0;
    bool ok = put_raw(out, size, &pos, "{\"id\":\"")
           && put_escaped(out, size, &pos, id)
           && put_raw(out, size, &pos, "\",\"action\":\"")
           && put_escaped(out, size, &pos, action)
           && put_raw(out, size, &pos, "\"}");
    if (!ok) {
        out[0] = '\0';
        return -1;
    }
    out[pos] = '\0';
    return (int)pos;
}
