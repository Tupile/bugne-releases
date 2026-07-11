// library: tag-based music index of the SD card (see library.h).
#include "library.h"
#include "decode.h"
#include "source_sd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "library";

#define SD_MOUNT    "/sdcard"
#define INDEX_PATH  "/sdcard/.bugne_library.tsv"
#define LIB_MAX_TRACKS 4000   // PSRAM model cap (~1.8 MB)

typedef struct {
    char artist[LIB_NAME_MAX];
    char album[LIB_NAME_MAX];
    char title[LIB_NAME_MAX];
    char path[LIB_PATH_MAX];   // relative to the SD root
    int  track;
} lib_track_t;

static lib_track_t *s_tracks;   // PSRAM
static size_t s_count;
static volatile bool s_scanning;
static volatile bool *s_scan_cancel;  // polled during scan_dir; NULL = not cancelable

static bool ensure_alloc(void)
{
    if (!s_tracks) {
        s_tracks = heap_caps_malloc(sizeof(lib_track_t) * LIB_MAX_TRACKS, MALLOC_CAP_SPIRAM);
    }
    return s_tracks != NULL;
}

// ---- decode_source_t over a FILE* (for tag reading) ----
static size_t f_read(void *ctx, void *buf, size_t n) { return fread(buf, 1, n, (FILE *)ctx); }
static bool f_seek(void *ctx, int off, int origin)
{
    int w = origin == 1 ? SEEK_CUR : (origin == 2 ? SEEK_END : SEEK_SET);
    return fseek((FILE *)ctx, off, w) == 0;
}
static bool f_tell(void *ctx, int64_t *cur)
{
    long p = ftell((FILE *)ctx);
    if (p < 0) return false;
    *cur = p;
    return true;
}

static bool fmt_from(const char *name, decode_format_t *fmt)
{
    const char *d = strrchr(name, '.');
    if (!d) return false;
    if (!strcasecmp(d, ".mp3"))  { *fmt = DECODE_FORMAT_MP3;  return true; }
    if (!strcasecmp(d, ".flac")) { *fmt = DECODE_FORMAT_FLAC; return true; }
    if (!strcasecmp(d, ".ogg") || !strcasecmp(d, ".opus") || !strcasecmp(d, ".oga")) {
        *fmt = DECODE_FORMAT_OGG; return true;  // tag-less: filename fallback title
    }
    return false;
}

// ---- scan ----
static void scan_dir(const char *absdir, const char *reldir)
{
    DIR *d = opendir(absdir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_count < LIB_MAX_TRACKS) {
        if (s_scan_cancel && *s_scan_cancel) break;  // user started playing: abort
        if (e->d_name[0] == '.') continue;  // skip hidden (incl. our index)
        char ab[400], rel[LIB_PATH_MAX];
        int an = snprintf(ab, sizeof(ab), "%s/%s", absdir, e->d_name);
        int rn = reldir[0] ? snprintf(rel, sizeof(rel), "%s/%s", reldir, e->d_name)
                           : snprintf(rel, sizeof(rel), "%s", e->d_name);
        if (an <= 0 || an >= (int)sizeof(ab) || rn <= 0 || rn >= (int)sizeof(rel)) continue;
        if (e->d_type == DT_DIR) {
            scan_dir(ab, rel);
            continue;
        }
        decode_format_t fmt;
        if (!fmt_from(e->d_name, &fmt)) continue;
        FILE *f = fopen(ab, "rb");
        if (!f) continue;
        decode_tags_t tg;
        decode_source_t src = { .read = f_read, .seek = f_seek, .tell = f_tell, .ctx = f };
        esp_err_t r = decode_read_tags(fmt, &src, &tg);
        fclose(f);
        if (r != ESP_OK) continue;
        lib_track_t *t = &s_tracks[s_count++];
        // Group by album artist (ALBUMARTIST) so a compilation's tracks, which
        // carry different per-track ARTIST tags, stay under one album. Fall back
        // to the track artist, then "Unknown artist".
        const char *grp = tg.album_artist[0] ? tg.album_artist
                        : (tg.artist[0] ? tg.artist : "Unknown artist");
        strlcpy(t->artist, grp, sizeof(t->artist));
        strlcpy(t->album,  tg.album[0] ? tg.album : "Unknown album", sizeof(t->album));
        if (tg.title[0]) {
            strlcpy(t->title, tg.title, sizeof(t->title));
        } else {  // fall back to the file name without its extension
            strlcpy(t->title, e->d_name, sizeof(t->title));
            char *dot = strrchr(t->title, '.');
            if (dot) *dot = '\0';
        }
        strlcpy(t->path, rel, sizeof(t->path));
        t->track = tg.track;
    }
    closedir(d);
}

