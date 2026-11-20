// lyrics: pure helpers for the lyrics view. See lyrics.h.
#include "lyrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tags.h"  // tags_utf8_trim_partial

#define LRCLIB_GET "https://lrclib.net/api/get"
#define BACKOFF_DEFAULT_S 60
#define BACKOFF_MAX_S     600
#define DURATION_MAX_S    3600  // LRCLIB refuses a duration outside 1..3600
#define STAMP_MIN_MAX     599   // a stamp past 10 hours is garbage, and would wrap in ms

static bool is_unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

bool lyrics_qenc(const char *in, char *out, size_t cap)
{
    static const char HEX[] = "0123456789ABCDEF";
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!in) return true;
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        size_t need = is_unreserved(*p) ? 1 : 3;
        if (o + need >= cap) { out[0] = '\0'; return false; }
        if (need == 1) {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = HEX[*p >> 4];
            out[o++] = HEX[*p & 0x0F];
        }
    }
    out[o] = '\0';
    return true;
}

bool lyrics_query_url(const char *title, const char *artist, uint32_t dur_s,
                      char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!title || !title[0] || !artist || !artist[0]) return false;

    int n = snprintf(out, cap, LRCLIB_GET "?track_name=");
    if (n < 0 || (size_t)n >= cap) goto too_small;
    size_t o = (size_t)n;
    if (!lyrics_qenc(title, out + o, cap - o)) goto too_small;
    o += strlen(out + o);
    n = snprintf(out + o, cap - o, "&artist_name=");
    if (n < 0 || (size_t)n >= cap - o) goto too_small;
    o += (size_t)n;
    if (!lyrics_qenc(artist, out + o, cap - o)) goto too_small;
    o += strlen(out + o);
    if (dur_s >= 1 && dur_s <= DURATION_MAX_S) {
        n = snprintf(out + o, cap - o, "&duration=%u", (unsigned)dur_s);
        if (n < 0 || (size_t)n >= cap - o) goto too_small;
    }
    return true;

too_small:
    out[0] = '\0';
    return false;
}

// Copy [s, s+len) into out, without surrounding spaces, bounded, UTF-8 clean.
static void copy_trimmed(char *out, size_t size, const char *s, size_t len)
{
    while (len > 0 && *s == ' ') { s++; len--; }
    while (len > 0 && s[len - 1] == ' ') len--;
    if (len >= size) len = size - 1;
    memcpy(out, s, len);
    out[len] = '\0';
    tags_utf8_trim_partial(out);
}

bool lyrics_icy_split(const char *icy, char *artist, size_t asz, char *title, size_t tsz)
{
    if (!icy || !artist || !title || asz == 0 || tsz == 0) return false;
    const char *sep = strstr(icy, " - ");
    if (!sep) return false;
    copy_trimmed(artist, asz, icy, (size_t)(sep - icy));
    copy_trimmed(title, tsz, sep + 3, strlen(sep + 3));
    return artist[0] != '\0' && title[0] != '\0';
}

// Read digits at *p, at least one. Advances *p. False when there is none.
static bool read_number(const char **p, uint32_t *value, int *digits)
{
    const char *s = *p;
    uint32_t v = 0;
    int n = 0;
    while (*s >= '0' && *s <= '9' && n < 6) {
        v = v * 10 + (uint32_t)(*s - '0');
        s++;
        n++;
    }
    if (n == 0) return false;
    *p = s;
    *value = v;
    if (digits) *digits = n;
    return true;
}

// One "[mm:ss]" or "[mm:ss.fff]" stamp at *p. Advances *p past the "]".
static bool read_stamp(const char **p, uint32_t *ms)
{
    const char *s = *p;
    if (*s != '[') return false;
    s++;
    uint32_t min = 0, sec = 0, frac = 0;
    int fd = 0;
    if (!read_number(&s, &min, NULL) || *s != ':' || min > STAMP_MIN_MAX) return false;
    s++;
    if (!read_number(&s, &sec, NULL) || sec > 59) return false;
    if (*s == '.' || *s == ':') {
        s++;
        if (!read_number(&s, &frac, &fd)) return false;
        while (fd < 3) { frac *= 10; fd++; }   // .5 is 500 ms, .50 is 500 ms
        while (fd > 3) { frac /= 10; fd--; }
    }
    if (*s != ']') return false;
    *p = s + 1;
    *ms = min * 60000u + sec * 1000u + frac;
    return true;
}

