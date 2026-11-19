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

static void test_date(void)
{
    char b[48];
    // 2026-11-19 is a Thursday (tm_wday 4, tm_mon 10).
    agenda_date_fmt(b, sizeof(b), 4, 19, 10, "fr");
    CHECK(strcmp(b, "jeudi 19 novembre") == 0, "fr date: %s", b);
    agenda_date_fmt(b, sizeof(b), 4, 19, 10, "en");
    CHECK(strcmp(b, "Thursday 19 November") == 0, "en date: %s", b);
    agenda_date_fmt(b, sizeof(b), 4, 19, 10, "de");
    CHECK(strcmp(b, "Donnerstag, 19. November") == 0, "de date: %s", b);
    agenda_date_fmt(b, sizeof(b), 4, 19, 10, "xx");
    CHECK(strcmp(b, "Thursday 19 November") == 0, "unknown language is English: %s", b);
    agenda_date_fmt(b, sizeof(b), 4, 19, 10, NULL);
    CHECK(strcmp(b, "Thursday 19 November") == 0, "NULL language is English: %s", b);

    // Accented names.
    agenda_date_fmt(b, sizeof(b), 0, 1, 1, "fr");
    CHECK(strcmp(b, "dimanche 1 f\xC3\xA9vrier") == 0, "fr accent: %s", b);
    agenda_date_fmt(b, sizeof(b), 6, 15, 7, "fr");
    CHECK(strcmp(b, "samedi 15 ao\xC3\xBBt") == 0, "fr accent 2: %s", b);
    agenda_date_fmt(b, sizeof(b), 1, 2, 2, "de");
    CHECK(strcmp(b, "Montag, 2. M\xC3\xA4rz") == 0, "de accent: %s", b);

    // Every weekday and month name, pinned exactly (Sunday first, as tm_wday).
    static const char *langs[3] = { "en", "fr", "de" };
    static const char *days[3][7] = {
        { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" },
        { "dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi" },
        { "Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag" },
    };
    static const char *months[3][12] = {
        { "January", "February", "March", "April", "May", "June", "July", "August",
          "September", "October", "November", "December" },
        { "janvier", "f\xC3\xA9vrier", "mars", "avril", "mai", "juin", "juillet", "ao\xC3\xBBt",
          "septembre", "octobre", "novembre", "d\xC3\xA9" "cembre" },
        { "Januar", "Februar", "M\xC3\xA4rz", "April", "Mai", "Juni", "Juli", "August",
          "September", "Oktober", "November", "Dezember" },
    };
    for (int l = 0; l < 3; l++) {
        for (int w = 0; w < 7; w++) {
            for (int m = 0; m < 12; m++) {
                char want[48];
                snprintf(want, sizeof(want), l == 2 ? "%s, 28. %s" : "%s 28 %s", days[l][w], months[l][m]);
                agenda_date_fmt(b, sizeof(b), w, 28, m, langs[l]);
                CHECK(strcmp(b, want) == 0, "%s wday %d mon %d: %s", langs[l], w, m, b);
            }
        }
    }

    // Out-of-range input shows nothing rather than a wrong date.
    strcpy(b, "x"); agenda_date_fmt(b, sizeof(b), 7, 19, 10, "fr");
    CHECK(b[0] == '\0', "wday 7");
    strcpy(b, "x"); agenda_date_fmt(b, sizeof(b), -1, 19, 10, "fr");
    CHECK(b[0] == '\0', "wday -1");
    strcpy(b, "x"); agenda_date_fmt(b, sizeof(b), 4, 19, 12, "fr");
    CHECK(b[0] == '\0', "mon 12");
    strcpy(b, "x"); agenda_date_fmt(b, sizeof(b), 4, 0, 10, "fr");
    CHECK(b[0] == '\0', "mday 0");
    strcpy(b, "x"); agenda_date_fmt(b, sizeof(b), 4, 32, 10, "fr");
    CHECK(b[0] == '\0', "mday 32");

    // A small buffer never ends inside a multi-byte character:
    // "dimanche 1 f" + first byte of the e-acute would be 13 bytes.
    char small[14];
    agenda_date_fmt(small, sizeof(small), 0, 1, 1, "fr");
    CHECK(strcmp(small, "dimanche 1 f") == 0, "truncation keeps valid UTF-8: %s", small);
    agenda_date_fmt(NULL, 8, 4, 19, 10, "fr");  // must not crash
    agenda_date_fmt(small, 0, 4, 19, 10, "fr");
}

static void test_date_short(void)
{
    char b[48];
    // 2026-11-19 is a Thursday (tm_wday 4, tm_mon 10).
    agenda_date_fmt_short(b, sizeof(b), 4, 19, 10, "fr");
    CHECK(strcmp(b, "jeu. 19 nov.") == 0, "fr short date: %s", b);
    agenda_date_fmt_short(b, sizeof(b), 4, 19, 10, "en");
    CHECK(strcmp(b, "Thu 19 Nov") == 0, "en short date: %s", b);
    agenda_date_fmt_short(b, sizeof(b), 4, 19, 10, "de");
    CHECK(strcmp(b, "Do., 19. Nov.") == 0, "de short date: %s", b);
    agenda_date_fmt_short(b, sizeof(b), 4, 19, 10, "xx");
    CHECK(strcmp(b, "Thu 19 Nov") == 0, "unknown language is English: %s", b);
    agenda_date_fmt_short(b, sizeof(b), 4, 19, 10, NULL);
    CHECK(strcmp(b, "Thu 19 Nov") == 0, "NULL language is English: %s", b);

    // Every short weekday and month name, pinned exactly (Sunday first).
    static const char *langs[3] = { "en", "fr", "de" };
    static const char *days[3][7] = {
        { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" },
        { "dim.", "lun.", "mar.", "mer.", "jeu.", "ven.", "sam." },
        { "So.", "Mo.", "Di.", "Mi.", "Do.", "Fr.", "Sa." },
    };
    static const char *months[3][12] = {
        { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" },
        { "janv.", "f\xC3\xA9vr.", "mars", "avr.", "mai", "juin", "juil.", "ao\xC3\xBBt",
          "sept.", "oct.", "nov.", "d\xC3\xA9" "c." },
        { "Jan.", "Feb.", "M\xC3\xA4rz", "Apr.", "Mai", "Juni", "Juli", "Aug.", "Sept.", "Okt.",
          "Nov.", "Dez." },
    };
    for (int l = 0; l < 3; l++) {
        for (int w = 0; w < 7; w++) {
            for (int m = 0; m < 12; m++) {
                char want[48];
                snprintf(want, sizeof(want), l == 2 ? "%s, 28. %s" : "%s 28 %s", days[l][w], months[l][m]);
                agenda_date_fmt_short(b, sizeof(b), w, 28, m, langs[l]);
                CHECK(strcmp(b, want) == 0, "short %s wday %d mon %d: %s", langs[l], w, m, b);
            }
        }
    }

    // Out-of-range input shows nothing rather than a wrong date.
    strcpy(b, "x"); agenda_date_fmt_short(b, sizeof(b), 7, 19, 10, "fr");
    CHECK(b[0] == '\0', "short wday 7");
    strcpy(b, "x"); agenda_date_fmt_short(b, sizeof(b), -1, 19, 10, "fr");
    CHECK(b[0] == '\0', "short wday -1");
    strcpy(b, "x"); agenda_date_fmt_short(b, sizeof(b), 4, 19, 12, "fr");
    CHECK(b[0] == '\0', "short mon 12");
    strcpy(b, "x"); agenda_date_fmt_short(b, sizeof(b), 4, 0, 10, "fr");
    CHECK(b[0] == '\0', "short mday 0");
    strcpy(b, "x"); agenda_date_fmt_short(b, sizeof(b), 4, 32, 10, "fr");
    CHECK(b[0] == '\0', "short mday 32");

    // "dim. 1 f" + the first byte of the e-acute would be 9 bytes.
    char small[10];
    agenda_date_fmt_short(small, sizeof(small), 0, 1, 1, "fr");
    CHECK(strcmp(small, "dim. 1 f") == 0, "short truncation keeps valid UTF-8: %s", small);
    agenda_date_fmt_short(NULL, 8, 4, 19, 10, "fr");  // must not crash
    agenda_date_fmt_short(small, 0, 4, 19, 10, "fr");
}

int main(void)
{
    test_copy();
    test_empty();
    test_icon();
    test_fresh();
    test_date();
    test_date_short();
    if (g_fail == 0) printf("OK: all agenda host tests passed\n");
    return g_fail != 0;
}
