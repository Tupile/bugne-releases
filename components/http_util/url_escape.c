// url_escape: percent-encode the bytes esp_http_client cannot parse. Pure C (no
// ESP-IDF dependency) so the host tests link it directly.
#include "url_escape.h"

#include <stdlib.h>
#include <string.h>

bool url_escape_needed(unsigned char c)
{
    if (c <= 0x20 || c >= 0x7F) return true;  // controls, space, DEL, non-ASCII
    return strchr("\"<>\\^`{|}", c) != NULL;
}

size_t url_escape_len(const char *in)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        n += url_escape_needed(*p) ? 3 : 1;
    }
    return n;
}

bool url_escape(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!in || !out || cap == 0) return false;
    if (url_escape_len(in) + 1 > cap) {
        out[0] = '\0';
        return false;
    }
    char *o = out;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (url_escape_needed(*p)) {
            *o++ = '%';
            *o++ = hex[*p >> 4];
            *o++ = hex[*p & 0x0F];
        } else {
            *o++ = (char)*p;
        }
    }
    *o = '\0';
    return true;
}

char *url_escape_dup(const char *in)
{
    if (!in) return NULL;
    size_t cap = url_escape_len(in) + 1;
    char *out = malloc(cap);
    if (out && !url_escape(in, out, cap)) {
        free(out);
        out = NULL;
    }
    return out;
}