static void sanitize(char *s)  // TSV-safe: no tab/newline in a field
{
    for (; *s; s++) {
        if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
    }
}

static void write_index(void)
{
    FILE *f = fopen(INDEX_PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "cannot write index");
        return;
    }
    for (size_t i = 0; i < s_count; i++) {
        lib_track_t t = s_tracks[i];  // copy so sanitizing does not touch the model
        sanitize(t.artist); sanitize(t.album); sanitize(t.title); sanitize(t.path);
        fprintf(f, "%s\t%s\t%d\t%s\t%s\n", t.artist, t.album, t.track, t.title, t.path);
    }
    fclose(f);
}

esp_err_t library_scan_cancelable(volatile bool *cancel)
{
    if (!source_sd_present()) return ESP_ERR_INVALID_STATE;
    if (!ensure_alloc()) return ESP_ERR_NO_MEM;
    s_scan_cancel = cancel;
    s_count = 0;
    scan_dir(SD_MOUNT, "");
    bool cancelled = (cancel && *cancel);
    s_scan_cancel = NULL;
    if (cancelled) {
        // Don't overwrite the index with a partial scan; restore the previous one.
        ESP_LOGW(TAG, "scan cancelled; keeping the existing index");
        library_load();
        return ESP_ERR_INVALID_STATE;
    }
    write_index();
    ESP_LOGI(TAG, "scan complete: %u tracks", (unsigned)s_count);
    if (s_count >= LIB_MAX_TRACKS) {
        ESP_LOGW(TAG, "track cap %d reached; remaining files were not indexed", LIB_MAX_TRACKS);
    }
    return ESP_OK;
}

esp_err_t library_scan(void) { return library_scan_cancelable(NULL); }

esp_err_t library_load(void)
{
    if (!ensure_alloc()) return ESP_ERR_NO_MEM;
    FILE *f = fopen(INDEX_PATH, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    s_count = 0;
    char line[LIB_PATH_MAX + 3 * LIB_NAME_MAX + 16];
    while (s_count < LIB_MAX_TRACKS && fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        char *fields[5], *p = line;
        bool ok = true;
        for (int k = 0; k < 4; k++) {  // first 4 fields are tab-terminated
            char *tab = strchr(p, '\t');
            if (!tab) { ok = false; break; }
            *tab = '\0';
            fields[k] = p;
            p = tab + 1;
        }
        if (!ok) continue;
        fields[4] = p;  // path (rest of the line)
        lib_track_t *t = &s_tracks[s_count++];
        strlcpy(t->artist, fields[0], sizeof(t->artist));
        strlcpy(t->album,  fields[1], sizeof(t->album));
        t->track = atoi(fields[2]);
        strlcpy(t->title,  fields[3], sizeof(t->title));
        strlcpy(t->path,   fields[4], sizeof(t->path));
    }
    fclose(f);
    ESP_LOGI(TAG, "loaded %u tracks", (unsigned)s_count);
    return ESP_OK;
}

size_t library_track_count(void) { return s_count; }

// Insertion sort of a name array (case-insensitive).
static void sort_names(char arr[][LIB_NAME_MAX], size_t n)
{
    for (size_t i = 1; i < n; i++) {
        char key[LIB_NAME_MAX];
        strlcpy(key, arr[i], LIB_NAME_MAX);
        size_t j = i;
        while (j > 0 && strcasecmp(arr[j - 1], key) > 0) {
            strlcpy(arr[j], arr[j - 1], LIB_NAME_MAX);
            j--;
        }
        strlcpy(arr[j], key, LIB_NAME_MAX);
    }
}

size_t library_artists(char out[][LIB_NAME_MAX], size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < s_count && n < max; i++) {
        // Tracks are stored grouped by folder, so the previous unique almost
        // always matches: O(1) fast path before the full dedup scan.
        if (n > 0 && !strcmp(out[n - 1], s_tracks[i].artist)) continue;
        bool seen = false;
        for (size_t j = 0; j < n; j++) {
            if (!strcmp(out[j], s_tracks[i].artist)) { seen = true; break; }
        }
        if (!seen) strlcpy(out[n++], s_tracks[i].artist, LIB_NAME_MAX);
    }
    sort_names(out, n);
    return n;
}

