// usage: pure accumulator for the parental daily usage limit. See usage.h.
#include "usage.h"

#define USAGE_SAVE_INTERVAL 60  // ticks (~1 Hz) between persist signals: 1 min

static int s_date;              // yyyymmdd being accumulated, 0 = none yet
static int s_seconds;           // usage seconds on s_date
static int s_ticks_since_save;  // ticks since the last persist signal

void usage_seed(int date, int seconds)
{
    if (date <= 0) return;
    s_date = date;
    s_seconds = seconds < 0 ? 0 : seconds;
    s_ticks_since_save = 0;
}

bool usage_tick(int date)
{
    if (date <= 0) return false;  // no valid clock: drop the tick

    bool rolled = false;
    if (date != s_date) {  // midnight rollover (or first tick): fresh day
        s_date = date;
        s_seconds = 0;
        s_ticks_since_save = 0;
        rolled = true;
    }
    s_seconds++;

    if (++s_ticks_since_save >= USAGE_SAVE_INTERVAL) {
        s_ticks_since_save = 0;
        return true;
    }
    return rolled;  // persist the reset promptly on a day change
}

int usage_today(int date)
{
    return (date > 0 && date == s_date) ? s_seconds : 0;
}

int usage_date(void)    { return s_date; }
int usage_seconds(void) { return s_seconds; }
