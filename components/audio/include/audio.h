// audio: the one shared output layer.
//
// Exactly one owner of the I2S bus and the ES8311 codec. This layer also drives
// the amp enable (IO1, active low) directly as a GPIO. All sources route PCM
// through audio_write().
//
// The source arbiter lives alongside this layer, see audio_arbiter.h.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the I2S TX channel and ES8311 on the shared I2C bus, and start the
// arbiter. The amp stays muted until audio_open(). Call once at boot.
esp_err_t audio_init(i2c_master_bus_handle_t i2c_bus);

// Configure the output for a stream and unmute the amp. Reconfigures the I2S
// clock and slots for the given format. Call before audio_write().
esp_err_t audio_open(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels);

// Write interleaved PCM. Blocks until the bytes are queued to I2S.
esp_err_t audio_write(const void *pcm, size_t bytes);

// Stop the output and mute the amp.
esp_err_t audio_close(void);

// Mute the amp immediately (FM8002E enable, IO1). Used on Stop so playback goes
// silent at once instead of draining buffered audio (a source can hold seconds of
// PCM, e.g. the Sendspin buffer). The next audio_open() unmutes.
void audio_output_off(void);

// Set output volume, 0 to 100. Clamped to the volume limit.
esp_err_t audio_set_volume(int volume);

// Current output volume, 0 to 100.
int audio_get_volume(void);

// Volume ceiling (child-ear protection), 1 to 100. Every volume request is
// clamped to it, whatever the source (UI, web, Music Assistant). Lowering it
// below the current volume lowers the volume immediately. Set from the
// ui.volume_max config by the ui component; default 100 (no limit).
void audio_set_volume_limit(int limit);
int audio_get_volume_limit(void);

// Microphone capture (ES8311 ADC over the I2S RX channel), used by the
// instrument tuner. Mutually exclusive with playback: the caller must hold
// the audio arbiter and have stopped every source before opening. The amp is
// never unmuted on this path.
esp_err_t audio_record_open(uint32_t sample_rate);
esp_err_t audio_record_read(void *pcm, size_t bytes);
void audio_record_close(void);

// Pause or resume playback without closing the stream. While paused, audio_write
// blocks and the codec is fed silence. Cleared automatically on each audio_open.
void audio_set_paused(bool paused);
bool audio_is_paused(void);

// True between audio_open() and audio_close(), i.e. while a source is playing
// (or paused). The UI uses this to know something is playing.
bool audio_is_active(void);

#ifdef __cplusplus
}
#endif
