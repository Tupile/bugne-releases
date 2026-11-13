// agenda: see agenda.h. Pure, host-tested (test/host/test_agenda.c).
#include "agenda.h"

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
