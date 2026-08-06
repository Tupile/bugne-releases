// art: cover art for the current track, decoded to a small RGB565 bitmap.
//
// Producers (the audio decoders' tag callbacks, and the UI worker for podcast
// covers) hand raw JPEG bytes to art_set_jpeg(); it decodes them, scaled down
// while decoding, into a PSRAM bitmap. The UI then TAKES that bitmap and owns
// it from then on.
//
// OWNERSHIP RULE: art_take() transfers the buffer to the caller and empties
// the slot. That is what keeps this lock-light: the producer never frees a
// bitmap the UI is drawing, because once taken the slot no longer points at
// it. A bitmap that is replaced before anyone took it is freed here.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Longest side of the stored bitmap. The cover shows in landscape only, in
// the band between the back button (bottom edge 52) and the transport row,
// whose tallest button is the 60 px pause at y 170: 118 px of room, so 112
// leaves a few pixels of air on both sides. Covers land on exactly this size
// (see the resample note in art.c), so the layout reserves a fixed slot
// instead of adapting to each file.
#define ART_BOX 112

// Biggest JPEG we accept. Embedded covers are typically 20-200 KB; anything
// beyond this is refused rather than copied into PSRAM.
#define ART_MAX_JPEG (512 * 1024)

// Decode and store. Returns false (and keeps no art) when the data is not a
// JPEG, is larger than ART_MAX_JPEG, would not fit in memory, or is so large
// that even 1/8 scale overshoots ART_BOX. Safe to call from any task.
bool art_set_jpeg(const uint8_t *jpeg, size_t len);

// Drop any stored art. Called on every track change.
void art_clear(void);

// Generation counter, bumped by art_set_jpeg and art_clear. The UI polls it
// to notice a change without holding a pointer.
uint32_t art_gen(void);

// Take ownership of the stored bitmap (RGB565, w*h*2 bytes). Returns false
// when there is none. On success the caller must heap_caps_free(*px) when it
// is done, and the slot is left empty.
bool art_take(uint8_t **px, uint16_t *w, uint16_t *h);
