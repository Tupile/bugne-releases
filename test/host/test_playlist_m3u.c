// Host unit tests for the playlist M3U module (playlist_m3u.c): name rules,
// path normalization, the line reader and the writer. Build and run with
// test/host/run.sh. No ESP-IDF needed.
#include "playlist_m3u.h"
#include "source_sd.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define CHECK_STR(got, want, what) \
    CHECK(strcmp((got), (want)) == 0, "%s: got \"%s\", want \"%s\"", (what), (got), (want))

static char s_titles[PLAYLIST_MAX_TRACKS][PLAYLIST_TITLE_MAX];
static char s_paths[PLAYLIST_MAX_TRACKS][PLAYLIST_PATH_MAX];

// Parse a text as if it were a .m3u file.
static size_t parse(const char *text, bool *truncated)
{
    FILE *f = tmpfile();
    fputs(text, f);
    rewind(f);
    size_t n = playlist_m3u_read(f, s_titles, s_paths, PLAYLIST_MAX_TRACKS, truncated);
    fclose(f);
    return n;
}

static void test_names(void)
{
    CHECK(playlist_name_valid("Rock"), "plain name");
    CHECK(playlist_name_valid("Chansons d'\xc3\xa9t\xc3\xa9 2026"), "accents, quote, digits");
    CHECK(playlist_name_valid("a.b"), "inner dot");
    CHECK(!playlist_name_valid(NULL), "NULL");
    CHECK(!playlist_name_valid(""), "empty");
    CHECK(!playlist_name_valid(".hidden"), "leading dot");
    CHECK(!playlist_name_valid("end."), "trailing dot");
    CHECK(!playlist_name_valid("end "), "trailing space");
    CHECK(!playlist_name_valid("a/b"), "slash");
    CHECK(!playlist_name_valid("a\\b"), "backslash");
    CHECK(!playlist_name_valid("a:b"), "colon");
    CHECK(!playlist_name_valid("a*b"), "star");
    CHECK(!playlist_name_valid("a?b"), "question mark");
    CHECK(!playlist_name_valid("a\"b"), "double quote");
    CHECK(!playlist_name_valid("a<b"), "less than");
    CHECK(!playlist_name_valid("a>b"), "greater than");
    CHECK(!playlist_name_valid("a|b"), "pipe");
    CHECK(!playlist_name_valid("a\tb"), "control character");
    CHECK(!playlist_name_valid("a\x7f" "b"), "DEL");
    // Malformed UTF-8: FatFs refuses it at open time, so refuse it here.
    CHECK(!playlist_name_valid("a\xff"), "invalid byte");
    CHECK(!playlist_name_valid("a\xc3"), "cut two-byte character");
    CHECK(!playlist_name_valid("\xa9" "a"), "stray continuation byte");
    CHECK(!playlist_name_valid("a\xc0\x80"), "overlong encoding");
    CHECK(playlist_name_valid("\xe2\x82\xac uro"), "three-byte character");
    CHECK(playlist_name_valid("\xf0\x9f\x8e\xb5"), "four-byte character");

    char n48[49], n49[50];
    memset(n48, 'a', 48); n48[48] = '\0';
    memset(n49, 'a', 49); n49[49] = '\0';
    CHECK(playlist_name_valid(n48), "48 bytes accepted");
    CHECK(!playlist_name_valid(n49), "49 bytes refused");
}

