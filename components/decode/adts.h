// adts: ADTS (raw AAC) framing helpers. Pure C, host-tested.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Offset of the next ADTS sync word (0xFFF, layer 0) strictly after buf[0], so
// a decoder that rejected the frame at buf[0] can skip it. A trailing 0xFF that
// may start a sync split across reads is kept (its offset is returned). Returns
// len when no candidate is found: the whole buffer can be dropped.
size_t adts_resync(const uint8_t *buf, size_t len);
