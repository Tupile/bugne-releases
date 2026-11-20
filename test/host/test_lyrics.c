// Host unit tests for the pure lyrics module (lyrics.c).
// Build and run with test/host/run.sh. No ESP-IDF needed.
// Every lyric line here is INVENTED test text: never paste real song lyrics
// in this file, the source is published.
#include "lyrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void test_qenc(void)
{
    char b[64];
    CHECK(lyrics_qenc("AZaz09-_.~", b, sizeof(b)) && strcmp(b, "AZaz09-_.~") == 0,
          "unreserved kept: '%s'", b);
    CHECK(lyrics_qenc("a b", b, sizeof(b)) && strcmp(b, "a%20b") == 0, "space: '%s'", b);
    CHECK(lyrics_qenc("&=+#?%/", b, sizeof(b)) && strcmp(b, "%26%3D%2B%23%3F%25%2F") == 0,
          "query specials: '%s'", b);
    CHECK(lyrics_qenc("\xC3\xA9", b, sizeof(b)) && strcmp(b, "%C3%A9") == 0, "UTF-8: '%s'", b);
    CHECK(lyrics_qenc("", b, sizeof(b)) && b[0] == '\0', "empty input");

    char small[6];
    CHECK(lyrics_qenc("ab cd", small, sizeof(small)) == false, "overflow refused");
    CHECK(lyrics_qenc("abcde", small, sizeof(small)) && strcmp(small, "abcde") == 0, "exact fit");
    CHECK(lyrics_qenc("abcdef", small, sizeof(small)) == false, "one byte too long");
}

static void test_url(void)
{
    char u[LYRICS_URL_MAX];
    CHECK(lyrics_query_url("Test Song", "The Bench & Co", 0, u, sizeof(u)), "url built");
    CHECK(strcmp(u, "https://lrclib.net/api/get?track_name=Test%20Song"
                    "&artist_name=The%20Bench%20%26%20Co") == 0, "no duration: '%s'", u);

    CHECK(lyrics_query_url("A", "B", 1, u, sizeof(u)) && strstr(u, "&duration=1") != NULL,
          "duration 1 sent: '%s'", u);
    CHECK(lyrics_query_url("A", "B", 3600, u, sizeof(u)) && strstr(u, "&duration=3600") != NULL,
          "duration 3600 sent");
    CHECK(lyrics_query_url("A", "B", 3601, u, sizeof(u)) && strstr(u, "duration") == NULL,
          "duration 3601 left out: '%s'", u);

    CHECK(lyrics_query_url("", "B", 0, u, sizeof(u)) == false, "empty title refused");
    CHECK(lyrics_query_url("A", "", 0, u, sizeof(u)) == false, "empty artist refused");
    CHECK(lyrics_query_url(NULL, "B", 0, u, sizeof(u)) == false, "NULL title refused");

    char small[48];
    CHECK(lyrics_query_url("A", "B", 0, small, sizeof(small)) == false, "small buffer refused");
}

static void test_icy(void)
{
    char a[32], t[32];
    CHECK(lyrics_icy_split("The Bench - Test Song", a, sizeof(a), t, sizeof(t)), "split");
    CHECK(strcmp(a, "The Bench") == 0 && strcmp(t, "Test Song") == 0, "parts: '%s' / '%s'", a, t);

    CHECK(lyrics_icy_split("One - Two - Three", a, sizeof(a), t, sizeof(t)), "several");
    CHECK(strcmp(a, "One") == 0 && strcmp(t, "Two - Three") == 0,
          "first separator wins: '%s' / '%s'", a, t);

    CHECK(lyrics_icy_split("Morning program", a, sizeof(a), t, sizeof(t)) == false, "no separator");
    CHECK(lyrics_icy_split("Self-made song", a, sizeof(a), t, sizeof(t)) == false,
          "a hyphen without spaces is not a separator");
    CHECK(lyrics_icy_split(" - Title", a, sizeof(a), t, sizeof(t)) == false, "empty artist");
    CHECK(lyrics_icy_split("Artist - ", a, sizeof(a), t, sizeof(t)) == false, "empty title");
    CHECK(lyrics_icy_split("", a, sizeof(a), t, sizeof(t)) == false, "empty");
    CHECK(lyrics_icy_split(NULL, a, sizeof(a), t, sizeof(t)) == false, "NULL");

    // A long side is bounded and never ends inside a UTF-8 sequence.
    char sa[8], st[8];
    CHECK(lyrics_icy_split("abcdef\xC3\xA9xyz - T", sa, sizeof(sa), st, sizeof(st)), "long side");
    CHECK(strcmp(sa, "abcdef") == 0, "bounded and trimmed: '%s'", sa);
}

