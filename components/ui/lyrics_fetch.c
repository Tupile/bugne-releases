// lyrics_fetch: looks a song's lyrics up, on its own task. See lyrics.h.
//
// Order: the sidecar files next to an SD track (.lrc, then .txt), then
// LRCLIB. A network result for an SD track is saved as a sidecar, so the
// next listening works offline.
//
// The task has a PSRAM stack: it never touches internal flash (NVS, LittleFS,
// OTA). It reads and writes the SD card only, like the library scan task.
// Every buffer here is PSRAM. Nothing logs lyric text, only counts.
#include "lyrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"  // xTaskCreateWithCaps (PSRAM stack)
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_util.h"
#include "net.h"
#include "source_sd.h"

static const char *TAG = "lyrics";

#define TASK_STACK      8192            // does its own TLS handshake
#define BODY_CAP        (128 * 1024)    // the answer always carries a large "lyricsfile" copy
#define HTTP_TIMEOUT_MS 8000
#define RETRY_GAP_MS    500             // LRCLIB asks for 200 to 500 ms between requests
#define FIELD_MAX       96              // title / artist (Sendspin fields are 96 bytes)
#define PATH_MAX_LEN    320             // "/sdcard/" + a 256-byte library path + extension
#define PROJECT_URL     "https://github.com/Tupile/bugne-releases"
#define JSON_DEPTH_MAX  8               // the real answer nests 1 level; cJSON recurses on our 8 KB stack

typedef struct {
    uint64_t key;
    uint32_t gen;
    uint32_t dur_ms;
    char title[FIELD_MAX];
    char artist[FIELD_MAX];
    char path[PATH_MAX_LEN];  // "" when the song is not an SD file
} request_t;

static SemaphoreHandle_t s_lock;      // guards everything below
static SemaphoreHandle_t s_wake;      // given when a request is pending
static bool s_task_started;
static request_t s_want;
static bool s_pending;
static uint32_t s_gen;                // bumped by every request and cancel
static lyrics_state_t s_state = LYRICS_IDLE;
static uint64_t s_state_key;
static lyrics_doc_t *s_doc;           // result waiting for lyrics_take
static int64_t s_backoff_until_us;    // after a 429: no request before this

void lyrics_doc_free(lyrics_doc_t *doc)
{
    if (!doc) return;
    free(doc->lines);
    free(doc->text);
    free(doc);
}

static void *psram_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

// A plain-text result. src is copied (bounded, cleaned).
static lyrics_doc_t *doc_plain(uint64_t key, const char *src)
{
    lyrics_doc_t *d = heap_caps_calloc(1, sizeof(*d), MALLOC_CAP_SPIRAM);
    char *text = psram_alloc(LYRICS_TEXT_MAX);
    if (!d || !text) { free(d); free(text); return NULL; }
    lyrics_copy_text(text, LYRICS_TEXT_MAX, src);
    d->key = key;
    d->text = text;
    return d;
}

// A synced result from LRC text. When no line has a valid stamp, the text is
// kept as plain text instead (a hand-made .lrc may be plain lines).
static lyrics_doc_t *doc_from_lrc(uint64_t key, const char *lrc)
{
    lyrics_doc_t *d = doc_plain(key, lrc);
    if (!d) return NULL;
    lyrics_line_t *lines = psram_alloc(sizeof(*lines) * LYRICS_LINES_MAX);
    if (!lines) return d;  // still usable as plain text
    size_t n = lyrics_parse_lrc(d->text, lines, LYRICS_LINES_MAX);
    if (n == 0) {
        free(lines);
        lyrics_copy_text(d->text, LYRICS_TEXT_MAX, lrc);  // the parse cut the buffer
        return d;
    }
    d->synced = true;
    d->lines = lines;
    d->line_count = n;
    return d;
}

// The words of a synced text whose timing cannot be trusted (another version
// of the song): the lines without their stamps, as plain text.
static lyrics_doc_t *doc_lrc_as_plain(uint64_t key, const char *lrc)
{
    lyrics_doc_t *d = doc_from_lrc(key, lrc);
    if (!d || !d->synced) return d;
    char *flat = psram_alloc(LYRICS_TEXT_MAX);
    if (!flat) { lyrics_doc_free(d); return NULL; }
    size_t o = 0;
    for (size_t i = 0; i < d->line_count; i++) {
        size_t len = strlen(d->lines[i].text);
        if (o + len + 2 > LYRICS_TEXT_MAX) break;
        memcpy(flat + o, d->lines[i].text, len);
        o += len;
        flat[o++] = '\n';
    }
    flat[o] = '\0';
    free(d->lines);
    free(d->text);
    d->lines = NULL;
    d->line_count = 0;
    d->synced = false;
    d->text = flat;
    return d;
}

