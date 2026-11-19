// agenda: see agenda.h. Pure, host-tested (test/host/test_agenda.c).
#include "agenda.h"

#include <stdio.h>
#include <string.h>

#include "tags.h"

void agenda_copy_field(char *out, size_t size, const char *src)
{
    if (!out || size == 0) return;
    size_t n = 0;
    if (src) {
        for (; src[n] && n + 1 < size; n++) {
            unsigned char c = (unsigned char)src[n];
            out[n] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
        }
    }
    out[n] = '\0';
    tags_utf8_trim_partial(out);
}

bool agenda_empty(const agenda_t *a)
{
    return !a || (!a->has_events && a->count <= 0 && !a->temp[0] && !a->cond[0]);
}

// Home Assistant weather states to Font Awesome 5 Solid glyphs. The glyphs
// live in fonts/bugne_icons_28.c and _20.c: a new entry here needs its code
// point added there.
static const struct { const char *cond; const char *glyph; } ICONS[] = {
    { "sunny",           "\xEF\x86\x85" },  // U+F185 sun
    { "clear-night",     "\xEF\x86\x86" },  // U+F186 moon
    { "partlycloudy",    "\xEF\x9B\x84" },  // U+F6C4 cloud-sun
    { "cloudy",          "\xEF\x83\x82" },  // U+F0C2 cloud
    { "fog",             "\xEF\x9D\x9F" },  // U+F75F smog
    { "rainy",           "\xEF\x9C\xBD" },  // U+F73D cloud-rain
    { "pouring",         "\xEF\x9D\x80" },  // U+F740 cloud-showers-heavy
    { "hail",            "\xEF\x9D\x80" },
    { "snowy",           "\xEF\x8B\x9C" },  // U+F2DC snowflake
    { "snowy-rainy",     "\xEF\x8B\x9C" },
    { "lightning",       "\xEF\x83\xA7" },  // U+F0E7 bolt
    { "lightning-rainy", "\xEF\x83\xA7" },
    { "windy",           "\xEF\x9C\xAE" },  // U+F72E wind
    { "windy-variant",   "\xEF\x9C\xAE" },
};

const char *agenda_cond_icon(const char *cond)
{
    if (!cond) return NULL;
    for (size_t i = 0; i < sizeof(ICONS) / sizeof(ICONS[0]); i++) {
        if (strcmp(cond, ICONS[i].cond) == 0) return ICONS[i].glyph;
    }
    return NULL;
}

bool agenda_fresh(bool have, int64_t rx_s, int64_t now_s)
{
    return have && now_s >= rx_s && now_s - rx_s < AGENDA_FRESH_S;
}

// Full weekday (Sunday first, as tm_wday) and month names, per UI language.
static const char *const DAY_EN[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
                                       "Friday", "Saturday" };
static const char *const DAY_FR[7] = { "dimanche", "lundi", "mardi", "mercredi", "jeudi",
                                       "vendredi", "samedi" };
static const char *const DAY_DE[7] = { "Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag",
                                       "Freitag", "Samstag" };
static const char *const MONTH_EN[12] = { "January", "February", "March", "April", "May", "June",
                                          "July", "August", "September", "October", "November",
                                          "December" };
static const char *const MONTH_FR[12] = { "janvier", "f\xC3\xA9vrier", "mars", "avril", "mai",
                                          "juin", "juillet", "ao\xC3\xBBt", "septembre",
                                          "octobre", "novembre", "d\xC3\xA9" "cembre" };
static const char *const MONTH_DE[12] = { "Januar", "Februar", "M\xC3\xA4rz", "April", "Mai",
                                          "Juni", "Juli", "August", "September", "Oktober",
                                          "November", "Dezember" };

// Short names for the home header. The month abbreviations are the same as
// epmeta.c's private tables, repeated here so the two pure modules stay
// independent.
static const char *const DAY_S_EN[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const DAY_S_FR[7] = { "dim.", "lun.", "mar.", "mer.", "jeu.", "ven.", "sam." };
static const char *const DAY_S_DE[7] = { "So.", "Mo.", "Di.", "Mi.", "Do.", "Fr.", "Sa." };
static const char *const MONTH_S_EN[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *const MONTH_S_FR[12] = { "janv.", "f\xC3\xA9vr.", "mars", "avr.", "mai", "juin",
                                            "juil.", "ao\xC3\xBBt", "sept.", "oct.", "nov.",
                                            "d\xC3\xA9" "c." };
static const char *const MONTH_S_DE[12] = { "Jan.", "Feb.", "M\xC3\xA4rz", "Apr.", "Mai", "Juni",
                                            "Juli", "Aug.", "Sept.", "Okt.", "Nov.", "Dez." };

// One date line from the tables of the three languages (en, fr, de).
static void date_fmt(char *out, size_t n, int wday, int mday, int mon, const char *lang,
                     const char *const *const days[3], const char *const *const months[3])
{
    if (!out || n == 0) return;
    out[0] = '\0';
    if (wday < 0 || wday > 6 || mon < 0 || mon > 11 || mday < 1 || mday > 31) return;
    if (lang && strcmp(lang, "fr") == 0) {
        snprintf(out, n, "%s %d %s", days[1][wday], mday, months[1][mon]);
    } else if (lang && strcmp(lang, "de") == 0) {
        snprintf(out, n, "%s, %d. %s", days[2][wday], mday, months[2][mon]);
    } else {
        snprintf(out, n, "%s %d %s", days[0][wday], mday, months[0][mon]);
    }
    tags_utf8_trim_partial(out);  // a truncation must not end mid-character
}

void agenda_date_fmt(char *out, size_t n, int wday, int mday, int mon, const char *lang)
{
    static const char *const *const days[3] = { DAY_EN, DAY_FR, DAY_DE };
    static const char *const *const months[3] = { MONTH_EN, MONTH_FR, MONTH_DE };
    date_fmt(out, n, wday, mday, mon, lang, days, months);
}

void agenda_date_fmt_short(char *out, size_t n, int wday, int mday, int mon, const char *lang)
{
    static const char *const *const days[3] = { DAY_S_EN, DAY_S_FR, DAY_S_DE };
    static const char *const *const months[3] = { MONTH_S_EN, MONTH_S_FR, MONTH_S_DE };
    date_fmt(out, n, wday, mday, mon, lang, days, months);
}