static void test_lrc_basic(void)
{
    char text[] = "[00:01.00] first test line\n"
                  "[00:05.50]second test line\n"
                  "[01:02.345] third test line\n";
    lyrics_line_t l[8];
    size_t n = lyrics_parse_lrc(text, l, 8);
    CHECK(n == 3, "3 lines, got %zu", n);
    CHECK(l[0].ms == 1000 && strcmp(l[0].text, "first test line") == 0,
          "line 0: %u '%s'", (unsigned)l[0].ms, l[0].text);
    CHECK(l[1].ms == 5500 && strcmp(l[1].text, "second test line") == 0,
          "line 1: %u '%s'", (unsigned)l[1].ms, l[1].text);
    CHECK(l[2].ms == 62345, "line 2 at %u", (unsigned)l[2].ms);
}

static void test_lrc_forms(void)
{
    // Tags dropped, CRLF, an empty timed line (a pause), a line without a
    // stamp, a malformed stamp, a one-digit fraction, no fraction at all.
    char text[] = "\xEF\xBB\xBF[ar:The Bench]\r\n"
                  "[offset:+250]\r\n"
                  "[00:10.5] tenth and a half\r\n"
                  "[00:20]\r\n"
                  "no stamp here\r\n"
                  "[00:xx.00] broken stamp\r\n"
                  "[00:30] no fraction\r\n";
    lyrics_line_t l[8];
    size_t n = lyrics_parse_lrc(text, l, 8);
    CHECK(n == 3, "3 lines kept, got %zu", n);
    CHECK(l[0].ms == 10500 && strcmp(l[0].text, "tenth and a half") == 0,
          "one-digit fraction: %u '%s'", (unsigned)l[0].ms, l[0].text);
    CHECK(l[1].ms == 20000 && l[1].text[0] == '\0', "empty timed line kept as a pause");
    CHECK(l[2].ms == 30000 && strcmp(l[2].text, "no fraction") == 0, "no fraction: '%s'", l[2].text);
}

static void test_lrc_multi_and_sort(void)
{
    char text[] = "[00:40.00] late line\n"
                  "[00:10.00][00:30.00] repeated line\n"
                  "[00:20.00] middle line\n";
    lyrics_line_t l[8];
    size_t n = lyrics_parse_lrc(text, l, 8);
    CHECK(n == 4, "4 entries, got %zu", n);
    CHECK(l[0].ms == 10000 && l[1].ms == 20000 && l[2].ms == 30000 && l[3].ms == 40000,
          "sorted by time");
    CHECK(strcmp(l[0].text, "repeated line") == 0 && strcmp(l[2].text, "repeated line") == 0,
          "both stamps share the text");
    CHECK(strcmp(l[1].text, "middle line") == 0, "middle: '%s'", l[1].text);
}

static void test_lrc_cap(void)
{
    // More lines than the table holds: the first `max` are kept, no overflow.
    size_t count = LYRICS_LINES_MAX + 25;
    char *text = malloc(count * 24 + 1);
    size_t off = 0;
    for (size_t i = 0; i < count; i++) {
        off += (size_t)sprintf(text + off, "[%02u:%02u.00] line %u\n",
                               (unsigned)(i / 60), (unsigned)(i % 60), (unsigned)i);
    }
    lyrics_line_t *l = malloc(sizeof(*l) * LYRICS_LINES_MAX);
    size_t n = lyrics_parse_lrc(text, l, LYRICS_LINES_MAX);
    CHECK(n == LYRICS_LINES_MAX, "capped at %d, got %zu", LYRICS_LINES_MAX, n);
    CHECK(l[n - 1].ms == (uint32_t)(LYRICS_LINES_MAX - 1) * 1000, "last kept line");
    free(l);
    free(text);

    // Stamps that would wrap or make no sense are dropped, not wrapped.
    char huge[] = "[999999:00.00] wrapped stamp\n[00:75.00] bad seconds\n[599:59.99] last valid\n";
    lyrics_line_t h[4];
    size_t hn = lyrics_parse_lrc(huge, h, 4);
    CHECK(hn == 1 && h[0].ms == 599u * 60000u + 59990u, "only the valid stamp kept (%zu)", hn);

    char tags_only[] = "[ar:The Bench]\n[ti:Test Song]\n";
    CHECK(lyrics_parse_lrc(tags_only, h, 4) == 0, "tags only: no line");

    char empty[] = "";
    lyrics_line_t one[1];
    CHECK(lyrics_parse_lrc(empty, one, 1) == 0, "empty text");
    CHECK(lyrics_parse_lrc(NULL, one, 1) == 0, "NULL text");
}