// ---- Sidecar files ---------------------------------------------------------

// Read a whole small text file, NUL-terminated, at most LYRICS_TEXT_MAX - 1
// bytes. NULL when it does not exist or is empty.
static char *read_sidecar(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *buf = psram_alloc(LYRICS_TEXT_MAX);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, LYRICS_TEXT_MAX - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (n == 0) { free(buf); return NULL; }
    return buf;
}

static lyrics_doc_t *load_sidecar(const request_t *r)
{
    if (!r->path[0] || !source_sd_present()) return NULL;
    char path[PATH_MAX_LEN];
    lyrics_doc_t *d = NULL;
    if (lyrics_sidecar_path(r->path, ".lrc", path, sizeof(path))) {
        char *text = read_sidecar(path);
        if (text) {
            d = doc_from_lrc(r->key, text);  // a sidecar .lrc is trusted
            free(text);
        }
    }
    if (!d && lyrics_sidecar_path(r->path, ".txt", path, sizeof(path))) {
        char *text = read_sidecar(path);
        if (text) {
            d = doc_plain(r->key, text);
            free(text);
        }
    }
    return d;
}

// Save a network result next to the track. Best effort: a failure is logged
// and the lyrics still show. An existing sidecar is never overwritten: a file
// put there by hand wins.
static void save_sidecar(const request_t *r, const char *ext, const char *raw)
{
    if (!r->path[0] || !source_sd_present()) return;
    char target[PATH_MAX_LEN], part[PATH_MAX_LEN], part_ext[16];
    snprintf(part_ext, sizeof(part_ext), "%s.part", ext);
    if (!lyrics_sidecar_path(r->path, ext, target, sizeof(target)) ||
        !lyrics_sidecar_path(r->path, part_ext, part, sizeof(part))) {
        ESP_LOGW(TAG, "sidecar path too long, not saved");
        return;
    }
    struct stat st;
    if (stat(target, &st) == 0) return;

    char *text = psram_alloc(LYRICS_TEXT_MAX);
    if (!text) return;
    size_t len = lyrics_copy_text(text, LYRICS_TEXT_MAX, raw);
    FILE *f = fopen(part, "w");
    if (!f) {
        ESP_LOGW(TAG, "sidecar not saved: cannot create the file");
        free(text);
        return;
    }
    bool ok = fwrite(text, 1, len, f) == len;
    // The final flush is where a full card fails: check fclose before the rename.
    if (fclose(f) != 0) ok = false;
    free(text);
    // The target does not exist (checked above), so no remove before the
    // rename; if it appeared meanwhile FatFs refuses and the old file stays.
    if (!ok || rename(part, target) != 0) {
        remove(part);
        ESP_LOGW(TAG, "sidecar not saved: write failed");
        return;
    }
    ESP_LOGI(TAG, "saved to card (%s, %u B)", ext, (unsigned)len);
}

// ---- Network ---------------------------------------------------------------

typedef struct {
    char retry_after[16];
} http_ctx_t;

// Response headers are only visible here (esp_http_client_get_header reads
// the REQUEST headers).
static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    http_ctx_t *ctx = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && ctx && evt->header_key && evt->header_value &&
        strcasecmp(evt->header_key, "Retry-After") == 0) {
        strlcpy(ctx->retry_after, evt->header_value, sizeof(ctx->retry_after));
    }
    return ESP_OK;
}

