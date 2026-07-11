// Host unit tests for the RSS parser core (rss_parse.c + yxml.c).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "rss_parse.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define CHECK_INT(got, want, what) \
    CHECK((got) == (want), "%s: got %d, want %d", (what), (int)(got), (int)(want))

#define CHECK_STR(got, want, what) \
    CHECK(strcmp((got), (want)) == 0, "%s: got \"%s\", want \"%s\"", (what), (got), (want))

static const char SAMPLE[] =
    "<?xml version=\"1.0\"?>\n"
    "<rss version=\"2.0\" xmlns:itunes=\"http://www.itunes.com/dtds/podcast-1.0.dtd\">\n"
    " <channel>\n"
    "  <title>My Show</title>\n"
    "  <item>\n"
    "    <title>Episode One</title>\n"
    "    <pubDate>Mon, 02 Jun 2026 06:00:00 GMT</pubDate>\n"
    "    <itunes:duration>1:02:03</itunes:duration>\n"
    "    <enclosure url=\"https://ex.com/ep1.mp3\" length=\"1\" type=\"audio/mpeg\"/>\n"
    "  </item>\n"
    "  <item>\n"
    "    <title>Episode Two</title>\n"
    "    <pubDate>Tue, 03 Jun 2026 06:00:00 GMT</pubDate>\n"
    "    <itunes:duration>185</itunes:duration>\n"
    "    <enclosure url=\"https://ex.com/ep2.flac\" type=\"audio/flac\"/>\n"
    "  </item>\n"
    "  <item>\n"
    "    <title>A &amp; B</title>\n"
    "    <itunes:duration>12:34</itunes:duration>\n"
    "    <enclosure url=\"https://ex.com/ep3.mp3\"/>\n"
    "  </item>\n"
    "  <item>\n"
    "    <title>No Enclosure Here</title>\n"
    "  </item>\n"
    " </channel>\n"
    "</rss>\n";

// Collector: the parser now emits each episode through a callback instead of
// storing them in an array, so the test gathers them here.
#define COLLECT_MAX 8
typedef struct {
    rss_episode_t eps[COLLECT_MAX];
    size_t count;
} collector_t;

static void collect_cb(const rss_episode_t *ep, void *ctx)
{
    collector_t *c = ctx;
    if (c->count < COLLECT_MAX) c->eps[c->count] = *ep;
    c->count++;
}

// Feed the buffer in small slices to exercise yxml's streaming across boundaries.
static void feed_chunked(rss_parser_t *p, const char *s, size_t step)
{
    size_t len = strlen(s);
    for (size_t off = 0; off < len; off += step) {
        size_t n = (len - off < step) ? (len - off) : step;
        CHECK(rss_parse_feed(p, s + off, n), "feed returned syntax error at offset %zu", off);
    }
}

static void test_sample(void)
{
    char ybuf[RSS_YXML_BUF_SIZE];
    collector_t c = {0};
    rss_parser_t p;
    rss_parse_init(&p, ybuf, sizeof(ybuf), collect_cb, &c);
    feed_chunked(&p, SAMPLE, 7);

    CHECK_STR(p.podcast_title, "My Show", "podcast_title");
    CHECK_INT(c.count, 3, "episode count (item without enclosure skipped)");
    CHECK_INT(p.emitted, 3, "emitted count");

    if (c.count >= 1) {
        CHECK_STR(c.eps[0].title, "Episode One", "ep0 title");
        CHECK_STR(c.eps[0].url, "https://ex.com/ep1.mp3", "ep0 url");
        CHECK_STR(c.eps[0].date, "Mon, 02 Jun 2026 06:00:00 GMT", "ep0 date");
        CHECK_INT(c.eps[0].duration_seconds, 3723, "ep0 duration 1:02:03");
    }
    if (c.count >= 2) {
        CHECK_STR(c.eps[1].title, "Episode Two", "ep1 title");
        CHECK_STR(c.eps[1].url, "https://ex.com/ep2.flac", "ep1 url");
        CHECK_INT(c.eps[1].duration_seconds, 185, "ep1 duration 185");
    }
    if (c.count >= 3) {
        CHECK_STR(c.eps[2].title, "A & B", "ep2 title (entity decoded)");
        CHECK_INT(c.eps[2].duration_seconds, 754, "ep2 duration 12:34");
    }
}

static void test_duration(void)
{
    CHECK_INT(rss_parse_duration("3600"), 3600, "duration plain seconds");
    CHECK_INT(rss_parse_duration("12:34"), 754, "duration MM:SS");
    CHECK_INT(rss_parse_duration("1:02:03"), 3723, "duration HH:MM:SS");
    CHECK_INT(rss_parse_duration(""), 0, "duration empty");
    CHECK_INT(rss_parse_duration("0"), 0, "duration zero");
    CHECK_INT(rss_parse_duration("1:2:3:4"), 3723, "duration >3 parts");
    CHECK_INT(rss_parse_duration("abc"), 0, "duration non-numeric");
    CHECK_INT(rss_parse_duration("12:abc"), 720, "duration partial non-numeric");
}

static void test_truncation(void)
{
    // A title far longer than RSS_TITLE_MAX must be bounded, not overflow.
    char xml[RSS_TITLE_MAX * 4];
    int off = snprintf(xml, sizeof(xml),
        "<rss><channel><title>");
    for (int i = 0; i < RSS_TITLE_MAX + 50 && off < (int)sizeof(xml) - 32; i++) {
        xml[off++] = 'x';
    }
    off += snprintf(xml + off, sizeof(xml) - off, "</title></channel></rss>");
    (void)off;

    char ybuf[RSS_YXML_BUF_SIZE];
    collector_t c = {0};
    rss_parser_t p;
    rss_parse_init(&p, ybuf, sizeof(ybuf), collect_cb, &c);
    CHECK(rss_parse_feed(&p, xml, strlen(xml)), "long-title feed syntax error");
    CHECK(strlen(p.podcast_title) < RSS_TITLE_MAX, "title bounded to < RSS_TITLE_MAX");
}

int main(void)
{
    test_duration();
    test_sample();
    test_truncation();
    if (g_fail == 0) {
        printf("OK: all rss_parse host tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", g_fail);
    return 1;
}