static void test_normalize(void)
{
    char out[PLAYLIST_PATH_MAX];
    CHECK(playlist_path_normalize("music/a.mp3", out, sizeof(out)), "plain path");
    CHECK_STR(out, "music/a.mp3", "plain path");
    CHECK(playlist_path_normalize("/sdcard/music/a.mp3", out, sizeof(out)), "/sdcard/ prefix");
    CHECK_STR(out, "music/a.mp3", "/sdcard/ prefix");
    CHECK(playlist_path_normalize("/music/a.FLAC", out, sizeof(out)), "leading slash");
    CHECK_STR(out, "music/a.FLAC", "leading slash");
    CHECK(playlist_path_normalize("music\\rock\\a.m4a", out, sizeof(out)), "backslashes");
    CHECK_STR(out, "music/rock/a.m4a", "backslashes");
    CHECK(playlist_path_normalize("\\music\\a.aac", out, sizeof(out)), "leading backslash");
    CHECK_STR(out, "music/a.aac", "leading backslash");
    CHECK(playlist_path_normalize("podcasts/X/sympa... tique.mp4", out, sizeof(out)), "ellipsis in a name");

    CHECK(!playlist_path_normalize("", out, sizeof(out)), "empty");
    CHECK(!playlist_path_normalize("/", out, sizeof(out)), "only a slash");
    CHECK(!playlist_path_normalize("../a.mp3", out, sizeof(out)), "leading ..");
    CHECK(!playlist_path_normalize("music/../../a.mp3", out, sizeof(out)), "inner ..");
    CHECK(!playlist_path_normalize("music\\..\\a.mp3", out, sizeof(out)), ".. with backslashes");
    CHECK(!playlist_path_normalize("/sdcard/../a.mp3", out, sizeof(out)), ".. after the prefix");
    CHECK(!playlist_path_normalize("http://example.com/a.mp3", out, sizeof(out)), "URL");
    CHECK(!playlist_path_normalize("music/a.txt", out, sizeof(out)), "non-playable extension");
    CHECK(!playlist_path_normalize("music/a", out, sizeof(out)), "no extension");
    CHECK(!playlist_path_normalize("music/a\tb.mp3", out, sizeof(out)), "control character");

    // Length is checked AFTER normalization: 255 bytes fit, 256 do not, and a
    // /sdcard/ prefix does not count.
    char longp[300];
    memset(longp, 'a', sizeof(longp));
    memcpy(longp + 251, ".mp3", 5);                    // 255 bytes
    CHECK(playlist_path_normalize(longp, out, sizeof(out)), "255 bytes accepted");
    memset(longp, 'a', sizeof(longp));
    memcpy(longp + 252, ".mp3", 5);                    // 256 bytes
    CHECK(!playlist_path_normalize(longp, out, sizeof(out)), "256 bytes refused");
    char pre[320];
    memset(longp, 'a', sizeof(longp));
    memcpy(longp + 251, ".mp3", 5);
    snprintf(pre, sizeof(pre), "/sdcard/%s", longp);
    CHECK(playlist_path_normalize(pre, out, sizeof(out)), "255 bytes after the prefix accepted");

    // Parity: whatever the SD guard refuses, the normalizer never lets through.
    static const char *const bad[] = {
        "..", "../x.mp3", "a/../b.mp3", "a\\..\\b.mp3", "a/.../b.mp3", "a/..", NULL,
    };
    for (int i = 0; bad[i]; i++) {
        CHECK(!source_sd_rel_path_safe(bad[i]), "guard refuses \"%s\"", bad[i]);
        CHECK(!playlist_path_normalize(bad[i], out, sizeof(out)), "normalizer refuses \"%s\"", bad[i]);
    }
    // And whatever the normalizer accepts passes the guard.
    CHECK(playlist_path_normalize("/sdcard/a/b.mp3", out, sizeof(out)) && source_sd_rel_path_safe(out),
          "normalized output passes the guard");
}

