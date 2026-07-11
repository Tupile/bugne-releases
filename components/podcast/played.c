// played: see played.h for the design.
#include "played.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"

static const char *TAG = "played";

#define PLAYED_CAP  256
#define PLAYED_FILE "/littlefs/podcasts/played.bin"
#define PLAYED_TMP  "/littlefs/podcasts/played.bin.tmp"

static uint64_t s_hashes[PLAYED_CAP];
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
    fclose(f);
    if (!ok) {
        remove(PLAYED_TMP);
        return;
    }
    remove(PLAYED_FILE);
    if (rename(PLAYED_TMP, PLAYED_FILE) != 0) {
        ESP_LOGE(TAG, "cannot rename %s into place", PLAYED_TMP);
        remove(PLAYED_TMP);
    }
}

void played_init(void)
{
    mkdir("/littlefs/podcasts", 0775);  // may already exist (podcast manifests)
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
    if (!episode_url || !episode_url[0]) return;
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
