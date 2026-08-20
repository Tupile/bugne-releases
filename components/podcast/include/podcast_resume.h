// podcast_resume: persistent playback resume positions for podcast episodes.
//
// Keeps a table of up to 32 entries tracking the last played position (pos_ms)
// and total duration (dur_ms) of podcast episodes (either cached files on SD
// or live stream URLs).
//
// LRU replacement is performed using a monotonic updated_at field when the
// table is full.
//
// Thread safety: podcast_resume functions are called only from the UI (LVGL)
// task. No lock is taken.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PODCAST_RESUME_CAP 32

typedef struct {
    char path[128];
    uint32_t pos_ms;
    uint32_t dur_ms;
    uint32_t updated_at;
} podcast_resume_entry_t;

// Initialize the resume table. Loads persisted positions from SD card.
void podcast_resume_init(void);

// Retrieve the saved playback position (in ms) for an episode.
// If found, returns pos_ms and (if dur_ms is non-NULL) writes total duration
// to *dur_ms. If not found or not initialized, returns 0.
uint32_t podcast_resume_get(const char *path_or_url, uint32_t *dur_ms);

// Save or update the playback position and total duration for an episode.
// If the table is full, evicts the least recently updated entry (LRU).
// Automatically persists to disk.
void podcast_resume_set(const char *path_or_url, uint32_t pos_ms, uint32_t dur_ms);

// Clear any saved playback position for an episode.
// Automatically persists to disk.
void podcast_resume_clear(const char *path_or_url);

// Explicitly persist the current resume table to disk (via temp file and atomic rename).
void podcast_resume_save(void);
