// podcast_resume: persistent playback resume positions for podcast episodes.
#include "podcast_resume.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"

static const char *TAG = "podcast_resume";

#ifndef PODCAST_RESUME_DIR
#define PODCAST_RESUME_DIR "/sdcard/podcasts"
#endif

#define PODCAST_RESUME_FILE PODCAST_RESUME_DIR "/.resume.bin"
#define PODCAST_RESUME_TMP  PODCAST_RESUME_DIR "/.resume.bin.tmp"

static podcast_resume_entry_t s_entries[PODCAST_RESUME_CAP];
static int s_count = 0;
static uint32_t s_seq = 0;
static volatile bool s_ready = false;

// Path normalization helper to skip leading slash and optional sdcard/ prefix.
static const char *normalize_path(const char *path)
{
    if (!path) {
        return "";
    }
    // Skip leading '/' if any
    while (*path == '/') {
        path++;
    }
    // Check if it starts with "sdcard/"
    if (strncmp(path, "sdcard/", 7) == 0) {
        path += 7;
    }
    // Skip any extra/accidental slashes
    while (*path == '/') {
        path++;
    }
    return path;
}

void podcast_resume_save(void)
{
    if (!s_ready) {
        return;
    }

    FILE *f = fopen(PODCAST_RESUME_TMP, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot write %s", PODCAST_RESUME_TMP);
        return;
    }

    bool ok = fwrite(&s_count, sizeof(s_count), 1, f) == 1 &&
              (s_count == 0 || fwrite(s_entries, sizeof(podcast_resume_entry_t), (size_t)s_count, f) == (size_t)s_count);

    if (fclose(f) != 0) {
        ok = false;
    }

    if (!ok) {
        remove(PODCAST_RESUME_TMP);
        return;
    }

    if (rename(PODCAST_RESUME_TMP, PODCAST_RESUME_FILE) != 0) {
        ESP_LOGE(TAG, "cannot rename %s into place", PODCAST_RESUME_TMP);
        remove(PODCAST_RESUME_TMP);
    }
}

void podcast_resume_init(void)
{
#ifdef PODCAST_RESUME_DIR
    mkdir(PODCAST_RESUME_DIR, 0775);
#endif

    FILE *f = fopen(PODCAST_RESUME_FILE, "rb");
    if (f) {
        int count = 0;
        if (fread(&count, sizeof(count), 1, f) == 1 &&
            count >= 0 && count <= PODCAST_RESUME_CAP &&
            (count == 0 || fread(s_entries, sizeof(podcast_resume_entry_t), (size_t)count, f) == (size_t)count)) {
            s_count = count;
            
            // Reconstruct s_seq from loaded entries to maintain LRU order
            uint32_t max_seq = 0;
            for (int i = 0; i < s_count; i++) {
                if (s_entries[i].updated_at > max_seq) {
                    max_seq = s_entries[i].updated_at;
                }
            }
            s_seq = max_seq;
        } else {
            ESP_LOGW(TAG, "resume file unreadable or corrupt, starting empty");
            s_count = 0;
            memset(s_entries, 0, sizeof(s_entries));
            s_seq = 0;
        }
        fclose(f);
    } else {
        s_count = 0;
        memset(s_entries, 0, sizeof(s_entries));
        s_seq = 0;
    }

    s_ready = true;
    ESP_LOGI(TAG, "loaded %d resume position(s)", s_count);
}

uint32_t podcast_resume_get(const char *path_or_url, uint32_t *dur_ms)
{
    if (!s_ready || !path_or_url || path_or_url[0] == '\0') {
        if (dur_ms) {
            *dur_ms = 0;
        }
        return 0;
    }

    const char *norm_path = normalize_path(path_or_url);

    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].path, norm_path) == 0) {
            if (dur_ms) {
                *dur_ms = s_entries[i].dur_ms;
            }
            return s_entries[i].pos_ms;
        }
    }

    if (dur_ms) {
        *dur_ms = 0;
    }
    return 0;
}

void podcast_resume_set(const char *path_or_url, uint32_t pos_ms, uint32_t dur_ms)
{
    if (!s_ready || !path_or_url || path_or_url[0] == '\0') {
        return;
    }

    const char *norm_path = normalize_path(path_or_url);

    s_seq++;

    int found_idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].path, norm_path) == 0) {
            found_idx = i;
            break;
        }
    }

    if (found_idx != -1) {
        s_entries[found_idx].pos_ms = pos_ms;
        s_entries[found_idx].dur_ms = dur_ms;
        s_entries[found_idx].updated_at = s_seq;
    } else {
        if (s_count < PODCAST_RESUME_CAP) {
            int idx = s_count;
            snprintf(s_entries[idx].path, sizeof(s_entries[idx].path), "%s", norm_path);
            s_entries[idx].pos_ms = pos_ms;
            s_entries[idx].dur_ms = dur_ms;
            s_entries[idx].updated_at = s_seq;
            s_count++;
        } else {
            // Table is full, evict the entry with the minimum updated_at (LRU)
            int min_idx = 0;
            uint32_t min_seq = s_entries[0].updated_at;
            for (int i = 1; i < PODCAST_RESUME_CAP; i++) {
                if (s_entries[i].updated_at < min_seq) {
                    min_seq = s_entries[i].updated_at;
                    min_idx = i;
                }
            }
            snprintf(s_entries[min_idx].path, sizeof(s_entries[min_idx].path), "%s", norm_path);
            s_entries[min_idx].pos_ms = pos_ms;
            s_entries[min_idx].dur_ms = dur_ms;
            s_entries[min_idx].updated_at = s_seq;
        }
    }

    podcast_resume_save();
}

void podcast_resume_clear(const char *path_or_url)
{
    if (!s_ready || !path_or_url || path_or_url[0] == '\0') {
        return;
    }

    const char *norm_path = normalize_path(path_or_url);

    int found_idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].path, norm_path) == 0) {
            found_idx = i;
            break;
        }
    }

    if (found_idx != -1) {
        for (int i = found_idx; i < s_count - 1; i++) {
            s_entries[i] = s_entries[i + 1];
        }
        memset(&s_entries[s_count - 1], 0, sizeof(podcast_resume_entry_t));
        s_count--;

        podcast_resume_save();
    }
}