// One GET. Returns the HTTP status, or -1 on a transport failure. body is a
// BODY_CAP + 1 buffer, NUL-terminated on return; *over is set when the answer
// did not fit.
static int http_get(const char *url, char *body, bool *over, http_ctx_t *ctx)
{
    char agent[96];
    snprintf(agent, sizeof(agent), "Bugne/%s (%s)", esp_app_get_description()->version, PROJECT_URL);
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .user_agent = agent,  // LRCLIB requires a client that names itself
        .event_handler = on_http_event,
        .user_data = ctx,
    };
    body[0] = '\0';
    *over = false;
    esp_http_client_handle_t client = http_util_client_init(&cfg);
    if (!client) return -1;
    int status = -1;
    // esp_http_client does not decompress: ask for the plain body.
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (status == 200) {
            int total = 0;
            while (total < BODY_CAP) {
                int r = esp_http_client_read(client, body + total, BODY_CAP - total);
                if (r < 0) { status = -1; break; }
                if (r == 0) break;
                total += r;
            }
            body[total] = '\0';
            // Full buffer: one more byte tells "exactly the cap" from "too large".
            char extra;
            if (total == BODY_CAP && esp_http_client_read(client, &extra, 1) > 0) *over = true;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

// Turn a 200 body into a result. Returns the state; *out is set for READY.
static lyrics_state_t use_body(const request_t *r, const char *body, lyrics_doc_t **out)
{
    // cJSON recurses once per nesting level, on this task's small stack: a
    // hostile body of brackets must not reach it.
    cJSON *root = lyrics_json_shape_ok(body, JSON_DEPTH_MAX) ? cJSON_Parse(body) : NULL;  // PSRAM (hooks in app_main)
    if (!root) {
        ESP_LOGW(TAG, "answer is not valid JSON");
        return LYRICS_ERROR;
    }
    lyrics_state_t state = LYRICS_NOT_FOUND;
    const cJSON *inst = cJSON_GetObjectItemCaseSensitive(root, "instrumental");
    const cJSON *synced = cJSON_GetObjectItemCaseSensitive(root, "syncedLyrics");
    const cJSON *plain = cJSON_GetObjectItemCaseSensitive(root, "plainLyrics");
    const cJSON *dur = cJSON_GetObjectItemCaseSensitive(root, "duration");
    bool has_synced = cJSON_IsString(synced) && synced->valuestring[0];
    bool has_plain = cJSON_IsString(plain) && plain->valuestring[0];
    uint32_t rec_ms = 0;
    if (cJSON_IsNumber(dur) && dur->valuedouble > 0 && dur->valuedouble < 100000) {
        rec_ms = (uint32_t)(dur->valuedouble * 1000.0);
    }
    bool trusted = has_synced && lyrics_use_synced(r->dur_ms, rec_ms);

    if (cJSON_IsTrue(inst) && !has_synced && !has_plain) {
        state = LYRICS_INSTRUMENTAL;
    } else if (has_synced || has_plain) {
        lyrics_doc_t *d;
        if (trusted) d = doc_from_lrc(r->key, synced->valuestring);
        else if (has_plain) d = doc_plain(r->key, plain->valuestring);
        else d = doc_lrc_as_plain(r->key, synced->valuestring);
        if (d) {
            *out = d;
            state = LYRICS_READY;
            if (d->synced) ESP_LOGI(TAG, "synced %u lines", (unsigned)d->line_count);
            else ESP_LOGI(TAG, "plain text %u B", (unsigned)strlen(d->text));
        } else {
            ESP_LOGW(TAG, "no memory for the result");
            state = LYRICS_ERROR;
        }
        switch (lyrics_save_form(has_synced, trusted, has_plain)) {
            case LYRICS_SAVE_LRC: save_sidecar(r, ".lrc", synced->valuestring); break;
            case LYRICS_SAVE_TXT: save_sidecar(r, ".txt", plain->valuestring); break;
            case LYRICS_SAVE_NONE: break;
        }
    }
    cJSON_Delete(root);
    return state;
}

// False once another song was asked or the request was cancelled: nothing
// more goes on the network for this one.
static bool still_wanted(const request_t *r)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool wanted = r->gen == s_gen;
    xSemaphoreGive(s_lock);
    return wanted;
}

