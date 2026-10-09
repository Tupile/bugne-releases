// adts: ADTS (raw AAC) framing helpers. Pure C, host-tested.
#include "adts.h"

static int is_sync(const uint8_t *p)
{
    return p[0] == 0xFF && (p[1] & 0xF6) == 0xF0;
}

size_t adts_resync(const uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return 0;
    for (size_t i = 1; i + 1 < len; i++) {
        if (is_sync(buf + i)) return i;
    }
    if (len >= 2 && buf[len - 1] == 0xFF) return len - 1;  // maybe a split sync
    return len;
}
