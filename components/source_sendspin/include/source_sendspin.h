// source_sendspin: Sendspin player for Music Assistant.
//
// Wraps the standalone sendspin-cpp component in the player role, pinned to a
// fixed version. It decodes itself and writes PCM to the shared audio output
// via a callback. Advertised over mDNS as _sendspin._tcp so Music Assistant
// discovers it. No Home Assistant or MQTT integration. The wrapper is C++ but
// exposes a C entry point.
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Transport commands the UI can send to Music Assistant (via the controller role).
typedef enum {
    SENDSPIN_CMD_PLAY = 0,
    SENDSPIN_CMD_PAUSE,
    SENDSPIN_CMD_STOP,
    SENDSPIN_CMD_NEXT,
    SENDSPIN_CMD_PREVIOUS,
} sendspin_cmd_t;

// Start the Sendspin player: build the client, add the player role, start the
// server, advertise mDNS, and run the protocol loop on its own task.
esp_err_t source_sendspin_init(void);

// True while Music Assistant is streaming audio to this player (playing, not
// paused). Drives the pause/play button state.
bool source_sendspin_active(void);

// True while Music Assistant is engaged with this player. Stays true across a
// pause (when MA ends the audio stream) and goes false on stop or disconnect.
// The UI uses this to open and close the now-playing screen.
bool source_sendspin_session_active(void);

// Copy the current track title / artist into buf (empty if none). Returns length.
size_t source_sendspin_title(char *buf, size_t size);
size_t source_sendspin_artist(char *buf, size_t size);

// Current playback position and track duration in milliseconds (0 if unknown).
void source_sendspin_progress(uint32_t *pos_ms, uint32_t *dur_ms);

// Queue a transport command for Music Assistant. Sent from the sendspin task on
// its next loop, so it is safe to call from the UI task.
void source_sendspin_command(sendspin_cmd_t cmd);

#ifdef __cplusplus
}
#endif
