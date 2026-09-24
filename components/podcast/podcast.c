// podcast: fetch an RSS feed over HTTP(S), parse it with the rss_parse core,
// and write a bounded manifest to SD. Reads the manifest back for the UI.
#include "podcast.h"
#include "rss_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  // strncasecmp
#include <time.h>
#include <errno.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "source_sd.h"

static const char *TAG = "podcast";

#define HTTP_CHUNK      4096
#define HTTP_TIMEOUT_MS 3000
#define MAX_REDIRECTS   6
// Hard wall-clock cap on a refresh. Per-read timeouts (HTTP_TIMEOUT_MS) bound a
// single stalled read, but a server that trickles bytes could keep the loop
// going for a long time. This guarantees the refresh always terminates.
#define REFRESH_MAX_MS  30000
// Same reasoning for an episode download, with room for a long episode on a slow
// link: an enclosure URL comes from an untrusted feed and could point at a live
// stream, which would otherwise be downloaded until the card is full.
#define EPISODE_MAX_BYTES (300ULL * 1024 * 1024)
#define EPISODE_MAX_MS    (60 * 60 * 1000)

// Open the client, following redirects. Returns the final status code or -1.
static bool transfer_stopped(volatile bool *cancel, int64_t deadline)
{
    return (cancel && *cancel) || esp_timer_get_time() >= deadline;
}

static int http_open_redirect(esp_http_client_handle_t client, volatile bool *cancel,
                              int64_t deadline)
{
    for (int i = 0; i < MAX_REDIRECTS; i++) {
        if (transfer_stopped(cancel, deadline)) return -1;
        if (esp_http_client_open(client, 0) != ESP_OK) return -1;
        if (transfer_stopped(cancel, deadline)) return -1;
        if (esp_http_client_fetch_headers(client) < 0) return -1;
        if (transfer_stopped(cancel, deadline)) return -1;
        int status = esp_http_client_get_status_code(client);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            if (esp_http_client_set_redirection(client) != ESP_OK) return -1;
            esp_http_client_close(client);
            continue;
        }
        return status;
    }
    return -1;
}

// Turn an untrusted title into one safe FAT path component. Drops characters
// illegal on FAT (and our separators), keeps UTF-8 multibyte sequences whole
// (FATFS here is LFN + UTF-8), trims leading/trailing spaces and dots, bounds
// the length, and uses `fallback` if nothing usable remains.
static void sanitize_name(const char *in, char *out, size_t cap, const char *fallback)
{
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 1 < cap; ) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            out[j++] = '_';
            i++;
            continue;
        }
        int seq = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        if (seq == 1) { out[j++] = (char)c; i++; continue; }
        // Copy a multibyte sequence only whole, and only if it fits and is valid.
        bool valid = (j + seq + 1 < cap);
        for (int k = 1; valid && k < seq; k++)
            if (((unsigned char)in[i + k] & 0xC0) != 0x80) valid = false;
        if (!valid) { out[j++] = '_'; i++; continue; }
        for (int k = 0; k < seq; k++) out[j++] = in[i + k];
        i += seq;
    }
    out[j] = '\0';
    while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '.')) out[--j] = '\0';
    size_t s = 0;
    while (out[s] == ' ' || out[s] == '.') s++;
    if (s) { memmove(out, out + s, j - s + 1); j -= s; }
    if (j == 0) strlcpy(out, fallback, cap);
}

// Streaming manifest writer: episodes are written to the open file one at a time
// (from the RSS parse callback), so the whole episode list is never held in RAM.
// The manifest (small metadata) lives in internal flash so podcasts work without
// an SD card. Only cached episode audio (cache_path) needs SD.
typedef struct {
    FILE       *f;
    bool        header_written;
    bool        failed;
    int         count;
    char        podir[80];          // per-podcast SD folder, resolved at header time
    const char *name;               // config display title (may be "")
    const char *rss_url;
    char        generated_at[32];
    const rss_parser_t *p;          // for the feed's <title> at header time
    uint32_t   *seen;               // hash of each cache_path already emitted
    uint32_t   *seen_url;           // hash of each episode_url, same index (same block)
    int         seen_max;           // 0 when the allocation failed (dedup off)
} manifest_writer_t;

// FNV-1a, only ever compared against itself (cache_path dedup below).
static uint32_t path_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h = (h ^ (unsigned char)*s) * 16777619u;
    }
    return h;
}

static void download_cover(const char *url, const char *podir, volatile bool *cancel,
                           int64_t deadline);
static void mw_retain_dropped(manifest_writer_t *w, int id);

static void mw_write_header(manifest_writer_t *w)
{
    // The SD folder is named after the podcast (config title, else feed title).
    sanitize_name((w->name && w->name[0]) ? w->name : w->p->podcast_title,
                  w->podir, sizeof(w->podir), "podcast");
    cJSON *h = cJSON_CreateObject();
    if (!h) { w->failed = true; return; }
    cJSON_AddNumberToObject(h, "schema_version", 1);
    cJSON_AddStringToObject(h, "podcast_title", w->p->podcast_title);
    cJSON_AddStringToObject(h, "rss_url", w->rss_url);
    cJSON_AddStringToObject(h, "generated_at", w->generated_at);
    char *hs = cJSON_PrintUnformatted(h);
    cJSON_Delete(h);
    if (!hs) { w->failed = true; return; }
    size_t hl = strlen(hs);
    if (hl && hs[hl - 1] == '}') hs[hl - 1] = '\0';  // drop the closing brace
    if (fprintf(w->f, "%s,\"episodes\":[", hs) < 0) w->failed = true;
    cJSON_free(hs);
    w->header_written = true;
}

