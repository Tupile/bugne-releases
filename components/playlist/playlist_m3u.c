// Playlist M3U reader and writer. See playlist_m3u.h.
#include "playlist_m3u.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "tags.h"

// Longest line kept. A longer one (a path over the limit, or an EXTINF with a
// huge title) is dropped whole.
#define LINE_MAX_BYTES 640

static bool is_ctrl(unsigned char c)
{
    return c < 0x20 || c == 0x7f;
}

// Well-formed UTF-8 only (no stray or missing continuation byte, no overlong
// form). FatFs refuses a malformed name when the file is opened; refusing it
// here turns that into a clean "bad name".
static bool utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int extra;
        if (*p < 0x80) extra = 0;
        else if (*p >= 0xC2 && *p <= 0xDF) extra = 1;
        else if (*p >= 0xE0 && *p <= 0xEF) extra = 2;
        else if (*p >= 0xF0 && *p <= 0xF4) extra = 3;
        else return false;  // continuation byte first, 0xC0/0xC1 overlong, > U+10FFFF
        unsigned char lead = *p++;
        for (int i = 0; i < extra; i++, p++) {
            if ((*p & 0xC0) != 0x80) return false;
            if (i == 0 && ((lead == 0xE0 && *p < 0xA0) || (lead == 0xF0 && *p < 0x90))) {
                return false;  // overlong three- and four-byte forms
            }
        }
    }
    return true;
}

bool playlist_name_valid(const char *name)
{
    if (!name) return false;
    size_t len = strlen(name);
    if (len == 0 || len >= PLAYLIST_NAME_MAX) return false;
    if (name[0] == '.' || name[len - 1] == '.' || name[len - 1] == ' ') return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (is_ctrl(c) || strchr("/\\:*?\"<>|", c)) return false;
    }
    return utf8_valid(name);
}

// Same list as sd_playable() in ui.c and format_from_path() in source_sd.c.
static bool ext_playable(const char *path)
{
    static const char *const exts[] = { "mp3", "flac", "m4a", "aac", "mp4" };
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(slash ? slash : path, '.');
    if (!dot) return false;
    for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
        const char *a = dot + 1, *b = exts[e];
        while (*a && *b && tolower((unsigned char)*a) == *b) { a++; b++; }
        if (!*a && !*b) return true;
    }
    return false;
}

bool playlist_path_normalize(const char *in, char *out, size_t size)
{
    if (!in || !out || size == 0) return false;
    if (strstr(in, "://")) return false;
    for (const char *q = in; *q; q++) {
        if (is_ctrl((unsigned char)*q)) return false;
    }
    // Drop the absolute prefix first: the length limit applies to what is left.
    const char *p = in;
    if ((p[0] == '/' || p[0] == '\\') && strncmp(p + 1, "sdcard", 6) == 0 &&
        (p[7] == '/' || p[7] == '\\')) {
        p += 8;
    }
    while (*p == '/' || *p == '\\') p++;
    size_t len = strlen(p);
    if (len == 0 || len >= size) return false;
    // Dots-only segments (".."), the rule of source_sd_rel_path_safe.
    size_t dots = 0, seg = 0;
    for (size_t i = 0; i <= len; i++) {
        char c = p[i];
        if (c == '/' || c == '\\' || c == '\0') {
            if (seg >= 2 && dots == seg) return false;
            dots = seg = 0;
        } else {
            seg++;
            if (c == '.') dots++;
        }
    }
    for (size_t i = 0; i <= len; i++) out[i] = (p[i] == '\\') ? '/' : p[i];
    return ext_playable(out);
}

// Bounded copy that never leaves half a UTF-8 character.
static void copy_title(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i + 1 < PLAYLIST_TITLE_MAX && i < n; i++) dst[i] = src[i];
    dst[i] = '\0';
    tags_utf8_trim_partial(dst);
}

// Title of last resort: the file name without its extension.
static void title_from_path(char *dst, const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    copy_title(dst, base, dot ? (size_t)(dot - base) : strlen(base));
}

size_t playlist_m3u_read(FILE *f, char (*titles)[PLAYLIST_TITLE_MAX],
                         char (*paths)[PLAYLIST_PATH_MAX], size_t max, bool *truncated)
{
    char pending[PLAYLIST_TITLE_MAX] = "";  // title of the EXTINF line just seen
    char path[PLAYLIST_PATH_MAX];
    size_t count = 0;
    bool first = true;
    if (truncated) *truncated = false;
    if (!f) return 0;
    // Off the stack: the device reads playlists on the LVGL task, inside an
    // event callback.
    char *line = malloc(LINE_MAX_BYTES);
    if (!line) return 0;

    while (fgets(line, LINE_MAX_BYTES, f)) {
        size_t len = strlen(line);
        if (len + 1 == LINE_MAX_BYTES && line[len - 1] != '\n') {
            // Over-long line: swallow the rest of it and drop it whole.
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {}
            pending[0] = '\0';
            first = false;
            continue;
        }
        char *s = line;
        if (first && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
            (unsigned char)s[2] == 0xBF) {
            s += 3;
            len -= 3;
        }
        first = false;
        while (len && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ' ||
                       s[len - 1] == '\t')) {
            s[--len] = '\0';
        }
        while (*s == ' ' || *s == '\t') { s++; len--; }
        if (len == 0) continue;

        if (s[0] == '#') {
            if (strncmp(s, "#EXTINF:", 8) == 0) {
                const char *comma = strchr(s, ',');
                if (comma) copy_title(pending, comma + 1, strlen(comma + 1));
                else pending[0] = '\0';
            }
            continue;  // header and unknown directives
        }

        bool ok = playlist_path_normalize(s, path, sizeof(path));
        if (ok) {
            if (count < max) {
                memcpy(paths[count], path, strlen(path) + 1);
                if (pending[0]) memcpy(titles[count], pending, sizeof(pending));
                else title_from_path(titles[count], path);
                count++;
            } else if (truncated) {
                *truncated = true;
            }
        }
        pending[0] = '\0';  // a title belongs to the very next entry only
    }
    free(line);
    return count;
}

bool playlist_m3u_write(FILE *f, char (*titles)[PLAYLIST_TITLE_MAX],
                        char (*paths)[PLAYLIST_PATH_MAX], size_t count)
{
    if (!f) return false;
    fputs("#EXTM3U\n", f);
    for (size_t i = 0; i < count; i++) {
        char path[PLAYLIST_PATH_MAX];
        if (!playlist_path_normalize(paths[i], path, sizeof(path))) return false;
        if (titles[i][0]) {
            char title[PLAYLIST_TITLE_MAX];
            copy_title(title, titles[i], strlen(titles[i]));
            // A line break inside a title would split the entry.
            for (char *p = title; *p; p++) if (is_ctrl((unsigned char)*p)) *p = ' ';
            fprintf(f, "#EXTINF:-1,%s\n", title);
        }
        // A first byte the reader would take for a directive ('#') or strip
        // (space) is protected by a leading '/', which the reader drops.
        fprintf(f, "%s%s\n", (path[0] == '#' || path[0] == ' ') ? "/" : "", path);
    }
    return !ferror(f);
}
