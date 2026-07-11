// web_config: the configuration web page.
//
// Served by esp_http_server with embedded HTML/CSS/JS (no CDN, the device may
// be offline). Reads and writes the same config.json. Lets the user manage
// Wi-Fi, web radios, podcast RSS URLs, and trigger a sync. Behind a login:
// password hashed on flash, session cookie. All download sizes are bounded.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Start the config web server. Called by net once the station is connected (or
// on the AP during provisioning).
esp_err_t web_config_start(void);

// Check the latest GitHub release's embedded version against the running one
// (reads only the image header, a few KB). On ESP_OK, *latest holds the
// release version (truncated to cap) and *update is true on a version
// mismatch. Returns an error (no release reachable, or the header read
// failed) without touching *latest/*update. Also refreshes the device-side
// cache served by GET /api/ghota/status, whatever the caller. Used by both
// the manual "check now" endpoint and the daily auto-check in ui.c.
esp_err_t web_config_gh_check(char *latest, size_t cap, bool *update);
