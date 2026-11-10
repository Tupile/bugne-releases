#pragma once

#include <stdbool.h>
#include <stdint.h>

// Should the daily firmware update check be queued now? All four readiness
// inputs must hold: the check function is wired (web_config is up), Wi-Fi is
// connected, audio has been idle long enough, and nothing else uses the
// device. A check that never ran since boot is due whatever `last_us` holds
// (a dropped request leaves a stale timestamp behind); otherwise it is due
// once `interval_us` has elapsed since `last_us`. Pure, host-tested in
// test/host/test_updcheck.c.
bool update_check_due(bool have_fn, bool net_connected, bool idle,
                      bool device_free, bool checked_once,
                      int64_t now_us, int64_t last_us, int64_t interval_us);
