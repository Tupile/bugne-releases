#pragma once
// Episode meta line for the episodes list: "1 Sep · 12 min" / "1 sept. · 12 min".
// Pure (no LVGL, no ESP-IDF), host-tested in test/host/test_epmeta.c.
#include <stdbool.h>
#include <stddef.h>

// pubdate: raw RSS pubDate (RFC 822, e.g. "Mon, 01 Sep 2026 08:00:00 GMT") or
// an ISO date ("2026-09-01..."); NULL or unparseable = no date part.
// dur_s: duration in seconds, <= 0 = no duration part.
// fr: French month names and spacing. cur_year: the year the date is compared
// with, the year is shown only when it differs (0 = unknown, never shown).
// Always NUL-terminates out (n > 0); an empty string when nothing is known.
void ep_meta_fmt(char *out, size_t n, const char *pubdate, int dur_s, bool fr, int cur_year);