static lyrics_state_t lookup_online(const request_t *r, lyrics_doc_t **out)
{
    if (esp_timer_get_time() < s_backoff_until_us) return LYRICS_ERROR;
    if (net_state() != NET_STATE_CONNECTED) return LYRICS_OFFLINE;

    char *body = psram_alloc(BODY_CAP + 1);
    char *url = psram_alloc(LYRICS_URL_MAX);
    lyrics_state_t state = LYRICS_ERROR;
    if (!body || !url) {
        ESP_LOGW(TAG, "no memory for the request");
        goto done;
    }
    uint32_t dur_s = (r->dur_ms + 500) / 1000;
    bool with_duration = dur_s >= 1 && dur_s <= 3600;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!lyrics_query_url(r->title, r->artist, with_duration ? dur_s : 0, url, LYRICS_URL_MAX)) {
            state = LYRICS_NOT_FOUND;  // empty or oversized title/artist: nothing to ask
            break;
        }
        if (!still_wanted(r)) break;  // the view was left or the song changed
        http_ctx_t ctx = { .retry_after = "" };
        bool over = false;
        ESP_LOGI(TAG, "request (%s duration)", with_duration ? "with" : "without");
        int status = http_get(url, body, &over, &ctx);
        lyrics_action_t act = lyrics_next_action(status, with_duration, over);
        if (act == LYRICS_ACT_RETRY_NO_DURATION) {
            with_duration = false;
            vTaskDelay(pdMS_TO_TICKS(RETRY_GAP_MS));
            continue;
        }
        if (act == LYRICS_ACT_USE) {
            state = use_body(r, body, out);
        } else if (act == LYRICS_ACT_NOT_FOUND) {
            ESP_LOGI(TAG, "not found");
            state = LYRICS_NOT_FOUND;
        } else if (act == LYRICS_ACT_BACKOFF) {
            uint32_t wait_s = lyrics_backoff_s(ctx.retry_after);
            s_backoff_until_us = esp_timer_get_time() + (int64_t)wait_s * 1000000;
            ESP_LOGW(TAG, "rate limited, no request for %u s", (unsigned)wait_s);
        } else {
            ESP_LOGW(TAG, "request failed (status %d%s)", status, over ? ", answer too large" : "");
        }
        break;
    }
done:
    free(url);
    free(body);
    return state;
}

static void lyrics_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_wake, portMAX_DELAY);
        for (;;) {
            request_t r;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool have = s_pending;
            if (have) {
                r = s_want;
                s_pending = false;
            }
            xSemaphoreGive(s_lock);
            if (!have) break;

            lyrics_doc_t *doc = load_sidecar(&r);
            lyrics_state_t state = LYRICS_READY;
            if (doc) {
                ESP_LOGI(TAG, "from card (%s)", doc->synced ? "synced" : "plain");
            } else {
                state = lookup_online(&r, &doc);
            }

            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (r.gen == s_gen) {
                lyrics_doc_free(s_doc);
                s_doc = doc;
                s_state = state;
                s_state_key = r.key;
                doc = NULL;
            }
            xSemaphoreGive(s_lock);
            lyrics_doc_free(doc);  // stale: another song was asked meanwhile
        }
    }
}

// ---- API (LVGL task) -------------------------------------------------------

void lyrics_init(void)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
    if (!s_lock || !s_wake) ESP_LOGW(TAG, "no lock: lyrics unavailable");
}

void lyrics_request(const char *title, const char *artist, uint32_t dur_ms, const char *sd_path)
{
    if (!s_lock || !s_wake) return;
    uint64_t key = lyrics_key(title, artist);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_task_started) {
        // Created on the first request: most devices never turn lyrics on.
        if (xTaskCreateWithCaps(lyrics_task, "lyrics", TASK_STACK, NULL, 3, NULL,
                                MALLOC_CAP_SPIRAM) == pdPASS) {
            s_task_started = true;
        } else {
            ESP_LOGW(TAG, "task not created: lyrics unavailable");
            s_state = LYRICS_ERROR;
            s_state_key = key;
            xSemaphoreGive(s_lock);
            return;
        }
    }
    memset(&s_want, 0, sizeof(s_want));
    s_want.key = key;
    s_want.gen = ++s_gen;
    s_want.dur_ms = dur_ms;
    strlcpy(s_want.title, title ? title : "", sizeof(s_want.title));
    strlcpy(s_want.artist, artist ? artist : "", sizeof(s_want.artist));
    strlcpy(s_want.path, sd_path ? sd_path : "", sizeof(s_want.path));
    s_pending = true;
    s_state = LYRICS_LOADING;
    s_state_key = key;
    lyrics_doc_free(s_doc);
    s_doc = NULL;
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_wake);
}

lyrics_state_t lyrics_state(uint64_t key)
{
    if (!s_lock) return LYRICS_ERROR;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    lyrics_state_t st = (s_state_key == key) ? s_state : LYRICS_IDLE;
    xSemaphoreGive(s_lock);
    return st;
}

lyrics_doc_t *lyrics_take(uint64_t key)
{
    if (!s_lock) return NULL;
    lyrics_doc_t *d = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_doc && s_doc->key == key) {
        d = s_doc;
        s_doc = NULL;
    }
    xSemaphoreGive(s_lock);
    return d;
}

void lyrics_cancel(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_gen++;
    s_pending = false;
    s_state = LYRICS_IDLE;
    lyrics_doc_free(s_doc);
    s_doc = NULL;
    xSemaphoreGive(s_lock);
}
