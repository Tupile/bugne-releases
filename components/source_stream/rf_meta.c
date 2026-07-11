// rf_meta: pure helpers for Radio France livemeta now-playing metadata.
// See rf_meta.h. No ESP-IDF dependencies, host-tested in test/host.
#include "rf_meta.h"

#include <stdio.h>
#include <string.h>

// Slug -> livemeta station id. Seed entries hand-verified 2026-07-05;
// regenerate with tools/rf_livemeta_map.py.
// BEGIN RF_STATIONS (generated)
static const struct { const char *slug; uint16_t id; } RF_STATIONS[] = {
    {"franceinter", 1},
    {"franceinfo", 2},
    {"francemusique", 4},
    {"franceculture", 5},
    {"mouv", 6},
    {"fip", 7},
    {"fiprock", 64},
    {"fipjazz", 65},
    {"fipgroove", 66},
    {"fipworld", 69},
    {"fipnouveautes", 70},
    {"fipreggae", 71},
    {"fipelectro", 74},
    {"fipmetal", 77},
    {"fippop", 78},
    {"fiphiphop", 95},
    {"fipsacrefrancais", 96},
    {"francemusiqueeasyclassique", 401},
    {"francemusiqueclassiqueplus", 402},
    {"francemusiqueconcertsradiofrance", 403},
    {"francemusiqueocoramonde", 404},
    {"francemusiquelajazz", 405},
    {"francemusiquelacontemporaine", 406},
    {"francemusiquelabo", 407},
    {"francemusiquebaroque", 408},
    {"francemusiqueopera", 409},
    {"fipcultes", 709},
    {"franceinterlamusiqueinter", 1101},
    {"monpetitfranceinter", 1102},
    {"montoutpetitfranceinter", 1103},
};
// END RF_STATIONS (generated)

// ---- small ASCII helpers (locale-independent, no ctype.h) ----

static bool starts_with_ci(const char *s, const char *prefix)
{
    for (size_t i = 0; prefix[i]; i++) {
        char cs = s[i], cp = prefix[i];
        if (cs == '\0') return false;
        if (cs >= 'A' && cs <= 'Z') cs += 32;
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        if (cs != cp) return false;
    }
    return true;
}

static bool ci_eq_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
    }
    return true;
}

static bool host_is_radiofrance(const char *host, size_t len)
{
    static const char exact[] = "radiofrance.fr";
    static const char suffix[] = ".radiofrance.fr";
    size_t exact_len = sizeof(exact) - 1;
    size_t suf_len = sizeof(suffix) - 1;
    if (len == exact_len && ci_eq_n(host, exact, exact_len)) return true;
    if (len > suf_len && ci_eq_n(host + (len - suf_len), suffix, suf_len)) return true;
    return false;
}

int rf_station_id(const char *url)
{
    if (!url) return 0;
    const char *p = url;
    if (starts_with_ci(p, "https://")) p += 8;
    else if (starts_with_ci(p, "http://")) p += 7;

    const char *host_start = p;
    while (*p && *p != '/' && *p != ':') p++;
    const char *host_end = p;
    if (*p == ':') {
        while (*p && *p != '/') p++;
    }
    if (*p != '/') return 0; // no path

    size_t host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || !host_is_radiofrance(host_start, host_len)) return 0;

    const char *path = p;
    const char *seg_end = strchr(path, '?');
    if (!seg_end) seg_end = path + strlen(path);

    const char *last_slash = path;
    for (const char *r = path; r < seg_end; r++) {
        if (*r == '/') last_slash = r;
    }
    const char *slug_start = last_slash + 1;
    const char *slug_end = seg_end;
    for (const char *r = slug_start; r < slug_end; r++) {
        if (*r == '-' || *r == '.') { slug_end = r; break; }
    }

    size_t slug_len = (size_t)(slug_end - slug_start);
    if (slug_len == 0 || slug_len >= 64) return 0;

    for (size_t i = 0; i < sizeof(RF_STATIONS) / sizeof(RF_STATIONS[0]); i++) {
        if (strlen(RF_STATIONS[i].slug) == slug_len &&
            ci_eq_n(slug_start, RF_STATIONS[i].slug, slug_len)) {
            return RF_STATIONS[i].id;
        }
    }
    return 0;
}

// ---- tiny bounded JSON scanner (no recursion, no malloc) ----

static const char *skip_ws_bounded(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

// p must point at '{' or '['. Returns a pointer just past the matching
// close, tracking nesting depth and in-string state (strings can contain
// braces/brackets). Returns NULL on truncated or malformed input.
static const char *skip_json_container(const char *p)
{
    if (*p != '{' && *p != '[') return NULL;
    int depth = 1;
    p++;
    while (*p && depth > 0) {
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) p += 2; else p++;
            }
            if (*p != '"') return NULL;
            p++;
            continue;
        }
        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') depth--;
        p++;
    }
    return depth == 0 ? p : NULL;
}