// Cache extension, from the URL PATH and matched at its END. Searching the whole
// URL made a query string win: "ep.mp3?src=feed.m4a" was cached as .m4a, then
// handed to the AAC decoder and skipped the MP3 intro trim.
static const char *url_ext(const char *url)
{
    static const struct { const char *dot; const char *ext; } MAP[] = {
        {".flac", "flac"}, {".m4a", "m4a"}, {".mp4", "m4a"}, {".aac", "m4a"},
        {".opus", "opus"}, {".ogg", "ogg"}, {".oga", "ogg"}, {".mp3", "mp3"},
    };
    size_t n = strcspn(url, "?#");        // path part only
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (url[i] == '/') start = i + 1;  // last path segment
    }
    const char *seg = url + start;
    size_t len = n - start;
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
        size_t dl = strlen(MAP[i].dot);
        if (len >= dl && strncasecmp(seg + len - dl, MAP[i].dot, dl) == 0) {
            return MAP[i].ext;
        }
    }
    return "mp3";  // most feeds, and the safe default for an extensionless URL
}

// rss_parse callback: append one episode object to the manifest file.
static void mw_on_episode(const rss_episode_t *ep, void *ctx)
{
    manifest_writer_t *w = ctx;
    if (w->failed || !w->f) return;
    if (!w->header_written) mw_write_header(w);
    if (w->failed) return;

    const char *ext = url_ext(ep->url);
    // Stable per-episode key for the untitled fallback and the collision
    // suffix: the pubDate, or the URL when the feed has no date. Never the
    // position in the feed: a new episode shifts every position, which moved
    // the path of an already-downloaded file (orphaned, re-downloaded, and
    // carried over as a duplicate row by mw_retain_dropped).
    uint32_t key = path_hash(ep->date[0] ? ep->date : ep->url);
    char epname[128];
    char fallback[24];
    snprintf(fallback, sizeof(fallback), "episode_%08" PRIx32, key);
    sanitize_name(ep->title, epname, sizeof(epname), fallback);
    char cache_path[PODCAST_PATH_MAX];
    snprintf(cache_path, sizeof(cache_path), "/sdcard/podcasts/%s/%s.%s", w->podir, epname, ext);

    // Two episodes whose titles sanitize (or truncate) to the same name would
    // share one cached file, so the second would play the first one's audio.
    // Disambiguate only on an ACTUAL collision: every other episode keeps its
    // historical path, so files already downloaded stay recognized (cached-ness
    // is a stat() of cache_path, so a blanket renaming would orphan them all).
    if (w->seen && w->count < w->seen_max) {
        uint32_t h = path_hash(cache_path);
        // Pass 0 tries the stable suffix; pass 1 (same title AND same date,
        // pathological) falls back to the position so paths stay unique.
        for (int pass = 0; pass < 2; pass++) {
            bool hit = false;
            for (int i = 0; i < w->count && !hit; i++) hit = w->seen[i] == h;
            if (!hit) break;
            if (pass == 0)
                snprintf(cache_path, sizeof(cache_path), "/sdcard/podcasts/%s/%s_%08" PRIx32 ".%s",
                         w->podir, epname, key, ext);
            else
                snprintf(cache_path, sizeof(cache_path), "/sdcard/podcasts/%s/%s_%08" PRIx32 "_%d.%s",
                         w->podir, epname, key, w->count, ext);
            h = path_hash(cache_path);
        }
        w->seen[w->count] = h;
        w->seen_url[w->count] = path_hash(ep->url);
    }

    cJSON *e = cJSON_CreateObject();
    if (!e) { w->failed = true; return; }
    cJSON_AddStringToObject(e, "title", ep->title);
    cJSON_AddStringToObject(e, "date", ep->date);
    cJSON_AddNumberToObject(e, "duration_seconds", ep->duration_seconds);
    cJSON_AddStringToObject(e, "episode_url", ep->url);
    cJSON_AddStringToObject(e, "cache_path", cache_path);
    cJSON_AddBoolToObject(e, "cached", false);
    char *es = cJSON_PrintUnformatted(e);
    cJSON_Delete(e);
    if (!es) { w->failed = true; return; }
    if (fprintf(w->f, "%s%s", w->count ? "," : "", es) < 0) w->failed = true;
    cJSON_free(es);
    w->count++;
}

esp_err_t podcast_refresh(int id, const char *name, const char *rss_url)
{
    return podcast_refresh_cancelable(id, name, rss_url, NULL);
}

