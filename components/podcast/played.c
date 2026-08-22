// played: see played.h for the design.
#include "played.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

static const char *TAG = "played";

#define PLAYED_CAP  256
#ifndef PLAYED_DIR                    // host tests redirect this to /tmp
#define PLAYED_DIR  "/littlefs/podcasts"
#endif
#define PLAYED_FILE PLAYED_DIR "/played.bin"
#define PLAYED_TMP  PLAYED_DIR "/played.bin.tmp"

#ifdef ESP_PLATFORM
// PSRAM: 2 KB of internal-RAM budget for plain data (same rule as the stats
// ring). Allocated once in played_init; host tests keep the static array.
static uint64_t *s_hashes;
#else
static uint64_t s_hashes[PLAYED_CAP];
#endif
static int  s_count;   // number of valid entries in s_hashes
static int  s_next;    // ring index the next mark will overwrite
// played_init() runs once on bg_init_task, before the UI can reach the
// episodes screen; played_mark()/played_contains() run only on the UI task
// afterwards (see played.h). volatile so the one-shot init is visible across
// that task handoff, same convention as other cross-task flags in ui.c.
static volatile bool s_ready;

static uint64_t fnv1a64(const char *s)
{
    uint64_t h = 1469598103934665603ULL;  // FNV-1a 64-bit offset basis
    for (; *s; s++) {
        h ^= (uint8_t)*s;
        h *= 1099511628211ULL;  // FNV prime
    }
    return h;
}

static bool contains_hash(uint64_t h)
{
    for (int i = 0; i < s_count; i++) {
        if (s_hashes[i] == h) return true;
    }
    return false;
}

static void save(void)
{
    FILE *f = fopen(PLAYED_TMP, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot write %s", PLAYED_TMP);
        return;
    }
    bool ok = fwrite(&s_count, sizeof(s_count), 1, f) == 1 &&
              fwrite(&s_next, sizeof(s_next), 1, f) == 1 &&
              (s_count == 0 || fwrite(s_hashes, sizeof(uint64_t), (size_t)s_count, f) == (size_t)s_count);
    if (fclose(f) != 0) ok = false;  // the flush is where a full LittleFS fails
    if (!ok) {
        remove(PLAYED_TMP);
        return;
    }
    // No remove() first: this file is on LittleFS, whose rename replaces the
    // target atomically. Removing it opened a power-loss window with no file.
    if (rename(PLAYED_TMP, PLAYED_FILE) != 0) {
        ESP_LOGE(TAG, "cannot rename %s into place", PLAYED_TMP);
        remove(PLAYED_TMP);
    }
}

void played_init(void)
{
#ifdef ESP_PLATFORM
    if (!s_hashes) {
        s_hashes = heap_caps_malloc(sizeof(uint64_t) * PLAYED_CAP, MALLOC_CAP_SPIRAM);
        if (!s_hashes) {
            // Without the ring, markers are a no-op: s_ready stays false so
            // played_mark/played_contains never touch a NULL table.
            ESP_LOGE(TAG, "no memory for the played ring");
            return;
        }
    }
#endif
    mkdir(PLAYED_DIR, 0775);  // may already exist (podcast manifests)
    FILE *f = fopen(PLAYED_FILE, "rb");
    if (f) {
        int count = 0, next = 0;
        if (fread(&count, sizeof(count), 1, f) == 1 &&
            fread(&next, sizeof(next), 1, f) == 1 &&
            count >= 0 && count <= PLAYED_CAP && next >= 0 && next < PLAYED_CAP &&
            fread(s_hashes, sizeof(uint64_t), (size_t)count, f) == (size_t)count) {
            s_count = count;
            s_next = next;
        } else {
            ESP_LOGW(TAG, "played.bin unreadable or corrupt, starting empty");
            s_count = 0;
            s_next = 0;
        }
        fclose(f);
    }
    s_ready = true;
    ESP_LOGI(TAG, "loaded %d played marker(s)", s_count);
}

void played_mark(const char *episode_url)
{
    // Same ready guard as played_contains: a mark racing played_init (the UI
    // task can run first) would save a 1-entry ring over the loaded one.
    if (!s_ready || !episode_url || !episode_url[0]) return;
    uint64_t h = fnv1a64(episode_url);
    if (contains_hash(h)) return;  // already marked: nothing to add or persist
    if (s_count < PLAYED_CAP) {
        s_hashes[s_count] = h;
        s_count++;
        s_next = s_count % PLAYED_CAP;
    } else {
        s_hashes[s_next] = h;
        s_next = (s_next + 1) % PLAYED_CAP;
    }
    save();
}

bool played_contains(const char *episode_url)
{
    if (!s_ready || !episode_url || !episode_url[0]) return false;
    return contains_hash(fnv1a64(episode_url));
}