size_t lyrics_parse_lrc(char *text, lyrics_line_t *lines, size_t max)
{
    if (!text || !lines || max == 0) return 0;
    size_t n = 0;
    char *line = text;
    if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
        (unsigned char)line[2] == 0xBF) {
        line += 3;
    }
    while (*line && n < max) {
        char *end = strchr(line, '\n');
        char *next = end ? end + 1 : line + strlen(line);
        if (end) *end = '\0';
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\r') line[len - 1] = '\0';

        // All the stamps of the line share the text that follows them.
        const char *p = line;
        size_t first = n;
        uint32_t ms;
        while (n < max && read_stamp(&p, &ms)) {
            lines[n].ms = ms;
            n++;
        }
        if (n > first) {
            while (*p == ' ') p++;
            for (size_t i = first; i < n; i++) lines[i].text = p;
        }
        line = next;
    }

    // Insertion sort: the input is nearly always sorted already, and it is
    // stable (two lines on one stamp keep their order).
    for (size_t i = 1; i < n; i++) {
        lyrics_line_t cur = lines[i];
        size_t j = i;
        while (j > 0 && lines[j - 1].ms > cur.ms) {
            lines[j] = lines[j - 1];
            j--;
        }
        lines[j] = cur;
    }
    return n;
}

int lyrics_line_at(const lyrics_line_t *lines, size_t n, uint32_t pos_ms)
{
    if (!lines || n == 0 || pos_ms < lines[0].ms) return -1;
    size_t lo = 0, hi = n - 1;   // invariant: lines[lo].ms <= pos_ms
    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1) / 2;
        if (lines[mid].ms <= pos_ms) lo = mid;
        else hi = mid - 1;
    }
    return (int)lo;
}

bool lyrics_use_synced(uint32_t our_dur_ms, uint32_t rec_dur_ms)
{
    if (our_dur_ms == 0 || rec_dur_ms == 0) return false;
    uint32_t diff = our_dur_ms > rec_dur_ms ? our_dur_ms - rec_dur_ms : rec_dur_ms - our_dur_ms;
    return diff <= LYRICS_SYNC_TOL_MS;
}

static uint64_t fnv1a(uint64_t h, const char *s)
{
    for (; s && *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t lyrics_key(const char *title, const char *artist)
{
    uint64_t h = fnv1a(14695981039346656037ULL, title);
    h ^= 0x1F;                 // field separator: ("ab","c") is not ("a","bc")
    h *= 1099511628211ULL;
    return fnv1a(h, artist);
}

size_t lyrics_copy_text(char *out, size_t cap, const char *in)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!in) return 0;
    const unsigned char *p = (const unsigned char *)in;
    if (p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) p += 3;
    size_t o = 0;
    for (; *p && o + 1 < cap; p++) {
        if (*p == '\r') continue;
        out[o++] = (char)*p;
    }
    out[o] = '\0';
    tags_utf8_trim_partial(out);
    return strlen(out);
}

lyrics_action_t lyrics_next_action(int status, bool had_duration, bool body_over_cap)
{
    if (status == 200) return body_over_cap ? LYRICS_ACT_ERROR : LYRICS_ACT_USE;
    if (status == 404) return had_duration ? LYRICS_ACT_RETRY_NO_DURATION : LYRICS_ACT_NOT_FOUND;
    if (status == 429) return LYRICS_ACT_BACKOFF;
    return LYRICS_ACT_ERROR;
}

uint32_t lyrics_backoff_s(const char *retry_after)
{
    // strtoul accepts a sign and leading spaces: only a bare number counts.
    if (!retry_after || retry_after[0] < '0' || retry_after[0] > '9') return BACKOFF_DEFAULT_S;
    char *end = NULL;
    unsigned long v = strtoul(retry_after, &end, 10);
    // Anything after the number (the HTTP-date form) is not a second count.
    if (end == retry_after || *end != '\0' || v == 0) return BACKOFF_DEFAULT_S;
    return v > BACKOFF_MAX_S ? BACKOFF_MAX_S : (uint32_t)v;
}

bool lyrics_sidecar_path(const char *track, const char *ext, char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!track || !track[0] || !ext) return false;
    // The extension is the last dot of the last path component only.
    const char *slash = strrchr(track, '/');
    const char *dot = strrchr(slash ? slash : track, '.');
    size_t stem = dot ? (size_t)(dot - track) : strlen(track);
    int n = snprintf(out, cap, "%.*s%s", (int)stem, track, ext);
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return false; }
    return true;
}

lyrics_save_t lyrics_save_form(bool has_synced, bool synced_trusted, bool has_plain)
{
    if (has_synced) return synced_trusted ? LYRICS_SAVE_LRC : LYRICS_SAVE_NONE;
    return has_plain ? LYRICS_SAVE_TXT : LYRICS_SAVE_NONE;
}

bool lyrics_json_shape_ok(const char *json, int max_depth)
{
    if (!json) return false;
    const char *p = json;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '{') return false;
    int depth = 0;
    bool in_string = false;
    for (; *p; p++) {
        if (in_string) {
            if (*p == '\\' && p[1]) p++;       // skip the escaped character
            else if (*p == '"') in_string = false;
        } else if (*p == '"') {
            in_string = true;
        } else if (*p == '{' || *p == '[') {
            if (++depth > max_depth) return false;
        } else if (*p == '}' || *p == ']') {
            depth--;
        }
    }
    return true;
}
