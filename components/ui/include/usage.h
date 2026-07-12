// usage: pure accumulator for the parental daily usage limit.
//
// Counts seconds of "usage" (audible listening or game screen time, the caller
// decides what counts) for one day at a time. Dependency-free (only stdbool)
// so it can be unit-tested on a host, like quiet.c. Dates are yyyymmdd local
// days; a date <= 0 (no valid clock yet) drops the tick. A new date resets the
// counter to zero (midnight rollover). Persistence is the caller's job: the
// ui component seeds from NVS at boot and writes back on the usage_tick
// signal, so a power cycle loses at most the last unsaved minute.
#pragma once

#include <stdbool.h>

// Seed the accumulator from persisted state (boot). A date <= 0 is ignored.
void usage_seed(int date, int seconds);

// Accumulate one second of usage on `date`. Returns true when the caller
// should persist (every USAGE_SAVE_INTERVAL ticks, and on a day rollover so
// the reset lands promptly).
bool usage_tick(int date);

// Seconds accumulated for `date`; 0 when `date` is not the tracked day (so a
// query right after midnight reads 0 without waiting for the first tick).
int usage_today(int date);

// The tracked day and its seconds, for persisting outside a tick (e.g. on a
// play -> idle edge). usage_date() is 0 before the first seed/tick.
int usage_date(void);
int usage_seconds(void);
