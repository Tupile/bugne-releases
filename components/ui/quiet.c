// quiet: pure predicate for the parental "quiet hours" window. See quiet.h for
// the semantics (half-open [start, end); day bit = start day; start==end off).
#include "quiet.h"

bool quiet_window_hit(int enabled, int days, int start_min, int end_min,
                      int now_min, int today)
{
    if (!enabled) return false;
    if (start_min == end_min) return false;  // off

    if (start_min < end_min) {
        // Same-day window: today's bit gates it.
        if (!(days & (1 << today))) return false;
        return now_min >= start_min && now_min < end_min;
    }

    // Midnight wrap (start > end): the day bit is the day the window starts.
    if (now_min >= start_min) {
        // Evening part: today's bit.
        return (days & (1 << today)) != 0;
    }
    if (now_min < end_min) {
        // Post-midnight part: yesterday's bit.
        int yesterday = (today + 6) % 7;
        return (days & (1 << yesterday)) != 0;
    }
    return false;
}
