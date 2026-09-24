// Episode meta line ("1 Sep · 12 min"), see epmeta.h. Pure and host-tested.
#include "epmeta.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "tags.h"  // tags_utf8_trim_partial

static const char *const MON_EN[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *const MON_FR[12] = { "janv.", "f\xC3\xA9vr.", "mars", "avr.", "mai", "juin",
                                        "juil.", "ao\xC3\xBBt", "sept.", "oct.", "nov.", "d\xC3\xA9" "c." };

// RFC 822 "[Day, ]DD Mon YYYY ..." or ISO "YYYY-MM-DD...". Month is 0..11.
static bool parse_date(const char *s, int *day, int *mon, int *year)
{
    if (!s) return false;
    while (*s == ' ') s++;
    if (isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1]) &&
        isdigit((unsigned char)s[2]) && isdigit((unsigned char)s[3]) && s[4] == '-') {
        int y, m, d;
        if (sscanf(s, "%4d-%2d-%2d", &y, &m, &d) != 3) return false;
        if (m < 1 || m > 12 || d < 1 || d > 31) return false;
        *year = y; *mon = m - 1; *day = d;
        return true;
    }
    const char *comma = strchr(s, ',');
    if (comma && comma - s <= 9) s = comma + 1;  // skip the weekday
    while (*s == ' ') s++;
    int d = 0, y = 0;
    char m[4] = "";
    if (sscanf(s, "%2d %3s %4d", &d, m, &y) != 3) return false;
    if (d < 1 || d > 31) return false;
    for (int i = 0; i < 12; i++) {
        if (strcasecmp(m, MON_EN[i]) == 0) {
            *day = d; *mon = i; *year = y;
            return true;
        }
    }
    return false;
}

void ep_meta_fmt(char *out, size_t n, const char *pubdate, int dur_s, bool fr, int cur_year)
{
    if (!out || n == 0) return;
    char date[32] = "", dur[16] = "";
    int d, m, y;
    if (parse_date(pubdate, &d, &m, &y)) {
        const char *mn = fr ? MON_FR[m] : MON_EN[m];
        if (cur_year > 0 && y != cur_year) snprintf(date, sizeof(date), "%d %s %d", d, mn, y);
        else snprintf(date, sizeof(date), "%d %s", d, mn);
    }
    if (dur_s > 0) {
        int min = (dur_s + 59) / 60;  // under a minute still reads "1 min"
        if (min >= 60) snprintf(dur, sizeof(dur), "%d h %02d", min / 60, min % 60);
        else snprintf(dur, sizeof(dur), "%d min", min);
    }
    snprintf(out, n, "%s%s%s", date, (date[0] && dur[0]) ? " \xC2\xB7 " : "", dur);
    tags_utf8_trim_partial(out);  // a truncation must not end mid-character
    // A cut right after the separator's space (or inside it) leaves a dangling
    // " ·" tail: drop trailing spaces and middle dots.
    size_t len = strlen(out);
    while (len && (out[len - 1] == ' ' ||
                   (len >= 2 && (unsigned char)out[len - 2] == 0xC2 && (unsigned char)out[len - 1] == 0xB7))) {
        if (out[len - 1] == ' ') len--;
        else len -= 2;
        out[len] = '\0';
    }
}
