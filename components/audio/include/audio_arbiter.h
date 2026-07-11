// audio_arbiter: only one audio source is active at a time.
//
// Sources acquire the arbiter before opening the output and release it after.
// The "syncing" state lets a source suspend playback while RSS refresh or a
// cache download runs, so decoding and network bulk transfer never overlap.
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_SOURCE_NONE = 0,
    AUDIO_SOURCE_SD,
    AUDIO_SOURCE_STREAM,
    AUDIO_SOURCE_SENDSPIN,
    AUDIO_SOURCE_BEEP,      // alarm beep fallback (generated tone, no decoder)
    AUDIO_SOURCE_TUNER,     // instrument tuner: mic capture only, no output
} audio_source_t;

// Create the arbiter state. Called by audio_init().
esp_err_t audio_arbiter_init(void);

// Become the active source. Fails with ESP_ERR_INVALID_STATE if another source
// already holds it. Re-acquiring as the current holder succeeds.
esp_err_t audio_arbiter_acquire(audio_source_t src);

// Release the arbiter if src is the active source. No-op otherwise.
void audio_arbiter_release(audio_source_t src);

// The currently active source, or AUDIO_SOURCE_NONE.
audio_source_t audio_arbiter_active(void);

// Suspend or resume playback for syncing work. While syncing, sources must not
// decode or write audio.
void audio_arbiter_set_syncing(bool syncing);
bool audio_arbiter_is_syncing(void);

#ifdef __cplusplus
}
#endif
