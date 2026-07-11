// library: a tag-based music index of the SD card, browsable by artist/album.
//
// A manual scan walks the SD card, reads each MP3/FLAC file's tags, and writes
// an index file to the card (/sdcard/.bugne_library.tsv). The index is loaded
// into PSRAM for browsing. Untagged files fall under "Unknown artist/album" with
// the file name as the title.
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#define LIB_NAME_MAX 64    // artist / album / title
#define LIB_PATH_MAX 256   // track path, relative to the SD root

// Scan the SD card, build the index, write it to the card, and load it into RAM.
// Blocking (run on a worker, not the UI/HTTP task). ESP_ERR_INVALID_STATE if no
// card. Refuses to run if a scan is already in progress.
esp_err_t library_scan(void);

// Like library_scan but abortable: *cancel is polled during the walk. If it goes
// true the scan stops, the on-disk index is left unchanged (the previous one is
// reloaded into RAM), and ESP_ERR_INVALID_STATE is returned. cancel may be NULL.
esp_err_t library_scan_cancelable(volatile bool *cancel);

// Start a scan on a background task (for the web / device buttons). Returns false
// if a scan is already running or there is no SD card.
bool library_scan_start(void);

// True while a scan is running.
bool library_scanning(void);

// Load the index from the SD card into RAM. ESP_ERR_NOT_FOUND if none exists.
esp_err_t library_load(void);

// Number of tracks in the loaded index.
size_t library_track_count(void);

// Browse. Each fills the caller's array (up to max) and returns the count.
size_t library_artists(char out[][LIB_NAME_MAX], size_t max);
size_t library_albums(const char *artist, char out[][LIB_NAME_MAX], size_t max);
// Every album across the library as parallel (album, artist) arrays, sorted by
// album name (albums belong to an artist, so the artist is needed to list tracks).
size_t library_all_albums(char albums[][LIB_NAME_MAX], char artists[][LIB_NAME_MAX], size_t max);
// Album tracks ordered by track number then title: titles for display, paths
// (relative to the SD root) for playback.
size_t library_album_tracks(const char *artist, const char *album,
                            char titles[][LIB_NAME_MAX], char paths[][LIB_PATH_MAX], size_t max);
