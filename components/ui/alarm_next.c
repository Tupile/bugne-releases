// alarm_next: pure predicate, see alarm_next.h for the semantics.
#include "alarm_next.h"

int alarm_next_fire(const config_alarm_t *alarms, int count, const struct tm *now, int *minutes_until)
{
    if (!alarms || !now || count <= 0) return -1;

    int today = (now->tm_wday + 6) % 7;  // Monday-based, matches the days bitmask
    int now_min = now->tm_hour * 60 + now->tm_min;

    int best_idx = -1;
    int best_delta = 0;

    for (int i = 0; i < count; i++) {
        const config_alarm_t *a = &alarms[i];
        if (!a->enabled) continue;
        int target_min = a->hour * 60 + a->minute;
        // Exactly one of "today" (add_day 0) / "tomorrow" (add_day 1) lands the
        // target time within the next 24h; the day bitmask can still exclude it.
        for (int add_day = 0; add_day < 2; add_day++) {
            int day = (today + add_day) % 7;
            if (!(a->days & (1 << day))) continue;
            int delta = target_min - now_min + add_day * 1440;
            if (delta < 0 || delta >= 1440) continue;
            if (best_idx == -1 || delta < best_delta) {
                best_idx = i;
                best_delta = delta;
            }
            break;
        }
    }

    if (best_idx != -1 && minutes_until) *minutes_until = best_delta;
    return best_idx;
}
