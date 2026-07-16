// ui: the LVGL touch interface.
//
// LVGL on the ILI9341V display and FT6336G touch, on its own task, talking to
// audio via events. Draws the two setup QR codes with qrcodegen (vendored).
// Screen sleep during playback: backlight off (IO45) plus display sleep, woken
// by the BOOT button (IO0) and touch INT (IO17). Audio keeps playing while the
// screen is off.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

// Start the UI task. Touch shares the same I2C bus as the codec, so the shared
// bus handle is passed in.
esp_err_t ui_start(i2c_master_bus_handle_t i2c_bus);

// ---- Remote playback control (for the web server) ----

// Playback commands sent from another task (the web server). The UI task applies
// the command on its next tick, so all LVGL/playback state stays single-threaded.
typedef enum {
    UI_REMOTE_TOGGLE,      // pause / resume
    UI_REMOTE_STOP,
    UI_REMOTE_NEXT,
    UI_REMOTE_PREV,
    UI_REMOTE_VOLUME,      // arg = 0..100
    UI_REMOTE_PLAY_RADIO,  // arg = web radio index in the config
    UI_REMOTE_REFRESH_PODCASTS,  // refresh every configured podcast feed
    UI_REMOTE_DOWNLOAD_PODCAST,    // arg = podcast id: background job, missing episodes
    UI_REMOTE_REDOWNLOAD_PODCAST,  // arg = podcast id: background job, re-download all (force)
    UI_REMOTE_DOWNLOAD_ALL,        // background job: refresh + download missing, every podcast
    UI_REMOTE_REDOWNLOAD_ALL,      // background job: re-download every episode of every podcast
    UI_REMOTE_CANCEL_DOWNLOAD,     // clear the download job (no auto-resume)
    UI_REMOTE_PLAY_PATH,           // play an SD path sent by the web page
    UI_REMOTE_SLEEP,               // sleep timer: arg = minutes, 0 = off, -1 = end-of-track
    UI_REMOTE_SEEK,                // arg = target position in ms (seekable file only)
} ui_remote_t;

// Queue a remote command. Thread-safe; returns immediately (applied within ~one
// UI tick). Only the latest pending command is kept.
void ui_remote(ui_remote_t cmd, int arg);

// Queue a request to play an SD music library track from the web page.
// path is relative to the SD root, as returned by /api/library. title is the
// display title (library tag); NULL or empty falls back to the file name.
void ui_remote_play_path(const char *rel_path, const char *title);

// ---- Remote debug helpers (web server) ----

// Take a screenshot of the current screen for GET /api/screenshot. Blocks until
// the LVGL task renders it (or timeout_ms elapses). On ESP_OK, *px points at a
// row-major top-down RGB565 buffer (stride = *w * 2), valid until
// ui_screenshot_release(). Returns ESP_ERR_TIMEOUT or ESP_ERR_NO_MEM otherwise.
// One client at a time; the caller MUST call ui_screenshot_release() afterwards,
// on success or failure.
esp_err_t ui_screenshot(const uint8_t **px, int *w, int *h, uint32_t timeout_ms);
void ui_screenshot_release(void);

// Navigate the UI to a named screen for POST /api/debug/nav. Validates the name
// synchronously; returns false on an unknown name (so the endpoint can answer
// 400), true when queued for the UI task.
bool ui_remote_nav(const char *screen);

// Snapshot of what is currently playing, for the web status view.
typedef struct {
    bool     active;       // something is playing (or paused)
    bool     paused;
    int      volume;       // 0..100
    char     source[12];   // "none","sd","radio","podcast","sendspin"
    char     title[64];
    char     artist[64];
    uint32_t pos_ms;       // 0 if unknown
    uint32_t dur_ms;       // 0 if unknown / live
    int      sleep_min;    // sleep timer (A2): 0 = off, -1 = end-of-track, else minutes left
    bool     seekable;     // the playing file's format honors the "seek" action
} ui_status_t;

// Fill a status snapshot. Thread-safe (a best-effort read of live state).
void ui_status(ui_status_t *out);

// True while a podcast refresh (single or refresh-all) is running on the worker.
// Lets the web page poll for completion.
bool ui_podcast_refreshing(void);

// Request a listening-stats reset (POST /api/stats/reset). Deferred to the UI
// task, which owns the stats RAM and file.
void ui_stats_reset(void);

// Background download job state for the web page to poll.
typedef enum {
    UI_DL_IDLE,         // no job
    UI_DL_SCHEDULED,    // job pending, waiting for the device to be idle 5 min / Wi-Fi
    UI_DL_PAUSED,       // job pending, paused because audio is playing
    UI_DL_REFRESHING,   // refreshing feeds before downloading
    UI_DL_DOWNLOADING,  // downloading episodes
    UI_DL_SCANNING,     // refreshing the SD music library index (end of auto-maintenance)
    UI_DL_SDFULL,       // stopped: SD card almost full, free space then retry
} ui_dl_phase_t;

typedef struct {
    bool          pending;  // a job exists (running, scheduled, or paused)
    bool          active;   // the worker is running right now (refresh or download)
    int           done;     // episodes downloaded so far this job
    int           total;    // episodes to download this job (best-effort)
    ui_dl_phase_t phase;
} ui_dl_status_t;

// Fill the download status. Thread-safe best-effort read. out may not be NULL.
void ui_download_status(ui_dl_status_t *out);

// ---- Daily GitHub firmware update check (A1) ----

// Signature matching web_config_gh_check. ui cannot REQUIRE web_config (that
// would be circular: web_config already depends on ui), so bugne_main wires
// this pointer once web_config_start() succeeds. The maintenance engine calls
// it at most once per 24 h; a NULL pointer (web_config not up) skips the
// scheduled check silently.
typedef esp_err_t (*ui_ghota_check_fn_t)(char *latest, size_t cap, bool *update);
void ui_set_ghota_check_fn(ui_ghota_check_fn_t fn);
