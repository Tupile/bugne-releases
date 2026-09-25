#pragma once

#include <stdint.h>

// Volume for the sleep-timer fade-out: linear from `start` down to 0 over the
// last `fade_ms` before the deadline, rounded to nearest. Outside the window
// (remaining_ms >= fade_ms, or fade_ms <= 0) returns `start`; at or past the
// deadline returns 0. Pure, host-tested in test/host/test_sleep_fade.c.
int sleep_fade_vol(int start, int64_t remaining_ms, int64_t fade_ms);
