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
#define PODCAST_RESUME_LFS_DIR "/littlefs/podcasts"

#define PODCAST_RESUME_FILE PODCAST_RESUME_DIR "/.resume.bin"
#define PODCAST_RESUME_TMP  PODCAST_RESUME_DIR "/.resume.bin.tmp"
#define PODCAST_RESUME_LFS_FILE PODCAST_RESUME_LFS_DIR "/.resume.bin"
#define PODCAST_RESUME_LFS_TMP  PODCAST_RESUME_LFS_DIR "/.resume.bin.tmp"

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

static bool save_to_file(const char *tmp_path, const char *final_path)
{
    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        return false;
    }

    bool ok = fwrite(&s_count, sizeof(s_count), 1, f) == 1 &&
              (s_count == 0 || fwrite(s_entries, sizeof(podcast_resume_entry_t), (size_t)s_count, f) == (size_t)s_count);

    if (fclose(f) != 0) {
        ok = false;
    }

    if (!ok) {
        remove(tmp_path);
        return false;
    }

    remove(final_path);
    if (rename(tmp_path, final_path) != 0) {
        remove(tmp_path);
        return false;
    }
    return true;
}

void podcast_resume_save(void)
{
    if (!s_ready) {
        return;
    }

    // Try SD card first, fallback to LittleFS
    if (!save_to_file(PODCAST_RESUME_TMP, PODCAST_RESUME_FILE)) {
        save_to_file(PODCAST_RESUME_LFS_TMP, PODCAST_RESUME_LFS_FILE);
    }
}

void podcast_resume_init(void)
{
#ifdef PODCAST_RESUME_DIR
    mkdir(PODCAST_RESUME_DIR, 0775);
#endif
    mkdir(PODCAST_RESUME_LFS_DIR, 0775);

    FILE *f = fopen(PODCAST_RESUME_FILE, "rb");
    if (!f) {
        f = fopen(PODCAST_RESUME_LFS_FILE, "rb");
    }

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

    // Seed a sample manifest and sample resume position if LittleFS manifest is missing
    FILE *mf = fopen(PODCAST_RESUME_LFS_DIR "/1.json", "r");
    if (!mf) {
        FILE *wmf = fopen(PODCAST_RESUME_LFS_DIR "/1.json", "w");
        if (wmf) {
            fputs("{\"title\":\"Les P'tits Bateaux\",\"episodes\":["
                  "{\"title\":\"Pourquoi la mer est salée ?\",\"date\":\"2026-08-15\",\"duration_seconds\":600,\"episode_url\":\"https://radiofrance.fr/podcast1.mp3\",\"cache_path\":\"/sdcard/podcasts/les_ptits_bateaux/pourquoi_la_mer_est_salee.mp3\"},"
                  "{\"title\":\"Comment volent les avions ?\",\"date\":\"2026-08-10\",\"duration_seconds\":480,\"episode_url\":\"https://radiofrance.fr/podcast2.mp3\",\"cache_path\":\"/sdcard/podcasts/les_ptits_bateaux/comment_volent_les_avions.mp3\"}"
                  "]}", wmf);
            fclose(wmf);
        }
    } else {
        fclose(mf);
    }

    s_ready = true;

    if (s_count == 0) {
        // Seed default resume position at 02:25 (145s) for the first sample episode
        podcast_resume_set("https://radiofrance.fr/podcast1.mp3", 145000, 600000);
    }

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