esp_err_t podcast_refresh_cancelable(int id, const char *name, const char *rss_url,
                                     volatile bool *cancel)
{
    if (cancel && *cancel) return ESP_ERR_INVALID_STATE;
    int64_t deadline = esp_timer_get_time() + (int64_t)REFRESH_MAX_MS * 1000;
    rss_parser_t *p = malloc(sizeof(rss_parser_t));  // small now: no episode array
    void *ybuf = malloc(RSS_YXML_BUF_SIZE);
    if (!p || !ybuf) {
        free(p); free(ybuf);
        return ESP_ERR_NO_MEM;
    }

    // Write to a temp file and rename on success, so a failed refresh never
    // corrupts or truncates an existing manifest.
    mkdir("/littlefs/podcasts", 0775);
    char final_path[64], tmp_path[72];
    snprintf(final_path, sizeof(final_path), "/littlefs/podcasts/%d.json", id);
    snprintf(tmp_path, sizeof(tmp_path), "/littlefs/podcasts/%d.json.tmp", id);

    manifest_writer_t w = {0};
    w.name = name;
    w.rss_url = rss_url;
    w.p = p;
    // Cache-path dedup table (1.2 KB, PSRAM). Best effort: without it the writer
    // simply behaves as before, so a failed allocation must not fail the refresh.
    // One block holds both tables (cache_path hashes, then episode_url hashes).
    w.seen = heap_caps_malloc(2 * RSS_MAX_EPISODES * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    w.seen_url = w.seen ? w.seen + RSS_MAX_EPISODES : NULL;
    w.seen_max = w.seen ? RSS_MAX_EPISODES : 0;
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(w.generated_at, sizeof(w.generated_at), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    w.f = fopen(tmp_path, "w");
    if (!w.f) {
        ESP_LOGE(TAG, "cannot write %s", tmp_path);
        free(p); free(ybuf); free(w.seen);
        return ESP_FAIL;
    }
    rss_parse_init(p, ybuf, RSS_YXML_BUF_SIZE, mw_on_episode, &w);

    esp_http_client_config_t cfg = {
        .url = rss_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        // A feed is hundreds of KB: 512 B reads (the default buffer) make the
        // refresh many small TLS-record hops. 4 KB reads cut that overhead.
        .buffer_size = HTTP_CHUNK,
        .buffer_size_tx = 2048,  // long feed URLs, same headroom as the download path
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    char *chunk = heap_caps_malloc(HTTP_CHUNK, MALLOC_CAP_SPIRAM);
    bool net_ok = false;
    if (client && chunk && http_open_redirect(client, cancel, deadline) == 200) {
        net_ok = true;
        for (;;) {
            if (transfer_stopped(cancel, deadline)) {
                ESP_LOGW(TAG, "refresh exceeded %d ms, aborting", REFRESH_MAX_MS);
                net_ok = false;
                break;
            }
            int n = esp_http_client_read(client, chunk, HTTP_CHUNK);
            if (n < 0) { net_ok = false; break; }  // transport error / premature
                                                    // close: keep the old manifest
            if (transfer_stopped(cancel, deadline)) { net_ok = false; break; }
            if (n == 0) {
                net_ok = esp_http_client_is_complete_data_received(client) &&
                         yxml_eof(&p->x) == YXML_OK;
                break;
            }
            if (!rss_parse_feed(p, chunk, (size_t)n)) { net_ok = false; break; }
            if (p->emitted >= RSS_MAX_EPISODES) break;  // safety cap
        }
    } else {
        ESP_LOGE(TAG, "RSS fetch failed: %s", rss_url);
    }
    free(chunk);
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    esp_err_t ret = ESP_FAIL;
    bool good = net_ok && !w.failed && w.header_written && !transfer_stopped(cancel, deadline);
    // Position is load-bearing: after the parse (w.seen holds every cache_path the
    // feed emitted), while the array is still open and the OLD manifest is still
    // readable at final_path (the rename is below). A refresh that failed for any
    // other reason never gets here and keeps the old manifest whole, as before.
    if (good) mw_retain_dropped(&w, id);
    if (w.failed) good = false;
    if (good && fputs("]}", w.f) < 0) good = false;  // close the array + object
    // fclose flushes: on a full LittleFS it is where the write actually fails, so
    // a bad close must not promote a truncated manifest over the good one.
    if (fclose(w.f) != 0) good = false;
    if (transfer_stopped(cancel, deadline)) good = false;
    if (good) {
        // No remove() first: the manifest lives on LittleFS, whose rename replaces
        // the target atomically. Removing it opened a power-loss window where
        // neither the old nor the new manifest existed.
        if (rename(tmp_path, final_path) == 0) {
            ret = ESP_OK;
            ESP_LOGI(TAG, "manifest written: %d episodes", w.count);
            // The feed's artwork, once the manifest is safely in place: it is a
            // nicety, so it must never be able to cost us the episode list. A
            // cancel landing here (any new request sets the flag) leaves the
            // episodes written and only skips the cover.
            download_cover(p->image_url, w.podir, cancel, deadline);
        } else {
            ESP_LOGE(TAG, "cannot rename manifest into place");
            remove(tmp_path);
        }
    } else {
        remove(tmp_path);
        if (net_ok && !w.header_written) ESP_LOGW(TAG, "no episodes parsed from %s", rss_url);
    }

    free(p);
    free(ybuf);
    free(w.seen);
    return cancel && *cancel && ret != ESP_OK ? ESP_ERR_INVALID_STATE : ret;
}

// Download/copy chunk size. 16 KB (PSRAM) means fewer, larger SD writes and
// TLS reads, which speeds episode downloads and the MP3 trim copy.
#define DL_BUF_BYTES 16384

// Parse one MPEG audio frame header (4 bytes). Returns false if `h` is not a
// valid frame header. Fills the frame length in bytes and its duration in
// seconds. Covers MPEG 1/2/2.5 Layer I/II/III, which is all podcasts use.
static bool mp3_parse_frame(const uint8_t *h, int *frame_len, double *frame_secs)
{
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;  // frame sync
    int ver   = (h[1] >> 3) & 0x3;   // 0=2.5, 1=reserved, 2=MPEG2, 3=MPEG1
    int layer = (h[1] >> 1) & 0x3;   // 1=Layer III, 2=Layer II, 3=Layer I
    if (ver == 1 || layer == 0) return false;
    int br_idx = (h[2] >> 4) & 0xF;
    int sr_idx = (h[2] >> 2) & 0x3;
    int pad    = (h[2] >> 1) & 0x1;
    if (br_idx == 0 || br_idx == 15 || sr_idx == 3) return false;  // free/bad/reserved

    static const int br[2][3][16] = {
        { // MPEG1                                            (kbps)
            {0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0},  // Layer I
            {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0},     // Layer II
            {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0},      // Layer III
        },
        { // MPEG2 / MPEG2.5
            {0,32,48,56,64,80,96,112,128,144,160,176,192,224,256,0},     // Layer I
            {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0},          // Layer II
            {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0},          // Layer III
        },
    };
    static const int sr[4][3] = {
        {11025,12000,8000},   // MPEG2.5
        {0,0,0},              // reserved
        {22050,24000,16000},  // MPEG2
        {44100,48000,32000},  // MPEG1
    };
    int is_v1     = (ver == 3);
    int layer_idx = 3 - layer;  // Layer I->0, II->1, III->2
    int bitrate   = br[is_v1 ? 0 : 1][layer_idx][br_idx] * 1000;
    int rate      = sr[ver][sr_idx];
    if (bitrate == 0 || rate == 0) return false;
    int samples = (layer == 3) ? 384 : (layer == 2 ? 1152 : (is_v1 ? 1152 : 576));
    int len = (layer == 3) ? (12 * bitrate / rate + pad) * 4
                           : (samples / 8) * bitrate / rate + pad;
    if (len < 4) return false;
    if (frame_len)  *frame_len  = len;
    if (frame_secs) *frame_secs = (double)samples / rate;
    return true;
}

// Copy `in` to `out` starting at byte offset `from` to EOF. Returns ESP_OK.
static esp_err_t copy_from(FILE *fi, long from, const char *out, volatile bool *cancel)
{
    if (cancel && *cancel) return ESP_ERR_INVALID_STATE;
    if (fseek(fi, from, SEEK_SET) != 0) return ESP_FAIL;
    char *buf = heap_caps_malloc(DL_BUF_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) return ESP_ERR_NO_MEM;
    FILE *fo = fopen(out, "wb");
    if (!fo) { free(buf); return ESP_FAIL; }
    esp_err_t ret = ESP_OK;
    size_t total = 0;
    for (;;) {
        if (cancel && *cancel) { ret = ESP_ERR_INVALID_STATE; break; }
        size_t n = fread(buf, 1, DL_BUF_BYTES, fi);
        if (ferror(fi)) { ret = ESP_FAIL; break; }
        if (cancel && *cancel) { ret = ESP_ERR_INVALID_STATE; break; }
        if (n && fwrite(buf, 1, n, fo) != n) { ret = ESP_FAIL; break; }
        total += n;
        if (n < DL_BUF_BYTES) {
            if (!feof(fi) || total == 0) ret = ESP_FAIL;
            break;
        }
    }
    free(buf);
    if (fclose(fo) != 0) ret = ESP_FAIL;
    if (cancel && *cancel) ret = ESP_ERR_INVALID_STATE;
    return ret;
}

// Trim the first `skip_seconds` of an MP3 by dropping whole leading frames.
// Skips a leading ID3v2 tag, walks frame headers accumulating their durations,
// then copies from the first frame past the cut point to EOF into `out`.
// ESP_ERR_NOT_SUPPORTED if the stream is not parseable here or shorter than the
// skip (the caller then keeps the untrimmed file).
static esp_err_t mp3_trim_file(const char *in, const char *out, int skip_seconds,
                               volatile bool *cancel)
{
    if (cancel && *cancel) return ESP_ERR_INVALID_STATE;
    FILE *fi = fopen(in, "rb");
    if (!fi) return ESP_FAIL;
    esp_err_t ret = ESP_FAIL;
    if (fseek(fi, 0, SEEK_END) != 0) goto done;
    long size = ftell(fi);
    if (size < 0 || fseek(fi, 0, SEEK_SET) != 0) goto done;

    long start = 0;
    uint8_t hdr[10];
    size_t n = fread(hdr, 1, sizeof(hdr), fi);
    if (ferror(fi)) goto done;
    if (n == sizeof(hdr) && hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
        long tagsz = ((long)(hdr[6] & 0x7f) << 21) | ((long)(hdr[7] & 0x7f) << 14) |
                     ((long)(hdr[8] & 0x7f) << 7)  |  (long)(hdr[9] & 0x7f);
        start = 10 + tagsz;
        if (hdr[3] == 4 && (hdr[5] & 0x10)) start += 10;
    }

    if (fseek(fi, start, SEEK_SET) != 0) goto done;
    double acc = 0.0;
    for (;;) {
        if (cancel && *cancel) { ret = ESP_ERR_INVALID_STATE; break; }
        long pos = ftell(fi);
        if (pos < 0) break;
        uint8_t h[4];
        n = fread(h, 1, sizeof(h), fi);
        if (ferror(fi)) break;
        if (n != sizeof(h)) { ret = ESP_ERR_NOT_SUPPORTED; break; }
        int len; double secs;
        if (!mp3_parse_frame(h, &len, &secs) || pos > size || len > size - pos) {
            ret = ESP_ERR_NOT_SUPPORTED;
            break;
        }
        if (acc >= (double)skip_seconds) {
            ret = copy_from(fi, pos, out, cancel);
            break;
        }
        acc += secs;
        if (fseek(fi, pos + len, SEEK_SET) != 0) break;
    }
done:
    if (fclose(fi) != 0) ret = ESP_FAIL;
    if (cancel && *cancel) ret = ESP_ERR_INVALID_STATE;
    return ret;
}

esp_err_t podcast_download_episode(const podcast_episode_t *ep, int skip_seconds, volatile bool *cancel)
{
    // cache_path is absolute ("/sdcard/..."); source_sd_create wants it relative
    // to the SD root.
    if (cancel && *cancel) return ESP_ERR_INVALID_STATE;
    int64_t deadline = esp_timer_get_time() + (int64_t)EPISODE_MAX_MS * 1000;
    const char *prefix = "/sdcard/";
    if (strncmp(ep->cache_path, prefix, strlen(prefix)) != 0) return ESP_ERR_INVALID_ARG;
    const char *rel = ep->cache_path + strlen(prefix);

    char part_rel[PODCAST_PATH_MAX + 8];
    char part_abs[PODCAST_PATH_MAX + 8];
    snprintf(part_rel, sizeof(part_rel), "%s.part", rel);
    snprintf(part_abs, sizeof(part_abs), "%s.part", ep->cache_path);

    FILE *fo = source_sd_create(part_rel);  // creates parent dirs, validates path
    if (!fo) return ESP_FAIL;

    esp_http_client_config_t cfg = {
        .url = ep->episode_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 4096,     // larger TLS reads speed the download
        .buffer_size_tx = 2048,  // long Radio France episode URLs
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { fclose(fo); remove(part_abs); return ESP_ERR_NO_MEM; }

    bool ok = false;
    if (http_open_redirect(client, cancel, deadline) == 200) {
        char *buf = heap_caps_malloc(DL_BUF_BYTES, MALLOC_CAP_SPIRAM);
        if (buf) {
            ok = true;
            uint64_t total = 0;
            for (;;) {
                if (cancel && *cancel) { ok = false; break; }
                if (esp_timer_get_time() > deadline) {
                    ESP_LOGW(TAG, "episode download exceeded %d ms, aborting", EPISODE_MAX_MS);
                    ok = false;
                    break;
                }
                int n = esp_http_client_read(client, buf, DL_BUF_BYTES);
                if (n < 0) { ok = false; break; }
                if (transfer_stopped(cancel, deadline)) { ok = false; break; }
                if (n == 0) {
                    ok = total > 0 && esp_http_client_is_complete_data_received(client);
                    break;
                }
                total += (uint64_t)n;
                if (total > EPISODE_MAX_BYTES) {
                    ESP_LOGW(TAG, "episode download over %llu bytes, aborting",
                             (unsigned long long)EPISODE_MAX_BYTES);
                    ok = false;
                    break;
                }
                if (fwrite(buf, 1, (size_t)n, fo) != (size_t)n) { ok = false; break; }
            }
            free(buf);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    // The final flush can fail on a full card: promoting the file then would
    // cache a truncated episode that plays short forever.
    if (fclose(fo) != 0) ok = false;

    if (!ok || (cancel && *cancel)) {
        remove(part_abs);
        return cancel && *cancel ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }

    // Finalize: physically trim the intro only for MP3 (frame-based). FLAC and
    // AAC/.m4a are saved as-is; their intro is skipped at playback instead.
    size_t pl = strlen(ep->cache_path);
    bool is_mp3 = pl >= 4 && strcasecmp(ep->cache_path + pl - 4, ".mp3") == 0;
    char trim_abs[PODCAST_PATH_MAX + 16];
    snprintf(trim_abs, sizeof(trim_abs), "%s.trim.part", ep->cache_path);
    const char *ready = part_abs;
    if (is_mp3 && skip_seconds > 0) {
        esp_err_t err = mp3_trim_file(part_abs, trim_abs, skip_seconds, cancel);
        if (err == ESP_OK) {
            ready = trim_abs;
        } else {
            remove(trim_abs);
            if (err != ESP_ERR_NOT_SUPPORTED) {
                remove(part_abs);
                return err;
            }
            ESP_LOGW(TAG, "intro trim skipped, keeping full file: %s", ep->cache_path);
        }
    }
    if (cancel && *cancel) {
        remove(part_abs);
        if (ready == trim_abs) remove(trim_abs);
        return ESP_ERR_INVALID_STATE;
    }
    if (remove(ep->cache_path) != 0 && errno != ENOENT) return ESP_FAIL;
    if (rename(ready, ep->cache_path) != 0) return ESP_FAIL;
    if (ready == trim_abs) remove(part_abs);
    return ESP_OK;
}

// One episode JSON object is parsed at a time, so the whole manifest is never
// loaded into RAM (it can hold hundreds of episodes). Worst-case object size:
// title + date + url(512) + cache_path(256) + keys, well under this.
#define PODCAST_OBJ_MAX 2048

// Open the manifest and position the stream just past the '[' of the episodes
// array. Returns NULL if there is no manifest or no episodes array.
static FILE *manifest_open_array(int id)
{
    char path[64];
    snprintf(path, sizeof(path), "/littlefs/podcasts/%d.json", id);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    // Find the "episodes" key. A value can never contain the bytes "episodes"
    // (a closing JSON quote is preceded by the string content, and escaping makes
    // an embedded quote \"), so a plain scan is safe.
    static const char key[] = "\"episodes\"";
    int ki = 0, c;
    while ((c = fgetc(f)) != EOF) {
        if (c == key[ki]) { if (!key[++ki]) break; }
        else ki = (c == key[0]) ? 1 : 0;
    }
    if (c == EOF) { fclose(f); return NULL; }
    while ((c = fgetc(f)) != EOF) if (c == '[') break;  // skip ':' and any whitespace
    if (c == EOF) { fclose(f); return NULL; }
    return f;
}

// Read the next {...} object from the array into buf. Returns 1 on an object,
// 0 at the end of the array (']'), -1 on error or overflow. Tracks JSON strings
// and escapes so braces inside string values do not confuse the depth count.
static int manifest_next_object(FILE *f, char *buf, size_t bufsz)
{
    int c;
    while ((c = fgetc(f)) != EOF && c != '{') {
        if (c == ']') return 0;  // end of the episodes array
    }
    if (c == EOF) return 0;
    size_t n = 0;
    buf[n++] = '{';
    int depth = 1;
    bool instr = false, esc = false;
    while ((c = fgetc(f)) != EOF) {
        if (n + 1 >= bufsz) return -1;
        buf[n++] = (char)c;
        if (esc) { esc = false; continue; }
        if (instr) {
            if (c == '\\') esc = true;
            else if (c == '"') instr = false;
            continue;
        }
        if (c == '"') instr = true;
        else if (c == '{') depth++;
        else if (c == '}' && --depth == 0) { buf[n] = '\0'; return 1; }
    }
    return -1;  // truncated object
}

// Episodes the feed no longer lists: carry them over from the previous manifest
// while their audio is on the card, so a downloaded episode never disappears
// from the list (the file was never deleted, it just became unreachable). The
// object text is copied VERBATIM: recomputing cache_path would move the
// collision suffix, which is the episode's position in the feed, and orphan the
// very file being kept. It also keeps the played and resume keys valid.
//
// Without a card the presence check is impossible, so only an entry a previous
// refresh already marked survives. "retained" is written here and nowhere else,
// so it can only come from a refresh that saw the file: a card pulled out for
// one refresh costs nothing, and a device that never had one never accumulates
// rows for episodes it does not hold.
//
// Best effort throughout: anything missing or failing on the READ side leaves
// the refresh exactly as it would have been. Only a failed WRITE to the temp
// manifest is fatal (a half-written object would be promoted otherwise).
// True when the manifest file for this id was written for the same feed. The
// web page gives a new podcast id max+1, so deleting the last podcast and
// adding another reuses its id, and its old manifest is still on flash: without
// this check the new feed inherited every downloaded episode of the deleted one.
// Reads the header (everything before "episodes") into buf and parses it; an
// unreadable or unrecognizable header counts as "not the same feed".
static bool manifest_same_feed(int id, const char *rss_url, char *buf, size_t bufsz)
{
    char path[64];
    snprintf(path, sizeof(path), "/littlefs/podcasts/%d.json", id);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, bufsz - 1, f);
    fclose(f);  // read-only
    buf[n] = '\0';
    // Same argument as manifest_open_array: the bytes "episodes" with their
    // quotes can only be the key, never inside an escaped string value.
    char *k = strstr(buf, "\"episodes\"");
    if (!k) return false;
    while (k > buf && k[-1] != ',') k--;
    if (k == buf) return false;
    k[-1] = '}';  // the header object, closed where "episodes" started
    *k = '\0';
    cJSON *h = cJSON_Parse(buf);
    const cJSON *u = h ? cJSON_GetObjectItemCaseSensitive(h, "rss_url") : NULL;
    bool same = u && cJSON_IsString(u) && strcmp(u->valuestring, rss_url ? rss_url : "") == 0;
    cJSON_Delete(h);
    return same;
}

static void mw_retain_dropped(manifest_writer_t *w, int id)
{
    if (!w->seen) return;  // no "already emitted" set: a duplicate row is worse
    char *obj = heap_caps_malloc(PODCAST_OBJ_MAX, MALLOC_CAP_SPIRAM);
    if (!obj) return;
    if (!manifest_same_feed(id, w->rss_url, obj, PODCAST_OBJ_MAX)) { free(obj); return; }
    FILE *f = manifest_open_array(id);
    if (!f) { free(obj); return; }  // no previous manifest, or an unusable one

    bool sd = source_sd_present();
    int kept = 0, over_cap = 0;
    while (manifest_next_object(f, obj, PODCAST_OBJ_MAX) == 1) {
        cJSON *e = cJSON_Parse(obj);
        if (!e) continue;  // skip a malformed object, same as the reader
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(e, "cache_path");
        char path[PODCAST_PATH_MAX] = "";
        if (v && cJSON_IsString(v)) strlcpy(path, v->valuestring, sizeof(path));
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(e, "episode_url");
        uint32_t uh = path_hash(u && cJSON_IsString(u) ? u->valuestring : "");
        // Read the marker from the PARSED object, never from a substring of the
        // text: titles are untrusted RSS and one containing the word "retained"
        // would fake a marker the entry does not have.
        bool marked = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "retained"));
        cJSON_Delete(e);
        if (path[0] == '\0') continue;

        uint32_t h = path_hash(path);
        int seen_n = w->count < w->seen_max ? w->count : w->seen_max;
        bool in_feed = false;
        // Still in the feed, already written this round: same path, or same
        // episode URL under a path that changed (e.g. a pre-2026-09-23
        // position suffix; that old file is orphaned, a re-download follows).
        for (int i = 0; i < seen_n && !in_feed; i++)
            in_feed = w->seen[i] == h || w->seen_url[i] == uh;
        if (in_feed) continue;

        if (sd) {
            struct stat st;
            if (stat(path, &st) != 0 || st.st_size <= 0) continue;
        } else if (!marked) {
            continue;
        }
        if (w->count >= PODCAST_MAX_EPISODES) { over_cap++; continue; }

        size_t len = strlen(obj);
        if (len < 2 || obj[len - 1] != '}') continue;
        int r = fprintf(w->f, "%s", w->count ? "," : "");
        if (r >= 0) {
            r = marked ? fprintf(w->f, "%s", obj)
                       : fprintf(w->f, "%.*s,\"retained\":true}", (int)len - 1, obj);
        }
        if (r < 0) { w->failed = true; break; }
        if (w->count < w->seen_max) {
            w->seen[w->count] = h;
            w->seen_url[w->count] = uh;
        }
        w->count++;
        kept++;
    }

    free(obj);
    fclose(f);  // read-only: a failed close here must not cost the refresh
    if (kept) ESP_LOGI(TAG, "kept %d downloaded episode(s) no longer in the feed", kept);
    if (over_cap) ESP_LOGW(TAG, "%d downloaded episode(s) dropped: manifest full", over_cap);
}

static bool eps_grow(podcast_episode_t **eps, size_t *cap, size_t need)
{
    if (*eps && *cap >= need) return true;
    size_t nc = *cap ? *cap * 2 : 16;
    if (nc < need) nc = need;
    podcast_episode_t *ne = heap_caps_realloc(*eps, sizeof(**eps) * nc, MALLOC_CAP_SPIRAM);
    if (!ne) return false;
    *eps = ne;
    *cap = nc;
    return true;
}

// One readdir of the feed's cache folder replaces one stat() per episode.
// Reading the manifest runs on the LVGL task (build_episodes), where hundreds
// of SD round trips stalled rendering for a visible fraction of a second on
// large feeds. File names are matched by FNV-1a hash against the listing; an
// episode whose name IS present still gets its real stat() once, so "cached"
// keeps meaning "exists with a non-zero size". Episodes living outside the
// scanned folder (legacy manifests, changed feed title) fall back to their own
// stat(). The scan state is per-call: read_manifest can run on the UI task and
// on the download worker concurrently.
#define CACHE_NAMES_MAX 512

typedef struct {
    char     dir[PODCAST_PATH_MAX];  // folder that was scanned ("" = none)
    uint64_t *names;                 // FNV-1a of each entry name (PSRAM)
    int      n;
    bool     complete;
} cache_scan_t;

static uint64_t fnv1a64(const char *s)
{
    uint64_t h = 1469598103934665603ULL;  // FNV-1a 64-bit offset basis
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;  // FNV prime
    }
    return h;
}

static void cache_scan_start(cache_scan_t *cs, const char *dir)
{
    memset(cs, 0, sizeof(*cs));
    if (dir[0] == '\0') return;
    cs->names = heap_caps_malloc(sizeof(*cs->names) * CACHE_NAMES_MAX, MALLOC_CAP_SPIRAM);
    if (!cs->names) return;
    DIR *d = opendir(dir);
    if (!d) {
        free(cs->names);  // no SD or no folder: every episode falls back to stat()
        cs->names = NULL;
        return;
    }
    strlcpy(cs->dir, dir, sizeof(cs->dir));
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e) { cs->complete = errno == 0; break; }
        if (cs->n == CACHE_NAMES_MAX) break;
        cs->names[cs->n++] = fnv1a64(e->d_name);
    }
    if (closedir(d) != 0) cs->complete = false;
}

