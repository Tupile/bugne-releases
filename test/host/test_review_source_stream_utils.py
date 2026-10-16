from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def function(text, name):
    match = re.search(r"^.*\b" + name + r"\([^;]*?\)\n\{", text, re.M)
    assert match, name
    opening = text.index("{", match.end() - 1)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end]

stream = (ROOT / "components/source_stream/source_stream.c").read_text(encoding="utf-8")

prelude = r'''
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <strings.h>

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define CHECK_STR(got, want, what) \
    CHECK(strcmp((got), (want)) == 0, "%s: got \"%s\", want \"%s\"", (what), (got), (want))

'''

body = "\n".join(function(stream, name) for name in (
    "url_has_ext", "is_playlist", "playlist_extract"
))

cases = r'''
static void test_url_has_ext(void)
{
    CHECK(url_has_ext("http://example.com/stream.mp3", ".mp3"), "matches simple extension");
    CHECK(url_has_ext("http://example.com/stream.MP3", ".mp3"), "case insensitive");
    CHECK(url_has_ext("http://example.com/stream.m3u8", ".m3u8"), "matches m3u8");
    CHECK(!url_has_ext("http://example.com/stream.mp3?token=123", ".mp3"), "fails if query string appended");
    CHECK(!url_has_ext("http://example.com/stream.mp3", ".m3u"), "fails wrong extension");
    CHECK(url_has_ext(".mp3", ".mp3"), "matches exact extension");
    CHECK(!url_has_ext("mp3", ".mp3"), "fails missing dot");
}

static void test_is_playlist(void)
{
    CHECK(is_playlist("http://x.com/a.m3u"), "matches .m3u");
    CHECK(is_playlist("http://x.com/a.m3u8"), "matches .m3u8");
    CHECK(is_playlist("http://x.com/a.pls"), "matches .pls");
    CHECK(is_playlist("http://x.com/a.M3U"), "case insensitive m3u");
    CHECK(!is_playlist("http://x.com/a.mp3"), "does not match mp3");
}

static void test_playlist_extract(void)
{
    char out[256];
    bool ok;

    const char *p1 = "some text\nhttp://example.com/stream\nmore text";
    ok = playlist_extract(p1, out, sizeof(out));
    CHECK(ok, "extract http ok");
    CHECK_STR(out, "http://example.com/stream", "extract simple http");

    const char *p2 = "File1=https://example.com/live\r\nTitle1=Test";
    ok = playlist_extract(p2, out, sizeof(out));
    CHECK(ok, "extract https ok");
    CHECK_STR(out, "https://example.com/live", "extract simple https");

    const char *p3 = "http://first.com\nhttp://second.com";
    ok = playlist_extract(p3, out, sizeof(out));
    CHECK(ok, "extract first ok");
    CHECK_STR(out, "http://first.com", "extracts first url");

    const char *p4 = "some noise\nhttps://second.com\nhttp://first.com";
    ok = playlist_extract(p4, out, sizeof(out));
    CHECK(ok, "extract earlier url");
    CHECK_STR(out, "https://second.com", "extracts the earliest url regardless of protocol");

    const char *p5 = "http://a.com/b c";
    ok = playlist_extract(p5, out, sizeof(out));
    CHECK(ok, "extract space terminated");
    CHECK_STR(out, "http://a.com/b", "extracts up to space");

    const char *p6 = "http://123456789";
    char small_out[15];
    ok = playlist_extract(p6, small_out, sizeof(small_out));
    CHECK(ok, "extract small out ok");
    CHECK_STR(small_out, "http://1234567", "truncated properly");

    const char *p7 = "just some text";
    ok = playlist_extract(p7, out, sizeof(out));
    CHECK(!ok, "extract no url returns false");
}

int main(void)
{
    test_url_has_ext();
    test_is_playlist();
    test_playlist_extract();

    if (g_fail == 0) {
        printf("OK: all source_stream utils host tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", g_fail);
    return 1;
}
'''

with tempfile.TemporaryDirectory(prefix="bugne-source-stream-") as directory:
    source = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    source.write_text(prelude + body + cases)
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
