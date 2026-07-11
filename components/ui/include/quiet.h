// quiet: pure predicate for the parental "quiet hours" window.
//
// Dependency-free (only stdbool) so it can be unit-tested on a host. Inputs are
// minutes-since-local-midnight and a Monday-based weekday (0=Monday..6=Sunday).
// The window is half-open [start, end). The day bit is the day the window
// STARTS: for a midnight-wrap window (start > end) the evening part uses today's
// bit and the post-midnight part uses yesterday's bit. start == end means the
// window is off (returns false). enabled == 0 returns false.
#pragma once

#include <stdbool.h>

// days is a weekday bitmask, bit0 = Monday .. bit6 = Sunday.
bool quiet_window_hit(int enabled, int days, int start_min, int end_min,
                      int now_min, int today);