size_t library_albums(const char *artist, char out[][LIB_NAME_MAX], size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < s_count && n < max; i++) {
        if (strcmp(s_tracks[i].artist, artist) != 0) continue;
        if (n > 0 && !strcmp(out[n - 1], s_tracks[i].album)) continue;  // folder locality
        bool seen = false;
        for (size_t j = 0; j < n; j++) {
            if (!strcmp(out[j], s_tracks[i].album)) { seen = true; break; }
        }
        if (!seen) strlcpy(out[n++], s_tracks[i].album, LIB_NAME_MAX);
    }
    sort_names(out, n);
    return n;
}

size_t library_all_albums(char albums[][LIB_NAME_MAX], char artists[][LIB_NAME_MAX], size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < s_count && n < max; i++) {
        if (n > 0 && !strcmp(albums[n - 1], s_tracks[i].album) &&
            !strcmp(artists[n - 1], s_tracks[i].artist)) continue;  // folder locality
        bool seen = false;
        for (size_t j = 0; j < n; j++) {
            if (!strcmp(albums[j], s_tracks[i].album) && !strcmp(artists[j], s_tracks[i].artist)) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            strlcpy(albums[n], s_tracks[i].album, LIB_NAME_MAX);
            strlcpy(artists[n], s_tracks[i].artist, LIB_NAME_MAX);
            n++;
        }
    }
    // Insertion sort by album name, carrying the artist along.
    for (size_t a = 1; a < n; a++) {
        char ka[LIB_NAME_MAX], kr[LIB_NAME_MAX];
        strlcpy(ka, albums[a], LIB_NAME_MAX);
        strlcpy(kr, artists[a], LIB_NAME_MAX);
        size_t b = a;
        while (b > 0 && strcasecmp(albums[b - 1], ka) > 0) {
            strlcpy(albums[b], albums[b - 1], LIB_NAME_MAX);
            strlcpy(artists[b], artists[b - 1], LIB_NAME_MAX);
            b--;
        }
        strlcpy(albums[b], ka, LIB_NAME_MAX);
        strlcpy(artists[b], kr, LIB_NAME_MAX);
    }
    return n;
}

size_t library_album_tracks(const char *artist, const char *album,
                            char titles[][LIB_NAME_MAX], char paths[][LIB_PATH_MAX], size_t max)
{
    // Collect matching track indices, then sort by track number, then title.
    size_t idx[128];
    size_t n = 0;
    size_t cap = max < 128 ? max : 128;
    for (size_t i = 0; i < s_count && n < cap; i++) {
        if (!strcmp(s_tracks[i].artist, artist) && !strcmp(s_tracks[i].album, album)) {
            idx[n++] = i;
        }
    }
    for (size_t a = 1; a < n; a++) {  // insertion sort by (track, title)
        size_t key = idx[a], b = a;
        while (b > 0) {
            lib_track_t *p = &s_tracks[idx[b - 1]], *k = &s_tracks[key];
            bool greater = (p->track != k->track) ? (p->track > k->track)
                                                  : (strcasecmp(p->title, k->title) > 0);
            if (!greater) break;
            idx[b] = idx[b - 1];
            b--;
        }
        idx[b] = key;
    }
    for (size_t i = 0; i < n; i++) {
        strlcpy(titles[i], s_tracks[idx[i]].title, LIB_NAME_MAX);
        strlcpy(paths[i],  s_tracks[idx[i]].path,  LIB_PATH_MAX);
    }
    return n;
}

bool library_scanning(void) { return s_scanning; }

static void scan_task(void *arg)
{
    (void)arg;
    library_scan();
    s_scanning = false;
    vTaskDelete(NULL);
}

bool library_scan_start(void)
{
    if (s_scanning || !source_sd_present()) return false;
    s_scanning = true;
    if (xTaskCreate(scan_task, "lib_scan", 8192, NULL, 4, NULL) != pdPASS) {
        s_scanning = false;
        return false;
    }
    return true;
}
