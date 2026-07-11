// source_sd: local file playback from the SD card.
//
// Mounts the SD (SDIO 4-bit) at /sdcard and plays MP3/FLAC files by feeding
// their bytes to the decoder. The format is chosen from the file extension.
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

// Mount the SD card. Always returns ESP_OK so boot continues without a card;
// use source_sd_present() to check availability.
esp_err_t source_sd_init(void);

// Whether the SD card is mounted.
bool source_sd_present(void);

// Card capacity and free space in bytes. Returns false (and leaves the outputs
// untouched) if no card is mounted or the query fails. Either pointer may be NULL.
bool source_sd_usage(uint64_t *total_bytes, uint64_t *free_bytes);

// Play a file (blocking until it finishes or errors). path is absolute, for
// example "/sdcard/music/song.mp3". Acquires the audio arbiter for the duration.
esp_err_t source_sd_play(const char *path);

// Ask the current file playback to stop. The play call then returns.
void source_sd_stop(void);

// Whether the last source_sd_play() ended on its own (decoded to the end), as
// opposed to being stopped by source_sd_stop() or failing. Used to auto-advance
// to the next track in the folder.
bool source_sd_completed(void);

#define SOURCE_SD_NAME_MAX 64

// List playable files (mp3/flac) directly in dir. Fills names (up to max) and
// sets *count. Names are bare file names, not full paths.
esp_err_t source_sd_list(const char *dir, char names[][SOURCE_SD_NAME_MAX], size_t max, size_t *count);

// ---- File-manager API for the web SD browser (#29) ----
// All paths below are RELATIVE to the SD root ("" = root, "Album/Disc 1"). They
// are validated: a path containing ".." or a leading "/" is rejected.

typedef struct {
    char     name[SOURCE_SD_NAME_MAX];
    bool     is_dir;
    uint32_t size;   // bytes, 0 for directories
} source_sd_entry_t;

// List every entry (files and subdirectories) in rel_dir. Fills out (up to max)
// and sets *count. ESP_ERR_INVALID_STATE if no card, ESP_ERR_INVALID_ARG on a
// bad path, ESP_ERR_NOT_FOUND if the directory cannot be opened. with_sizes
// stat()s every file to fill entry.size: one SD transaction per file, slow on
// large folders, so pass false when sizes are not displayed (sizes stay 0).
esp_err_t source_sd_browse(const char *rel_dir, source_sd_entry_t *out, size_t max, size_t *count,
                           bool with_sizes);

// Open rel_path for writing (truncating), creating any missing parent
// directories. Returns NULL on a bad path, no card, or error. Caller fcloses.
FILE *source_sd_create(const char *rel_path);

// Create directory rel_path (and parents). ESP_OK if it now exists.
esp_err_t source_sd_mkdir(const char *rel_path);

// Delete rel_path: a file, or a directory and all its contents (recursive).
// Refuses the SD root. ESP_ERR_INVALID_ARG on a bad/empty path.
esp_err_t source_sd_delete(const char *rel_path);
