#include "sleep_fade.h"

int sleep_fade_vol(int start, int64_t remaining_ms, int64_t fade_ms)
{
    if (start <= 0) return 0;
    if (fade_ms <= 0 || remaining_ms >= fade_ms) return start;
    if (remaining_ms <= 0) return 0;
    return (int)((start * remaining_ms + fade_ms / 2) / fade_ms);
}
