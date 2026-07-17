// podcast: fetch an RSS feed over HTTP(S), parse it with the rss_parse core,
// and write a bounded manifest to SD. Reads the manifest back for the UI.
#include "podcast.h"
#include "rss_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
#define HTTP_TIMEOUT_MS 15000
#define MAX_REDIRECTS   6
// Hard wall-clock cap on a refresh. Per-read timeouts (HTTP_TIMEOUT_MS) bound a
// single stalled read, but a server that trickles bytes could keep the loop
// going for a long time. This guarantees the refresh always terminates.
#define REFRESH_MAX_MS  30000

// Open the client, following redirects. Returns the final status code or -1.
static int http_open_redirect(esp_http_client_handle_t client)
{
    for (int i = 0; i < MAX_REDIRECTS; i++) {
        if (esp_http_client_open(client, 0) != ESP_OK) return -1;
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            esp_http_client_set_redirection(client);
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
} manifest_writer_t;

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

// rss_parse callback: append one episode object to the manifest file.
static void mw_on_episode(const rss_episode_t *ep, void *ctx)
{
    manifest_writer_t *w = ctx;
    if (w->failed || !w->f) return;
    if (!w->header_written) mw_write_header(w);
    if (w->failed) return;

    const char *url = ep->url;
    const char *ext = strstr(url, ".flac") ? "flac"
                    : (strstr(url, ".m4a") || strstr(url, ".mp4") || strstr(url, ".aac")) ? "m4a"
                    : strstr(url, ".opus") ? "opus"
                    : (strstr(url, ".ogg") || strstr(url, ".oga")) ? "ogg"
                    : "mp3";
    char epname[128];
    char fallback[16];
    snprintf(fallback, sizeof(fallback), "episode_%d", w->count);
    sanitize_name(ep->title, epname, sizeof(epname), fallback);
    char cache_path[PODCAST_PATH_MAX];
    snprintf(cache_path, sizeof(cache_path), "/sdcard/podcasts/%s/%s.%s", w->podir, epname, ext);

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
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(w.generated_at, sizeof(w.generated_at), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    w.f = fopen(tmp_path, "w");
    if (!w.f) {
        ESP_LOGE(TAG, "cannot write %s", tmp_path);
        free(p); free(ybuf);
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
    if (client && chunk && http_open_redirect(client) == 200) {
        net_ok = true;
        int64_t deadline = esp_timer_get_time() + (int64_t)REFRESH_MAX_MS * 1000;
        for (;;) {
            if (esp_timer_get_time() > deadline) {
                ESP_LOGW(TAG, "refresh exceeded %d ms, aborting", REFRESH_MAX_MS);
                net_ok = false;
                break;
            }
            int n = esp_http_client_read(client, chunk, HTTP_CHUNK);
            if (n < 0) { net_ok = false; break; }  // transport error / premature
                                                    // close: keep the old manifest
            if (n == 0) break;                      // clean EOF
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
    bool good = net_ok && !w.failed && w.header_written;
    if (good && fputs("]}", w.f) < 0) good = false;  // close the array + object
    fclose(w.f);
    if (good) {
        remove(final_path);
        if (rename(tmp_path, final_path) == 0) {
            ret = ESP_OK;
            ESP_LOGI(TAG, "manifest written: %d episodes", w.count);
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
    return ret;
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
static esp_err_t copy_from(FILE *fi, long from, const char *out)
{
    FILE *fo = fopen(out, "wb");
    if (!fo) return ESP_FAIL;
    char *buf = heap_caps_malloc(DL_BUF_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(fo); return ESP_ERR_NO_MEM; }
    fseek(fi, from, SEEK_SET);
    esp_err_t ret = ESP_OK;
    for (;;) {
        size_t n = fread(buf, 1, DL_BUF_BYTES, fi);
        if (n == 0) break;
        if (fwrite(buf, 1, n, fo) != n) { ret = ESP_FAIL; break; }
    }
    free(buf);
    fclose(fo);
    return ret;
}

// Trim the first `skip_seconds` of an MP3 by dropping whole leading frames.
// Skips a leading ID3v2 tag, walks frame headers accumulating their durations,
// then copies from the first frame past the cut point to EOF into `out`.
// ESP_ERR_NOT_SUPPORTED if the stream is not parseable here or shorter than the
// skip (the caller then keeps the untrimmed file).
static esp_err_t mp3_trim_file(const char *in, const char *out, int skip_seconds)
{
    FILE *fi = fopen(in, "rb");
    if (!fi) return ESP_FAIL;

    long start = 0;
    uint8_t hdr[10];
    if (fread(hdr, 1, 10, fi) == 10 && hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
        long tagsz = ((long)(hdr[6] & 0x7f) << 21) | ((long)(hdr[7] & 0x7f) << 14) |
                     ((long)(hdr[8] & 0x7f) << 7)  |  (long)(hdr[9] & 0x7f);
        start = 10 + tagsz;
    }

    long cut = 0;
    double acc = 0.0;
    bool found = false;
    fseek(fi, start, SEEK_SET);
    for (;;) {
        long pos = ftell(fi);
        uint8_t h[4];
        if (fread(h, 1, 4, fi) != 4) break;  // EOF before reaching the skip
        int len; double secs;
        if (!mp3_parse_frame(h, &len, &secs)) { fclose(fi); return ESP_ERR_NOT_SUPPORTED; }
        if (acc >= (double)skip_seconds) { cut = pos; found = true; break; }
        acc += secs;
        if (fseek(fi, pos + len, SEEK_SET) != 0) break;
    }
    if (!found) { fclose(fi); return ESP_ERR_NOT_SUPPORTED; }

    esp_err_t ret = copy_from(fi, cut, out);
    fclose(fi);
    return ret;
}

bool podcast_episode_cached(const podcast_episode_t *ep)
{
    struct stat st;
    return ep->cache_path[0] != '\0' && stat(ep->cache_path, &st) == 0 && st.st_size > 0;
}

esp_err_t podcast_download_episode(const podcast_episode_t *ep, int skip_seconds, volatile bool *cancel)
{
    // cache_path is absolute ("/sdcard/..."); source_sd_create wants it relative
    // to the SD root.
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

    bool ok = false, aborted = false;
    if (http_open_redirect(client) == 200) {
        char *buf = heap_caps_malloc(DL_BUF_BYTES, MALLOC_CAP_SPIRAM);
        if (buf) {
            ok = true;
            for (;;) {
                if (cancel && *cancel) { ok = false; aborted = true; break; }
                int n = esp_http_client_read(client, buf, DL_BUF_BYTES);
                if (n < 0) { ok = false; break; }
                if (n == 0) break;  // end of body
                if (fwrite(buf, 1, (size_t)n, fo) != (size_t)n) { ok = false; break; }
            }
            free(buf);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    fclose(fo);

    if (!ok) {
        remove(part_abs);
        return aborted ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }

    // Finalize: physically trim the intro only for MP3 (frame-based). FLAC and
    // AAC/.m4a are saved as-is; their intro is skipped at playback instead.
    size_t pl = strlen(ep->cache_path);
    bool is_mp3 = pl >= 4 && strcasecmp(ep->cache_path + pl - 4, ".mp3") == 0;
    if (is_mp3 && skip_seconds > 0) {
        if (mp3_trim_file(part_abs, ep->cache_path, skip_seconds) == ESP_OK) {
            remove(part_abs);
            return ESP_OK;
        }
        // Not trimmable (unparseable, or shorter than the skip): keep the file.
        ESP_LOGW(TAG, "intro trim skipped, keeping full file: %s", ep->cache_path);
    }
    remove(ep->cache_path);  // drop any stale file before the rename
    if (rename(part_abs, ep->cache_path) != 0) { remove(part_abs); return ESP_FAIL; }
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

esp_err_t podcast_read_manifest(int id, podcast_episode_t *eps, size_t max, size_t *count)
{
    *count = 0;
    FILE *f = manifest_open_array(id);
    if (!f) return ESP_ERR_NOT_FOUND;
    char *obj = malloc(PODCAST_OBJ_MAX);
    if (!obj) { fclose(f); return ESP_ERR_NO_MEM; }

    size_t n = 0;
    int r;
    while (n < max && (r = manifest_next_object(f, obj, PODCAST_OBJ_MAX)) == 1) {
        cJSON *e = cJSON_Parse(obj);
        if (!e) continue;  // skip a malformed object
        podcast_episode_t *out = &eps[n];
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
        // Trust the filesystem, not the stored flag: the file is present iff it
        // was downloaded. This survives manifest rewrites by a refresh.
        out->cached = podcast_episode_cached(out);
        cJSON_Delete(e);
        n++;
    }
    free(obj);
    fclose(f);
    *count = n;
    return ESP_OK;
}

size_t podcast_manifest_count(int id)
{
    FILE *f = manifest_open_array(id);
    if (!f) return 0;
    char *obj = malloc(PODCAST_OBJ_MAX);
    if (!obj) { fclose(f); return 0; }
    size_t n = 0;
    while (manifest_next_object(f, obj, PODCAST_OBJ_MAX) == 1) n++;
    free(obj);
    fclose(f);
    return n;
}