static void test_read(void)
{
    bool tr = true;
    size_t n = parse("#EXTM3U\n#EXTINF:-1,Song one\nmusic/one.mp3\n#EXTINF:123,Song two\nmusic/two.flac\n", &tr);
    CHECK(n == 2 && !tr, "basic file: %zu entries", n);
    CHECK_STR(s_titles[0], "Song one", "title 0");
    CHECK_STR(s_paths[0], "music/one.mp3", "path 0");
    CHECK_STR(s_titles[1], "Song two", "title 1");
    CHECK_STR(s_paths[1], "music/two.flac", "path 1");

    // PC-edited: BOM, CRLF, blank lines, unknown directives, absolute paths.
    n = parse("\xef\xbb\xbf#EXTM3U\r\n\r\n#EXTVLCOPT:x\r\n#EXTINF:-1,Un \xc3\xa9t\xc3\xa9\r\n"
              "\\music\\ete.mp3\r\n#EXTINF:-1,Deux\r\n/sdcard/music/deux.m4a\r\n", &tr);
    CHECK(n == 2 && !tr, "PC file: %zu entries", n);
    CHECK_STR(s_titles[0], "Un \xc3\xa9t\xc3\xa9", "PC title 0");
    CHECK_STR(s_paths[0], "music/ete.mp3", "PC path 0");
    CHECK_STR(s_paths[1], "music/deux.m4a", "PC path 1");

    // A BOM on a file with no header line.
    n = parse("\xef\xbb\xbfmusic/a.mp3\n", &tr);
    CHECK(n == 1, "BOM before a path: %zu", n);
    CHECK_STR(s_paths[0], "music/a.mp3", "BOM before a path");

    // No EXTINF: the title is the file name without its extension. No final
    // newline on the last line.
    n = parse("music/rock/My Song.mp3\nmusic/other.flac", &tr);
    CHECK(n == 2, "plain m3u: %zu entries", n);
    CHECK_STR(s_titles[0], "My Song", "derived title");
    CHECK_STR(s_titles[1], "other", "derived title, last line");

    // A refused entry is skipped, the rest loads, and its title is not
    // inherited by the next entry.
    n = parse("#EXTINF:-1,Bad\n../x.mp3\nmusic/good.mp3\n#EXTINF:-1,Url\nhttp://h/a.mp3\n"
              "#EXTINF:-1,Kept\nmusic/kept.mp3\n", &tr);
    CHECK(n == 2, "skips refused entries: %zu", n);
    CHECK_STR(s_titles[0], "good", "title not inherited from a refused entry");
    CHECK_STR(s_titles[1], "Kept", "entry after a refused one");

    // EXTINF without a comma, and an empty title: fall back to the file name.
    n = parse("#EXTINF:-1\nmusic/a.mp3\n#EXTINF:-1,\nmusic/b.mp3\n", &tr);
    CHECK(n == 2, "EXTINF without a title: %zu", n);
    CHECK_STR(s_titles[0], "a", "no comma");
    CHECK_STR(s_titles[1], "b", "empty title");

    // Long title: cut to 63 bytes on a UTF-8 boundary. 62 ASCII bytes then a
    // 2-byte character: the character does not fit and must go entirely.
    char text[400];
    char t62[63];
    memset(t62, 'x', 62); t62[62] = '\0';
    snprintf(text, sizeof(text), "#EXTINF:-1,%s\xc3\xa9tail\nmusic/a.mp3\n", t62);
    n = parse(text, &tr);
    CHECK(n == 1, "long title: %zu", n);
    CHECK_STR(s_titles[0], t62, "title cut on a character boundary");

    // An over-long line is dropped whole; the lines after it still load.
    char big[1200];
    memset(big, 'a', 1000);
    strcpy(big + 1000, ".mp3\nmusic/after.mp3\n");
    n = parse(big, &tr);
    CHECK(n == 1, "over-long line: %zu", n);
    CHECK_STR(s_paths[0], "music/after.mp3", "line after an over-long one");

    // Empty file and header-only file.
    CHECK(parse("", &tr) == 0 && !tr, "empty file");
    CHECK(parse("#EXTM3U\n", &tr) == 0 && !tr, "header only");
}