// Finds the first `"key"` literal in [start, end) followed by ':', and
// returns a pointer to the value start (after skipping whitespace). JSON
// requires an unescaped quote to close a string, so this simple substring
// scan cannot be fooled by string content: it always lands on a real key.
static const char *find_key_bounded(const char *start, const char *end, const char *key)
{
    char token[80];
    int n = snprintf(token, sizeof token, "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof token) return NULL;
    size_t tlen = (size_t)n;
    if (end < start || (size_t)(end - start) < tlen) return NULL;
    for (const char *p = start; p + tlen <= end; p++) {
        if (memcmp(p, token, tlen) == 0) {
            const char *v = skip_ws_bounded(p + tlen, end);
            if (v < end && *v == ':') return skip_ws_bounded(v + 1, end);
        }
    }
    return NULL;
}

static void append_byte(char *out, size_t out_size, size_t *oi, char c)
{
    if (*oi < out_size) out[*oi] = c;
    (*oi)++;
}

static void append_utf8(char *out, size_t out_size, size_t *oi, unsigned int cp)
{
    unsigned char b[4];
    int n;
    if (cp < 0x80) { b[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800) {
        b[0] = (unsigned char)(0xC0 | (cp >> 6));
        b[1] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (unsigned char)(0xE0 | (cp >> 12));
        b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        b[0] = (unsigned char)(0xF0 | (cp >> 18));
        b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    for (int i = 0; i < n; i++) append_byte(out, out_size, oi, (char)b[i]);
}

static bool hex4(const char *p, const char *end, unsigned int *val)
{
    if (p + 4 > end) return false;
    unsigned int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned int)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned int)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned int)(c - 'A' + 10);
        else return false;
    }
    *val = v;
    return true;
}

// Decodes a JSON string value starting at the opening quote, unescaping
// standard escapes and \uXXXX (including surrogate pairs). Truncates to
// out_size on overflow (byte truncation, always NUL-terminated). Returns
// false when the value is not a string or is unterminated within [_, end).
static bool decode_json_string(const char *p, const char *end, char *out, size_t out_size)
{
    if (out_size == 0) return false;
    if (p >= end || *p != '"') return false;
    p++;
    size_t oi = 0;
    while (p < end && *p != '"') {
        if (*p != '\\') {
            append_byte(out, out_size, &oi, *p);
            p++;
            continue;
        }
        p++;
        if (p >= end) return false;
        switch (*p) {
        case '"': append_byte(out, out_size, &oi, '"'); p++; break;
        case '\\': append_byte(out, out_size, &oi, '\\'); p++; break;
        case '/': append_byte(out, out_size, &oi, '/'); p++; break;
        case 'b': case 'f': case 'n': case 'r': case 't':
            append_byte(out, out_size, &oi, ' '); p++; break;
        case 'u': {
            unsigned int cp1;
            if (!hex4(p + 1, end, &cp1)) return false;
            p += 5; // 'u' + 4 hex digits
            if (cp1 >= 0xD800 && cp1 <= 0xDBFF) {
                unsigned int cp2;
                if (p + 6 <= end && p[0] == '\\' && p[1] == 'u' &&
                    hex4(p + 2, end, &cp2) && cp2 >= 0xDC00 && cp2 <= 0xDFFF) {
                    unsigned int cp = 0x10000 + ((cp1 - 0xD800) << 10) + (cp2 - 0xDC00);
                    append_utf8(out, out_size, &oi, cp);
                    p += 6;
                } // else: lone high surrogate, skip (emit nothing)
            } else if (cp1 >= 0xDC00 && cp1 <= 0xDFFF) {
                // lone low surrogate, skip
            } else {
                append_utf8(out, out_size, &oi, cp1);
            }
            break;
        }
        default:
            return false; // invalid escape
        }
    }
    if (p >= end) return false; // unterminated
    out[oi < out_size ? oi : out_size - 1] = '\0';
    return true;
}

static bool get_string_field(const char *obj, const char *obj_end, const char *key,
                              char *out, size_t out_size)
{
    const char *v = find_key_bounded(obj, obj_end, key);
    if (!v) return false;
    return decode_json_string(v, obj_end, out, out_size);
}

static bool get_int64_field(const char *obj, const char *obj_end, const char *key, int64_t *out)
{
    const char *v = find_key_bounded(obj, obj_end, key);
    if (!v || v >= obj_end) return false;
    bool neg = false;
    if (*v == '-') { neg = true; v++; }
    if (v >= obj_end || *v < '0' || *v > '9') return false;
    int64_t val = 0;
    while (v < obj_end && *v >= '0' && *v <= '9') { val = val * 10 + (*v - '0'); v++; }
    *out = neg ? -val : val;
    return true;
}

bool rf_meta_parse(const char *json, char *out, size_t out_size, int64_t *end_epoch)
{
    if (end_epoch) *end_epoch = 0;
    if (!json || !out || out_size == 0) return false;
    out[0] = '\0';

    const char *buf_end = json + strlen(json);

    const char *now = find_key_bounded(json, buf_end, "now");
    if (!now || now >= buf_end || *now != '{') return false;

    const char *now_end = skip_json_container(now);
    if (!now_end) return false;

    char first_line[256];
    if (!get_string_field(now, now_end, "firstLine", first_line, sizeof first_line)) return false;
    if (first_line[0] == '\0') return false;

    char second_line[256];
    bool have_second = get_string_field(now, now_end, "secondLine", second_line, sizeof second_line)
                        && second_line[0] != '\0';

    // "firstLineSongUuid"/"secondLineSongUuid" also exist in this object, but
    // the token below (quotes included) only matches the exact "songUuid" key.
    char song_uuid[40];
    bool is_song = get_string_field(now, now_end, "songUuid", song_uuid, sizeof song_uuid)
                   && song_uuid[0] != '\0';

    if (is_song && have_second) snprintf(out, out_size, "%s - %s", second_line, first_line);
    else snprintf(out, out_size, "%s", first_line);

    int64_t end_val = 0;
    get_int64_field(now, now_end, "endTime", &end_val);
    if (end_epoch) *end_epoch = end_val;

    return true;
}
