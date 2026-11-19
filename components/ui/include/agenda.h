// agenda: the data behind the agenda + weather screen. Home Assistant pushes
// ready-to-display text through POST /api/agenda; the device only shows it.
// Pure (no ESP, no LVGL), host-tested in test/host/test_agenda.c.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENDA_EVENTS_MAX 3
#define AGENDA_TEMP_MAX   16
#define AGENDA_COND_MAX   24
#define AGENDA_WHEN_MAX   32
#define AGENDA_WHAT_MAX   64

// Data older than this is hidden: a stale agenda is worse than none.
#define AGENDA_FRESH_S (2 * 60 * 60)

typedef struct {
    char when[AGENDA_WHEN_MAX];
    char what[AGENDA_WHAT_MAX];
} agenda_event_t;

typedef struct {
    char temp[AGENDA_TEMP_MAX];   // current temperature, "" if unknown
    char tmin[AGENDA_TEMP_MAX];   // today's minimum, "" if unknown
    char tmax[AGENDA_TEMP_MAX];   // today's maximum, "" if unknown
    char cond[AGENDA_COND_MAX];   // raw Home Assistant weather state
    bool rain;                    // rain forecast within the hour
    bool has_events;              // an event list was sent, even an empty one
    int count;                    // events used, 0..AGENDA_EVENTS_MAX
    agenda_event_t ev[AGENDA_EVENTS_MAX];
} agenda_t;

// Copy untrusted UTF-8 into a fixed buffer: bounded, control characters
// become spaces, a multi-byte sequence cut by the bound is dropped. A NULL
// source gives an empty string.
void agenda_copy_field(char *out, size_t size, const char *src);

// True when the structure holds nothing to show. A sent but empty event
// list is content: the screen then says that nothing is planned.
bool agenda_empty(const agenda_t *a);

// Weather pictogram (UTF-8 string of one icon-font glyph) for a Home
// Assistant weather state. NULL for an unknown state or "exceptional".
const char *agenda_cond_icon(const char *cond);

// Today's date line for the agenda screen, from the device's own clock:
// "Thursday 19 November", "jeudi 19 novembre", "Donnerstag, 19. November".
// wday (0 = Sunday), mday (1..31) and mon (0..11) as in struct tm; lang is
// the UI language code ("fr", "de", anything else is English). Out-of-range
// input gives an empty string.
void agenda_date_fmt(char *out, size_t n, int wday, int mday, int mon, const char *lang);

// Short form of the same date, for the home header: "Thu 19 Nov",
// "jeu. 19 nov.", "Do., 19. Nov.". Same arguments and same contract.
void agenda_date_fmt_short(char *out, size_t n, int wday, int mday, int mon, const char *lang);

// True while data received at `rx_s` is still fresh at `now_s` (monotonic
// seconds). `have` false means nothing was ever received.
bool agenda_fresh(bool have, int64_t rx_s, int64_t now_s);