static void test_line_at(void)
{
    lyrics_line_t l[3] = { {1000, "a"}, {5000, "b"}, {9000, "c"} };
    CHECK(lyrics_line_at(l, 3, 0) == -1, "before the first");
    CHECK(lyrics_line_at(l, 3, 999) == -1, "just before the first");
    CHECK(lyrics_line_at(l, 3, 1000) == 0, "on the first stamp");
    CHECK(lyrics_line_at(l, 3, 4999) == 0, "inside the first");
    CHECK(lyrics_line_at(l, 3, 5000) == 1, "on the second stamp");
    CHECK(lyrics_line_at(l, 3, 9000) == 2, "on the last stamp");
    CHECK(lyrics_line_at(l, 3, 4000000) == 2, "far after the last");
    CHECK(lyrics_line_at(l, 0, 5000) == -1, "no lines");
    CHECK(lyrics_line_at(NULL, 3, 5000) == -1, "NULL table");
}

static void test_synced_rule(void)
{
    CHECK(lyrics_use_synced(200000, 200000), "same duration");
    CHECK(lyrics_use_synced(200000, 203000), "3 s longer: still synced");
    CHECK(lyrics_use_synced(203000, 200000), "3 s shorter: still synced");
    CHECK(!lyrics_use_synced(200000, 203001), "3.001 s apart: plain");
    CHECK(!lyrics_use_synced(203001, 200000), "3.001 s apart the other way: plain");
    CHECK(!lyrics_use_synced(0, 200000), "our duration unknown: plain");
    CHECK(!lyrics_use_synced(200000, 0), "record duration unknown: plain");
}

static void test_key(void)
{
    CHECK(lyrics_key("Song", "Band") == lyrics_key("Song", "Band"), "stable");
    CHECK(lyrics_key("Song", "Band") != lyrics_key("Song", "Other"), "artist matters");
    CHECK(lyrics_key("Song", "Band") != lyrics_key("Other", "Band"), "title matters");
    CHECK(lyrics_key("ab", "c") != lyrics_key("a", "bc"), "the two fields are separated");
    CHECK(lyrics_key(NULL, NULL) == lyrics_key("", ""), "NULL is empty");
}

static void test_copy_text(void)
{
    char b[32];
    size_t n = lyrics_copy_text(b, sizeof(b), "\xEF\xBB\xBFline one\r\nline two\r\n");
    CHECK(strcmp(b, "line one\nline two\n") == 0 && n == strlen(b), "BOM and CR dropped: '%s'", b);

    CHECK(lyrics_copy_text(b, sizeof(b), NULL) == 0 && b[0] == '\0', "NULL gives empty");

    // The cut lands inside the two-byte e-acute: the half sequence goes.
    char s[8];
    lyrics_copy_text(s, sizeof(s), "abcdef\xC3\xA9tail");
    CHECK(strcmp(s, "abcdef") == 0, "partial UTF-8 dropped at the cut: '%s'", s);

    // Same at the real limit.
    char *in = malloc(LYRICS_TEXT_MAX + 16);
    memset(in, 'x', LYRICS_TEXT_MAX + 15);
    in[LYRICS_TEXT_MAX + 15] = '\0';
    in[LYRICS_TEXT_MAX - 2] = (char)0xC3;
    in[LYRICS_TEXT_MAX - 1] = (char)0xA9;
    char *out = malloc(LYRICS_TEXT_MAX);
    n = lyrics_copy_text(out, LYRICS_TEXT_MAX, in);
    CHECK(n == LYRICS_TEXT_MAX - 2 && out[n - 1] == 'x', "8 KB cut is UTF-8 clean (len %zu)", n);
    free(out);
    free(in);
}