static void test_cap(void)
{
    static char text[PLAYLIST_MAX_TRACKS * 24 + 64];
    bool tr = true;
    size_t off = 0;
    for (int i = 0; i < PLAYLIST_MAX_TRACKS; i++) off += (size_t)sprintf(text + off, "music/t%03d.mp3\n", i);
    size_t n = parse(text, &tr);
    CHECK(n == PLAYLIST_MAX_TRACKS && !tr, "exactly the cap: %zu, truncated %d", n, tr);

    // A refused line past the cap is not a truncation; a valid one is.
    strcpy(text + off, "../bad.mp3\n");
    n = parse(text, &tr);
    CHECK(n == PLAYLIST_MAX_TRACKS && !tr, "bad line past the cap: truncated %d", tr);
    strcpy(text + off, "music/extra.mp3\n");
    n = parse(text, &tr);
    CHECK(n == PLAYLIST_MAX_TRACKS && tr, "one over the cap: %zu, truncated %d", n, tr);
    CHECK_STR(s_paths[PLAYLIST_MAX_TRACKS - 1], "music/t127.mp3", "last kept entry");
}

static void test_write(void)
{
    static char wt[3][PLAYLIST_TITLE_MAX] = { "Un \xc3\xa9t\xc3\xa9", "Line\nbreak\r", "" };
    static char wp[3][PLAYLIST_PATH_MAX] = { "music/ete.mp3", "music/b.flac", "music/No Title.m4a" };
    FILE *f = tmpfile();
    CHECK(playlist_m3u_write(f, wt, wp, 3), "write ok");
    rewind(f);
    char raw[512];
    size_t len = fread(raw, 1, sizeof(raw) - 1, f);
    raw[len] = '\0';
    CHECK(strncmp(raw, "#EXTM3U\n", 8) == 0, "header first");
    CHECK(strchr(raw, '\r') == NULL, "LF only, no CR copied from a title");
    CHECK(strstr(raw, "#EXTINF:-1,Line break \nmusic/b.flac\n") != NULL, "newline in a title becomes a space: %s", raw);

    // Round trip.
    rewind(f);
    bool tr = true;
    size_t n = playlist_m3u_read(f, s_titles, s_paths, PLAYLIST_MAX_TRACKS, &tr);
    fclose(f);
    CHECK(n == 3 && !tr, "round trip: %zu entries", n);
    CHECK_STR(s_titles[0], "Un \xc3\xa9t\xc3\xa9", "round trip title 0");
    CHECK_STR(s_paths[0], "music/ete.mp3", "round trip path 0");
    CHECK_STR(s_paths[1], "music/b.flac", "round trip path 1");
    CHECK_STR(s_titles[2], "No Title", "empty title falls back to the file name");

    // A path whose first byte would be read as a directive ('#') or stripped
    // (space) still round-trips: the writer protects it with a leading '/'.
    static char ht[2][PLAYLIST_TITLE_MAX] = { "Hits", "Intro" };
    static char hp[2][PLAYLIST_PATH_MAX] = { "#1 Hits/a.mp3", " intro.mp3" };
    f = tmpfile();
    CHECK(playlist_m3u_write(f, ht, hp, 2), "write # and space paths");
    rewind(f);
    n = playlist_m3u_read(f, s_titles, s_paths, PLAYLIST_MAX_TRACKS, &tr);
    fclose(f);
    CHECK(n == 2, "# and space paths read back: %zu", n);
    CHECK_STR(s_paths[0], "#1 Hits/a.mp3", "path starting with #");
    CHECK_STR(s_paths[1], " intro.mp3", "path starting with a space");

    // The writer refuses a path the reader would refuse: a file it wrote
    // always reads back whole.
    static char bt[1][PLAYLIST_TITLE_MAX] = { "x" };
    static char bp[1][PLAYLIST_PATH_MAX] = { "../x.mp3" };
    f = tmpfile();
    CHECK(!playlist_m3u_write(f, bt, bp, 1), "writer refuses an unsafe path");
    fclose(f);
}

int main(void)
{
    test_names();
    test_normalize();
    test_read();
    test_cap();
    test_write();
    if (g_fail) { printf("test_playlist_m3u: %d FAILED\n", g_fail); return 1; }
    printf("test_playlist_m3u: all passed\n");
    return 0;
}
