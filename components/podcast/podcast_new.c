// podcast_new: see podcast_new.h for the design.
#include "podcast_new.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

static const char *TAG = "podnew";

#ifndef PODCAST_NEW_CAP               // host tests shrink it to exercise the ring
#define PODCAST_NEW_CAP 128
#endif
#ifndef PODCAST_NEW_DIR               // host tests redirect this to /tmp
#define PODCAST_NEW_DIR "/littlefs/podcasts"
#endif
#define NEW_FILE PODCAST_NEW_DIR "/new.bin"
#define NEW_TMP  PODCAST_NEW_DIR "/new.bin.tmp"

typedef struct {
    int32_t  pod_id;
    uint32_t pad;      // explicit, so the on-flash layout has no garbage bytes
    uint64_t hash;
} entry_t;

#ifdef ESP_PLATFORM
// PSRAM (2 KB of plain data), same rule as the played ring.
static entry_t *s_tab;
#else
static entry_t s_tab[PODCAST_NEW_CAP];
#endif
static int s_count;   // valid entries, packed at s_tab[0..s_count-1]
static int s_next;    // ring slot the next add overwrites once full
static bool s_ready;
static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;

uint64_t podcast_new_hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;  // FNV-1a 64-bit offset basis
    for (; s && *s; s++) {
        h ^= (uint8_t)*s;
        h *= 1099511628211ULL;  // FNV prime
    }
    return h;
}

static int find(uint64_t h)
{
    for (int i = 0; i < s_count; i++) {
        if (s_tab[i].hash == h) return i;
    }
    return -1;
}

static void save(void)
{
    FILE *f = fopen(NEW_TMP, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot write %s", NEW_TMP);
        return;
    }
    bool ok = fwrite(&s_count, sizeof(s_count), 1, f) == 1 &&
              fwrite(&s_next, sizeof(s_next), 1, f) == 1 &&
              (s_count == 0 || fwrite(s_tab, sizeof(entry_t), (size_t)s_count, f) == (size_t)s_count);
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        remove(NEW_TMP);
        return;
    }
    // LittleFS rename replaces the target atomically (see played.c).
    if (rename(NEW_TMP, NEW_FILE) != 0) {
        ESP_LOGE(TAG, "cannot rename %s into place", NEW_TMP);
        remove(NEW_TMP);
    }
}

// Remove entry i keeping the table packed. Order is irrelevant except for the
// ring cursor: a removal reopens a free slot, so the next add appends.
static void remove_at(int i)
{
    s_tab[i] = s_tab[s_count - 1];
    s_count--;
    s_next = s_count;
}

void podcast_new_init(void)
{
    pthread_mutex_lock(&s_mx);
#ifdef ESP_PLATFORM
    if (!s_tab) {
        s_tab = heap_caps_malloc(sizeof(entry_t) * PODCAST_NEW_CAP, MALLOC_CAP_SPIRAM);
        if (!s_tab) {
            ESP_LOGE(TAG, "no memory for the new-episode table");
            pthread_mutex_unlock(&s_mx);
            return;
        }
    }
#endif
    mkdir(PODCAST_NEW_DIR, 0775);  // may already exist
    s_count = 0;
    s_next = 0;
    FILE *f = fopen(NEW_FILE, "rb");
    if (f) {
        int count = 0, next = 0;
        if (fread(&count, sizeof(count), 1, f) == 1 &&
            fread(&next, sizeof(next), 1, f) == 1 &&
            count >= 0 && count <= PODCAST_NEW_CAP && next >= 0 && next <= PODCAST_NEW_CAP &&
            fread(s_tab, sizeof(entry_t), (size_t)count, f) == (size_t)count) {
            s_count = count;
            s_next = next % PODCAST_NEW_CAP;
        } else {
            ESP_LOGW(TAG, "new.bin unreadable or corrupt, starting empty");
        }
        fclose(f);
    }
    s_ready = true;
    pthread_mutex_unlock(&s_mx);
    ESP_LOGI(TAG, "loaded %d new-episode marker(s)", s_count);
}

void podcast_new_add(int pod_id, const char *episode_url)
{
    if (!episode_url || !episode_url[0]) return;
    uint64_t h = podcast_new_hash(episode_url);
    pthread_mutex_lock(&s_mx);
    if (s_ready && find(h) < 0) {
        entry_t e = { .pod_id = pod_id, .pad = 0, .hash = h };
        if (s_count < PODCAST_NEW_CAP) {
            s_tab[s_count++] = e;
            s_next = s_count % PODCAST_NEW_CAP;
        } else {
            s_tab[s_next] = e;   // full: overwrite the oldest
            s_next = (s_next + 1) % PODCAST_NEW_CAP;
        }
        save();
    }
    pthread_mutex_unlock(&s_mx);
}

void podcast_new_clear(const char *episode_url)
{
    if (!episode_url || !episode_url[0]) return;
    uint64_t h = podcast_new_hash(episode_url);
    pthread_mutex_lock(&s_mx);
    int i = s_ready ? find(h) : -1;
    if (i >= 0) {
        remove_at(i);
        save();
    }
    pthread_mutex_unlock(&s_mx);
}

bool podcast_new_contains(const char *episode_url)
{
    if (!episode_url || !episode_url[0]) return false;
    uint64_t h = podcast_new_hash(episode_url);
    pthread_mutex_lock(&s_mx);
    bool r = s_ready && find(h) >= 0;
    pthread_mutex_unlock(&s_mx);
    return r;
}

int podcast_new_count(int pod_id)
{
    int n = 0;
    pthread_mutex_lock(&s_mx);
    for (int i = 0; s_ready && i < s_count; i++) {
        if (s_tab[i].pod_id == pod_id) n++;
    }
    pthread_mutex_unlock(&s_mx);
    return n;
}

void podcast_new_retain(int pod_id, const uint64_t *keep, size_t n)
{
    pthread_mutex_lock(&s_mx);
    bool changed = false;
    for (int i = 0; s_ready && i < s_count;) {
        bool kept = s_tab[i].pod_id != pod_id;
        for (size_t k = 0; !kept && k < n; k++) {
            if (keep[k] == s_tab[i].hash) kept = true;
        }
        if (kept) {
            i++;
        } else {
            remove_at(i);   // swaps the last entry into i: re-test the same slot
            changed = true;
        }
    }
    if (changed) save();
    pthread_mutex_unlock(&s_mx);
}
