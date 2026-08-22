// podcast_resume: persistent playback resume positions for podcast episodes.
//
// Entries are keyed by a 64-bit FNV-1a hash of the normalized path/URL rather
// than by the stored string: episode URLs reach 512 chars and raw keys were
// truncated at 127, so long-URL episodes never matched their own entry and
// every throttled write re-inserted it until the table saturated. A 64-bit
// hash over at most 32 live keys cannot collide in practice. The debug tag
// keeps the first characters of the key readable in hex dumps only.
//
// The persisted file carries a magic header; anything else (older string-keyed
// layout, truncated write) starts empty. See podcast_resume.h for the contract.
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

#define PODCAST_RESUME_FILE     PODCAST_RESUME_DIR "/.resume.bin"
#define PODCAST_RESUME_TMP      PODCAST_RESUME_DIR "/.resume.bin.tmp"
#define PODCAST_RESUME_LFS_FILE PODCAST_RESUME_LFS_DIR "/.resume.bin"
#define PODCAST_RESUME_LFS_TMP  PODCAST_RESUME_LFS_DIR "/.resume.bin.tmp"

// "1RGB" on disk (little-endian). The old layout began with a raw int count,
// which can never equal this value, so old files fail the header cleanly.
#define PODCAST_RESUME_MAGIC 0x42475231u

typedef struct {
    uint32_t magic;
    uint32_t count;
} resume_hdr_t;

static podcast_resume_entry_t s_entries[PODCAST_RESUME_CAP];
static int s_count = 0;
static uint32_t s_seq = 0;
static volatile bool s_ready = false;

// Path normalization helper to skip leading slash and optional sdcard/ prefix,
// so one episode matches however the caller writes its path.
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

static uint64_t key_hash(const char *key)
{
    uint64_t h = 1469598103934665603ULL;  // FNV-1a 64 offset basis
    for (const char *p = key; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 1099511628211ULL;            // FNV-1a 64 prime
    }
    return h;
}

static void copy_tag(podcast_resume_entry_t *e, const char *key)
{
    size_t n = strlen(key);
    if (n > sizeof(e->tag) - 1) {
        n = sizeof(e->tag) - 1;
    }
    memcpy(e->tag, key, n);
    e->tag[n] = '\0';
}

// FatFs refuses rename() onto an existing target (FR_EXIST), so the old file
// must be removed first on SD. LittleFS replaces atomically and a pre-remove
// would open a power-loss window with no file at all (same rule as played.c).
static bool fatfs_needs_remove(const char *path)
{
    return strncmp(path, "/sdcard/", 8) == 0;
}

static bool save_to_file(const char *tmp_path, const char *final_path)
{
    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        return false;
    }

    resume_hdr_t hdr = { .magic = PODCAST_RESUME_MAGIC, .count = (uint32_t)s_count };
    bool ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1 &&
              (s_count == 0 || fwrite(s_entries, sizeof(podcast_resume_entry_t), (size_t)s_count, f) == (size_t)s_count);

    if (fclose(f) != 0) {
        ok = false;
    }

    if (!ok) {
        remove(tmp_path);
        return false;
    }

    if (fatfs_needs_remove(final_path)) {
        remove(final_path);
    }
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
        resume_hdr_t hdr;
        if (fread(&hdr, sizeof(hdr), 1, f) == 1 &&
            hdr.magic == PODCAST_RESUME_MAGIC && hdr.count <= PODCAST_RESUME_CAP &&
            (hdr.count == 0 ||
             fread(s_entries, sizeof(podcast_resume_entry_t), hdr.count, f) == hdr.count)) {
            s_count = (int)hdr.count;

            // Reconstruct s_seq from loaded entries to maintain LRU order
            uint32_t max_seq = 0;
            for (int i = 0; i < s_count; i++) {
                if (s_entries[i].updated_at > max_seq) {
                    max_seq = s_entries[i].updated_at;
                }
            }
            s_seq = max_seq;
        } else {
            ESP_LOGW(TAG, "resume file unreadable or from an older format, starting empty");
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

    uint64_t h = key_hash(normalize_path(path_or_url));

    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].key_hash == h) {
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
    uint64_t h = key_hash(norm_path);

    s_seq++;

    int found_idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].key_hash == h) {
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
            s_entries[idx].key_hash = h;
            copy_tag(&s_entries[idx], norm_path);
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
            s_entries[min_idx].key_hash = h;
            copy_tag(&s_entries[min_idx], norm_path);
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

    uint64_t h = key_hash(normalize_path(path_or_url));

    int found_idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].key_hash == h) {
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
