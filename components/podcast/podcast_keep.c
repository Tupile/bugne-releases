#include "podcast_keep.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *const MON[12] = { "January", "February", "March", "April", "May", "June",
                                     "July", "August", "September", "October", "November",
                                     "December" };

// Month 0..11 of a name of `len` letters: any prefix of the English name from
// 3 letters up ("Sep", "Sept", "September"). -1 otherwise.
static int month_index(const char *name, size_t len)
{
    if (len < 3) return -1;
    for (int i = 0; i < 12; i++) {
        if (len <= strlen(MON[i]) && strncasecmp(name, MON[i], len) == 0) return i;
    }
    return -1;
}

static const char *skip_space(const char *s)
{
    while (isspace((unsigned char)*s)) s++;
    return s;
}

bool podcast_date_parse(const char *s, int *day, int *mon, int *year)
{
    if (!s) return false;
    s = skip_space(s);
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
    s = skip_space(s);

    int d = 0, digits = 0;
    while (isdigit((unsigned char)*s) && digits < 2) { d = d * 10 + (*s - '0'); s++; digits++; }
    if (digits == 0 || isdigit((unsigned char)*s) || d < 1 || d > 31) return false;
    s = skip_space(s);

    const char *name = s;
    while (isalpha((unsigned char)*s)) s++;
    int m = month_index(name, (size_t)(s - name));
    if (m < 0) return false;
    if (*s == '.') s++;
    s = skip_space(s);

    if (!isdigit((unsigned char)*s)) return false;
    char *end;
    long y = strtol(s, &end, 10);
    if (end - s == 2) y += 2000;        // RFC 822 allows a 2-digit year
    else if (end - s != 4) return false;
    *day = d; *mon = m; *year = (int)y;
    return true;
}

int podcast_date_key(const char *date)
{
    int d, m, y;
    if (!podcast_date_parse(date, &d, &m, &y)) return 0;
    return y * 10000 + (m + 1) * 100 + d;
}

bool podcast_keep_in_window(const int *key, size_t n, int keep, size_t i)
{
    if (keep <= 0) return true;
    if (i >= n) return false;
    bool dated = key != NULL;
    for (size_t j = 0; dated && j < n; j++) {
        if (key[j] <= 0) dated = false;
    }
    if (!dated) return i < (size_t)keep;
    size_t before = 0;  // entries that rank ahead of i
    for (size_t j = 0; j < n; j++) {
        if (key[j] > key[i] || (key[j] == key[i] && j < i)) {
            if (++before >= (size_t)keep) return false;
        }
    }
    return true;
}

size_t podcast_keep_select(const bool *cached, const bool *protect, const int *key,
                           size_t n, int keep, bool *del)
{
    size_t marked = 0;
    for (size_t i = 0; i < n; i++) {
        del[i] = keep > 0 && cached[i] && !(protect && protect[i]) &&
                 !podcast_keep_in_window(key, n, keep, i);
        if (del[i]) marked++;
    }
    return marked;
}
