#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "epmeta.h"

static char out[64];

static void check(const char *date, int dur, int fr, int year, const char *want)
{
    ep_meta_fmt(out, sizeof(out), date, dur, fr, year);
    if (strcmp(out, want) != 0) {
        printf("FAIL: (%s, %d, %d, %d) -> \"%s\", want \"%s\"\n",
               date ? date : "NULL", dur, fr, year, out, want);
        assert(0);
    }
}

int main(void)
{
    const char *d = "Mon, 01 Sep 2026 08:00:00 GMT";
    check(d, 720, 0, 2026, "1 Sep \xC2\xB7 12 min");
    check(d, 720, 1, 2026, "1 sept. \xC2\xB7 12 min");
    check(d, 3900, 0, 2026, "1 Sep \xC2\xB7 1 h 05");
    check(d, 30, 0, 2026, "1 Sep \xC2\xB7 1 min");          // under a minute rounds up
    check(d, 0, 1, 2026, "1 sept.");                         // no duration
    check(NULL, 720, 0, 2026, "12 min");                     // no date
    check("garbage", 720, 1, 2026, "12 min");                // unparseable date
    check(NULL, 0, 0, 2026, "");                             // nothing known
    check(d, 720, 0, 2024, "1 Sep 2026 \xC2\xB7 12 min");    // other year: shown
    check(d, 720, 0, 0, "1 Sep \xC2\xB7 12 min");            // unknown year: hidden
    check("Tue, 5 Aug 2025 10:00:00 +0200", 60, 1, 2026, "5 ao\xC3\xBBt 2025 \xC2\xB7 1 min");
    check("01 Dec 2026 08:00:00 GMT", 0, 0, 2026, "1 Dec");  // no weekday
    check("2026-02-14T08:00:00Z", 0, 1, 2026, "14 f\xC3\xA9vr.");  // ISO
    check("Mon, 32 Sep 2026", 0, 0, 2026, "");               // day out of range
    check("Mon, 01 Foo 2026", 0, 0, 2026, "");               // unknown month
    // Truncation stays NUL-terminated and never splits a UTF-8 sequence.
    char small[8];
    ep_meta_fmt(small, sizeof(small), d, 720, 1, 2026);
    assert(strlen(small) < sizeof(small));
    assert(strcmp(small, "1 sept.") == 0);
    char tiny[1];
    ep_meta_fmt(tiny, sizeof(tiny), d, 720, 0, 2026);
    assert(tiny[0] == '\0');
    puts("OK: all epmeta host tests passed");
    return 0;
}
