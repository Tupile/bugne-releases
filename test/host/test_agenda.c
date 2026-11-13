// Host unit tests for the agenda data module (agenda.c).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "agenda.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void test_copy(void)
{
    char b[8];
    agenda_copy_field(b, sizeof(b), "abc");
    CHECK(strcmp(b, "abc") == 0, "plain copy: '%s'", b);

    agenda_copy_field(b, sizeof(b), NULL);
    CHECK(b[0] == '\0', "NULL gives empty");

    agenda_copy_field(b, sizeof(b), "0123456789");
    CHECK(strcmp(b, "0123456") == 0, "bounded: '%s'", b);

    // "abcdef" + e-acute (2 bytes): the bound cuts the sequence in half.
    agenda_copy_field(b, sizeof(b), "abcdef\xC3\xA9");
    CHECK(strcmp(b, "abcdef") == 0, "partial UTF-8 dropped: '%s'", b);

    // A complete sequence that fits is kept.
    agenda_copy_field(b, sizeof(b), "abcde\xC3\xA9");
    CHECK(strcmp(b, "abcde\xC3\xA9") == 0, "whole UTF-8 kept: '%s'", b);

    agenda_copy_field(b, sizeof(b), "a\nb\tc\x7f");
    CHECK(strcmp(b, "a b c ") == 0, "control chars to spaces: '%s'", b);
}

static void test_empty(void)
{
    agenda_t a;
    memset(&a, 0, sizeof(a));
    CHECK(agenda_empty(&a), "zeroed is empty");
    a.rain = true;
    CHECK(agenda_empty(&a), "rain alone is still empty");
    a.rain = false;
    strcpy(a.temp, "12");
    CHECK(!agenda_empty(&a), "temperature is content");
    a.temp[0] = '\0';
    strcpy(a.cond, "sunny");
    CHECK(!agenda_empty(&a), "condition is content");
    a.cond[0] = '\0';
    a.count = 1;
    CHECK(!agenda_empty(&a), "an event is content");
    a.count = 0;
    a.has_events = true;
    CHECK(!agenda_empty(&a), "a sent empty list is content");
}

static void test_icon(void)
{
    static const char *known[] = {
        "sunny", "clear-night", "partlycloudy", "cloudy", "fog", "rainy",
        "pouring", "hail", "snowy", "snowy-rainy", "lightning",
        "lightning-rainy", "windy", "windy-variant",
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        const char *g = agenda_cond_icon(known[i]);
        // Every glyph is a 3-byte private-use code point (U+Fxxx).
        CHECK(g && strlen(g) == 3 && (unsigned char)g[0] == 0xEF,
              "icon for %s", known[i]);
    }
    CHECK(strcmp(agenda_cond_icon("sunny"), "\xEF\x86\x85") == 0, "sunny is U+F185");
    CHECK(strcmp(agenda_cond_icon("rainy"), "\xEF\x9C\xBD") == 0, "rainy is U+F73D");
    CHECK(agenda_cond_icon("pouring") == agenda_cond_icon("hail"), "pouring and hail share");
    CHECK(agenda_cond_icon("exceptional") == NULL, "exceptional has none");
    CHECK(agenda_cond_icon("volcano") == NULL, "unknown has none");
    CHECK(agenda_cond_icon("") == NULL, "empty has none");
    CHECK(agenda_cond_icon(NULL) == NULL, "NULL has none");
    CHECK(agenda_cond_icon("Sunny") == NULL, "match is exact");
}

static void test_fresh(void)
{
    CHECK(!agenda_fresh(false, 0, 10), "nothing received");
    CHECK(agenda_fresh(true, 100, 100), "just received");
    CHECK(agenda_fresh(true, 100, 100 + AGENDA_FRESH_S - 1), "last fresh second");
    CHECK(!agenda_fresh(true, 100, 100 + AGENDA_FRESH_S), "stale at the limit");
    CHECK(!agenda_fresh(true, 100, 50), "clock going backwards is stale");
}

int main(void)
{
    test_copy();
    test_empty();
    test_icon();
    test_fresh();
    if (g_fail == 0) printf("OK: all agenda host tests passed\n");
    return g_fail != 0;
}