static void test_action(void)
{
    CHECK(lyrics_next_action(200, true, false) == LYRICS_ACT_USE, "200 used");
    CHECK(lyrics_next_action(200, false, true) == LYRICS_ACT_ERROR, "200 over the cap is an error");
    CHECK(lyrics_next_action(404, true, false) == LYRICS_ACT_RETRY_NO_DURATION,
          "404 with a duration: once more without");
    CHECK(lyrics_next_action(404, false, false) == LYRICS_ACT_NOT_FOUND, "404 without: not found");
    CHECK(lyrics_next_action(429, true, false) == LYRICS_ACT_BACKOFF, "429 backs off");
    CHECK(lyrics_next_action(500, true, false) == LYRICS_ACT_ERROR, "500 is an error");
    CHECK(lyrics_next_action(-1, false, false) == LYRICS_ACT_ERROR, "transport failure is an error");

    CHECK(lyrics_backoff_s("120") == 120, "Retry-After seconds");
    CHECK(lyrics_backoff_s(NULL) == 60, "no header: 60 s");
    CHECK(lyrics_backoff_s("") == 60, "empty header: 60 s");
    CHECK(lyrics_backoff_s("Wed, 21 Oct 2026 07:28:00 GMT") == 60, "date form: 60 s");
    CHECK(lyrics_backoff_s("0") == 60, "zero: 60 s");
    CHECK(lyrics_backoff_s("99999") == 600, "capped at 600 s");
    CHECK(lyrics_backoff_s("-5") == 60, "negative: 60 s");
    CHECK(lyrics_backoff_s(" 30") == 60, "leading space: 60 s");
    CHECK(lyrics_backoff_s("30 ") == 60, "trailing space: 60 s");
}

static void test_json_shape(void)
{
    CHECK(lyrics_json_shape_ok("{\"a\":1}", 8), "object accepted");
    CHECK(lyrics_json_shape_ok("  \n{\"a\":{\"b\":[1,2]}}", 8), "leading space, nesting 3");
    CHECK(!lyrics_json_shape_ok("[1,2]", 8), "array at the top refused");
    CHECK(!lyrics_json_shape_ok("", 8), "empty refused");
    CHECK(!lyrics_json_shape_ok(NULL, 8), "NULL refused");
    CHECK(!lyrics_json_shape_ok("{\"a\":[[[[[[[[[1]]]]]]]]]}", 8), "too deep refused");
    CHECK(lyrics_json_shape_ok("{\"a\":\"[[[[[[[[[[[[ {{{{ \\\" [[[\"}", 8),
          "brackets inside a string do not count");
}

static void test_sidecar(void)
{
    char p[64];
    CHECK(lyrics_sidecar_path("/sdcard/Music/Song.mp3", ".lrc", p, sizeof(p)) &&
          strcmp(p, "/sdcard/Music/Song.lrc") == 0, "extension replaced: '%s'", p);
    CHECK(lyrics_sidecar_path("/sdcard/Music/Song.flac", ".txt", p, sizeof(p)) &&
          strcmp(p, "/sdcard/Music/Song.txt") == 0, "txt form: '%s'", p);
    CHECK(lyrics_sidecar_path("/sdcard/Vol. 2/Song", ".lrc", p, sizeof(p)) &&
          strcmp(p, "/sdcard/Vol. 2/Song.lrc") == 0,
          "a dot in a folder name is not an extension: '%s'", p);
    CHECK(lyrics_sidecar_path("/sdcard/a.b.mp3", ".lrc", p, sizeof(p)) &&
          strcmp(p, "/sdcard/a.b.lrc") == 0, "last dot only: '%s'", p);
    CHECK(lyrics_sidecar_path("", ".lrc", p, sizeof(p)) == false, "empty path");
    CHECK(lyrics_sidecar_path(NULL, ".lrc", p, sizeof(p)) == false, "NULL path");

    char small[12];
    CHECK(lyrics_sidecar_path("/sdcard/Song.mp3", ".lrc", small, sizeof(small)) == false,
          "path too long refused");

    CHECK(lyrics_save_form(true, true, true) == LYRICS_SAVE_LRC, "synced and trusted: lrc");
    CHECK(lyrics_save_form(true, false, true) == LYRICS_SAVE_NONE,
          "synced of a doubtful version: nothing (a txt would hide the synced one for good)");
    CHECK(lyrics_save_form(false, false, true) == LYRICS_SAVE_TXT, "plain only: txt");
    CHECK(lyrics_save_form(true, false, false) == LYRICS_SAVE_NONE,
          "doubtful synced without plain text: nothing");
    CHECK(lyrics_save_form(false, false, false) == LYRICS_SAVE_NONE, "nothing to save");
}

int main(void)
{
    test_qenc();
    test_url();
    test_icy();
    test_lrc_basic();
    test_lrc_forms();
    test_lrc_multi_and_sort();
    test_lrc_cap();
    test_line_at();
    test_synced_rule();
    test_key();
    test_copy_text();
    test_action();
    test_json_shape();
    test_sidecar();
    if (g_fail) { printf("%d FAILED\n", g_fail); return 1; }
    printf("all lyrics tests passed\n");
    return 0;
}
