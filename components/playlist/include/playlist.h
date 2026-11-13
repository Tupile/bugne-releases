// Playlists: one extended M3U file per playlist in /sdcard/playlists. The
// playlist name is the file name without ".m3u". The file format lives in
// playlist_m3u.h; this is the SD layer.
//
// Threading: the web routes (httpd task) write, the device screens (LVGL task)
// read. ONE mutex is held for each whole operation, so a reader never sees the
// window between the remove and the rename of a write. wait_ms is how long the
// caller accepts to wait for that mutex: keep it short on the LVGL task.
//
// FAT is case-insensitive: "Rock" and "rock" name the same playlist.
//
// Every function returns ESP_ERR_INVALID_STATE without a mounted card,
// ESP_ERR_INVALID_ARG on a bad name and ESP_ERR_TIMEOUT when the mutex was not
// obtained in time.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "playlist_m3u.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLAYLIST_DIR      "playlists"            // relative to the SD root
#ifndef PLAYLIST_ABS_DIR                         // host tests redirect to /tmp
#define PLAYLIST_ABS_DIR  "/sdcard/" PLAYLIST_DIR
#endif

// Create the mutex. Call once from app_main, before any task can use the
// module (two tasks could otherwise be the first caller).
void playlist_init(void);

// Playlist names in name order (case-insensitive), at most max. The files are
// not opened. A missing folder is an empty list.
esp_err_t playlist_list(char (*names)[PLAYLIST_NAME_MAX], size_t max, size_t *count,
                        uint32_t wait_ms);

// Read one playlist. ESP_ERR_NOT_FOUND when it does not exist. *truncated is
// set when the file holds more valid entries than max.
esp_err_t playlist_read(const char *name, char (*titles)[PLAYLIST_TITLE_MAX],
                        char (*paths)[PLAYLIST_PATH_MAX], size_t max, size_t *count,
                        bool *truncated, uint32_t wait_ms);

// Replace (or create) one playlist. ESP_ERR_INVALID_ARG above
// PLAYLIST_MAX_TRACKS or on an unsafe path, ESP_ERR_NO_MEM when it would be
// playlist number PLAYLIST_MAX_LISTS + 1, ESP_FAIL on a write error. On any
// failure the previous file is untouched.
esp_err_t playlist_write(const char *name, char (*titles)[PLAYLIST_TITLE_MAX],
                         char (*paths)[PLAYLIST_PATH_MAX], size_t count, uint32_t wait_ms);

// Rename. ESP_ERR_NOT_FOUND when from does not exist, ESP_ERR_NOT_ALLOWED when
// to names another existing playlist. A change of case only is allowed.
esp_err_t playlist_rename(const char *from, const char *to, uint32_t wait_ms);

// Delete. ESP_ERR_NOT_FOUND when it does not exist.
esp_err_t playlist_delete(const char *name, uint32_t wait_ms);

#ifdef __cplusplus
}
#endif
