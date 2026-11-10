#include "updcheck.h"

bool update_check_due(bool have_fn, bool net_connected, bool idle,
                      bool device_free, bool checked_once,
                      int64_t now_us, int64_t last_us, int64_t interval_us)
{
    if (!have_fn || !net_connected || !idle || !device_free) return false;
    if (!checked_once) return true;
    return (now_us - last_us) >= interval_us;
}
