// Host tests for components/decode/adts.c.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "adts.h"

int main(void)
{
    // Next sync after the corrupt frame at offset 0.
    const uint8_t a[] = {0xFF, 0xF1, 0x50, 0x80, 0x12, 0x34, 0xFF, 0xF1, 0x50};
    assert(adts_resync(a, sizeof(a)) == 6);
    // MPEG-2 ADTS (0xFFF9) and protection-absent variants are syncs too.
    const uint8_t b[] = {0x00, 0x11, 0xFF, 0xF9, 0x00};
    assert(adts_resync(b, sizeof(b)) == 2);
    // A lone 0xFF followed by a non-sync byte is not a sync.
    const uint8_t c[] = {0xFF, 0xF1, 0xFF, 0x00, 0xFF, 0xE0, 0x01};
    assert(adts_resync(c, sizeof(c)) == sizeof(c));
    // Layer bits set (0xFFF3 / 0xFFF7): MPEG audio, not ADTS.
    const uint8_t d[] = {0x00, 0xFF, 0xF3, 0x00, 0xFF, 0xF7, 0x00};
    assert(adts_resync(d, sizeof(d)) == sizeof(d));
    // Trailing 0xFF kept: the sync may continue in the next read.
    const uint8_t e[] = {0xFF, 0xF1, 0x00, 0x00, 0xFF};
    assert(adts_resync(e, sizeof(e)) == 4);
    // Degenerate inputs.
    assert(adts_resync(NULL, 4) == 0);
    assert(adts_resync(a, 0) == 0);
    assert(adts_resync(a, 1) == 1);
    puts("adts: resync after corrupt frame, sync variants, split sync, bounds passed");
    return 0;
}