static void cache_scan_done(cache_scan_t *cs)
{
    free(cs->names);
    cs->names = NULL;
}

static bool cache_scan_has(const cache_scan_t *cs, const char *name)
{
    uint64_t h = fnv1a64(name);
    for (int i = 0; i < cs->n; i++) {
        if (cs->names[i] == h) return true;
    }
    return false;
}

// Cached iff the file exists non-empty (same semantics as before). Fast path:
// the name is absent from the one-shot directory scan, so no SD round trip.
static bool ep_is_cached(const cache_scan_t *cs, const podcast_episode_t *ep)
{
    if (ep->cache_path[0] == '\0') return false;
    if (cs->names && cs->complete) {
        size_t dl = strlen(cs->dir);
        bool in_dir = strncmp(ep->cache_path, cs->dir, dl) == 0 && ep->cache_path[dl] == '/' &&
                      strchr(ep->cache_path + dl + 1, '/') == NULL;
        if (in_dir) {
            const char *slash = strrchr(ep->cache_path, '/');
            if (!cache_scan_has(cs, slash ? slash + 1 : ep->cache_path)) return false;
        }
    }
    struct stat st;
    return stat(ep->cache_path, &st) == 0 && st.st_size > 0;
}

esp_err_t podcast_read_manifest(int id, podcast_episode_t **eps, size_t *cap, size_t *count)
{
    *count = 0;
    FILE *f = manifest_open_array(id);
    if (!f) return ESP_ERR_NOT_FOUND;
    char *obj = heap_caps_malloc(PODCAST_OBJ_MAX, MALLOC_CAP_SPIRAM);
    if (!obj) { fclose(f); return ESP_ERR_NO_MEM; }

    esp_err_t err = ESP_OK;
    size_t n = 0;
    int r;
    while ((r = manifest_next_object(f, obj, PODCAST_OBJ_MAX)) == 1) {
        if (!eps_grow(eps, cap, n + 1)) { err = ESP_ERR_NO_MEM; break; }
        cJSON *e = cJSON_Parse(obj);
        if (!e) continue;  // skip a malformed object
        podcast_episode_t *out = &(*eps)[n];
        memset(out, 0, sizeof(*out));
        const cJSON *v;
        if ((v = cJSON_GetObjectItemCaseSensitive(e, "title")) && cJSON_IsString(v))
            strlcpy(out->title, v->valuestring, sizeof(out->title));
        if ((v = cJSON_GetObjectItemCaseSensitive(e, "date")) && cJSON_IsString(v))
            strlcpy(out->date, v->valuestring, sizeof(out->date));
        if ((v = cJSON_GetObjectItemCaseSensitive(e, "duration_seconds")) && cJSON_IsNumber(v))
            out->duration_seconds = v->valueint;
        if ((v = cJSON_GetObjectItemCaseSensitive(e, "episode_url")) && cJSON_IsString(v))
            strlcpy(out->episode_url, v->valuestring, sizeof(out->episode_url));
        if ((v = cJSON_GetObjectItemCaseSensitive(e, "cache_path")) && cJSON_IsString(v))
            strlcpy(out->cache_path, v->valuestring, sizeof(out->cache_path));
        cJSON_Delete(e);
        n++;
    }
    free(obj);
    fclose(f);

    // Trust the filesystem, not any stored flag: the file is present iff it was
    // downloaded. This survives manifest rewrites by a refresh. The scanned
    // folder comes from the first episode's cache_path: the writer puts every
    // episode of a feed in one folder (/sdcard/podcasts/<feed>/).
    cache_scan_t cs;
    char dir[PODCAST_PATH_MAX] = "";
    if (n > 0 && (*eps)[0].cache_path[0] != '\0') {
        const char *slash = strrchr((*eps)[0].cache_path, '/');
        if (slash) {
            snprintf(dir, sizeof(dir), "%.*s", (int)(slash - (*eps)[0].cache_path),
                     (*eps)[0].cache_path);
        }
    }
    cache_scan_start(&cs, dir);
    for (size_t i = 0; i < n; i++) {
        (*eps)[i].cached = ep_is_cached(&cs, &(*eps)[i]);
    }
    cache_scan_done(&cs);

    *count = n;
    return err;
}

