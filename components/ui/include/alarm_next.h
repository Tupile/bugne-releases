// alarm_next: pure helper that finds the next alarm due to fire, used by the
// sunrise light feature (C1) to know when to start ramping the backlight.
// Dependency-free apart from config_store.h and <time.h>, so it can be
// host-tested (see test/host/test_alarm_next.c). No globals, no time() calls:
// the caller passes the current wall time.
#pragma once

#include <time.h>
#include "config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

// Among `count` alarms (normally CFG_MAX_ALARMS), find the one that will next
// reach its configured hour:minute on an enabled day, within the coming 24
// hours. An alarm whose time is exactly `now` counts as due with
// minutes_until = 0. On a tie (same minutes_until), the lowest index wins.
// Returns the winning index, or -1 if no alarm is enabled or none falls
// within the next 24 hours. *minutes_until (0..1439) is only written when an
// index is returned.
int alarm_next_fire(const config_alarm_t *alarms, int count, const struct tm *now, int *minutes_until);

#ifdef __cplusplus
}
#endif