size_t podcast_manifest_count(int id)
{
    FILE *f = manifest_open_array(id);
    if (!f) return 0;
    char *obj = heap_caps_malloc(PODCAST_OBJ_MAX, MALLOC_CAP_SPIRAM);
    if (!obj) { fclose(f); return 0; }
    size_t n = 0;
    while (manifest_next_object(f, obj, PODCAST_OBJ_MAX) == 1) n++;
    free(obj);
    fclose(f);
    return n;
}

// ---- Channel artwork ----

// Biggest cover we download. Feed images are commonly 1400x1400 JPEGs of a few
// hundred KB; anything past this is refused rather than filling the card (the
// URL comes from an untrusted feed, same reasoning as EPISODE_MAX_BYTES).
#define COVER_MAX_BYTES (1024 * 1024)

void podcast_cover_path(int id, const char *name, char *out, size_t out_size)
{
    (void)id;  // the folder is named after the podcast, like the episodes'
    char podir[80];
    sanitize_name((name && name[0]) ? name : "podcast", podir, sizeof(podir), "podcast");
    snprintf(out, out_size, "/sdcard/podcasts/%s/cover.jpg", podir);
}

// Fetch the feed's cover next to its episodes. Best effort: any failure leaves
// no file and is not reported, a refresh must still succeed without artwork.
// Skipped when a cover is already there (feeds rarely change their image, and
// a refresh runs on every auto-maintenance pass).
static void download_cover(const char *url, const char *podir, volatile bool *cancel,
                           int64_t deadline)
{
    if (transfer_stopped(cancel, deadline) || !url || !url[0] || !podir || !podir[0]) return;

    char abs_path[PODCAST_PATH_MAX];
    char rel[PODCAST_PATH_MAX];
    snprintf(abs_path, sizeof(abs_path), "/sdcard/podcasts/%s/cover.jpg", podir);
    snprintf(rel, sizeof(rel), "podcasts/%s/cover.jpg.part", podir);
    struct stat st;
    if (stat(abs_path, &st) == 0 && st.st_size > 0) return;  // already cached

    char part_abs[PODCAST_PATH_MAX + 8];
    snprintf(part_abs, sizeof(part_abs), "%s.part", abs_path);
    FILE *fo = source_sd_create(rel);
    if (!fo) return;

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,  // feed image URLs are long too
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { fclose(fo); remove(part_abs); return; }

    bool ok = false;
    if (http_open_redirect(client, cancel, deadline) == 200) {
        char *buf = heap_caps_malloc(HTTP_CHUNK, MALLOC_CAP_SPIRAM);
        if (buf) {
            size_t total = 0;
            ok = true;
            for (;;) {
                if (transfer_stopped(cancel, deadline)) { ok = false; break; }
                int n = esp_http_client_read(client, buf, HTTP_CHUNK);
                if (n < 0 || transfer_stopped(cancel, deadline)) { ok = false; break; }
                if (n == 0) {
                    ok = esp_http_client_is_complete_data_received(client);
                    break;
                }
                total += (size_t)n;
                if (total > COVER_MAX_BYTES) { ok = false; break; }
                if (fwrite(buf, 1, (size_t)n, fo) != (size_t)n) { ok = false; break; }
            }
            if (total == 0) ok = false;
            free(buf);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (fclose(fo) != 0) ok = false;
    if (transfer_stopped(cancel, deadline)) ok = false;
    if (ok && remove(abs_path) != 0 && errno != ENOENT) ok = false;
    if (ok && rename(part_abs, abs_path) == 0) {
        ESP_LOGI(TAG, "cover cached: %s", abs_path);
    } else {
        remove(part_abs);
    }
}
