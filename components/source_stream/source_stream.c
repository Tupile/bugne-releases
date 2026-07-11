// source_stream: HTTP/HTTPS byte source feeding the shared decoder.
#include "source_stream.h"
#include "decode.h"
#include "audio_arbiter.h"

#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"  // xTaskCreatePinnedToCoreWithCaps (PSRAM stack)
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "rf_meta.h"

#include <time.h>

static const char *TAG = "source_stream";

#define HTTP_TIMEOUT_MS   10000
#define MAX_REDIRECTS     6
#define PLAYLIST_MAX      2048   // bound on playlist downloads
#define URL_MAX           512

// Network read jitter must not starve the I2S DMA. A reader task fills a large
// PSRAM ring buffer from HTTP; the decoder drains it. This decouples decode
// pacing from network hiccups. Sizes: ~16 s of 128 kbps audio buffered, decode
// starts once a ~1 s head start is in the buffer (tap-to-sound; the ring keeps
// filling toward its full size during playback, so the steady-state margin is
// unchanged). If start-of-stream stutters on a slow link, step back to 24 KB.
#define STREAM_BUF_BYTES  (256 * 1024)
#define PREBUFFER_BYTES   (16 * 1024)
#define READ_CHUNK        2048
#define RECV_TIMEOUT_MS   8000

typedef struct {
    esp_http_client_handle_t client;
    StreamBufferHandle_t sb;
    volatile bool reader_done;  // reader task has exited and no longer touches client
    int64_t pos;  // bytes consumed, for the decoder's tell()
    int metaint;  // ICY metadata interval in bytes (0 = no ICY metadata)
} stream_ctx_t;

static volatile bool s_stop;
static bool s_completed;  // last play reached the stream end (not stopped, no error)
static int s_metaint;  // icy-metaint captured by the HTTP header event handler
static char s_ctype[64];  // Content-Type captured by the header event handler
static volatile uint32_t s_rf_gen;  // bumped on every play start and stop: invalidates any running poller
static bool s_preroll_decoy;  // one-shot, consumed at the start of the next source_stream_play

// The streaming ring buffer (PSRAM, 256 KB) is created once and reused for every
// stream: it is reset at the start of each play and never freed. Creating and
// deleting this large block on every radio switch churned the PSRAM heap and
// triggered a tlsf double-free assert during teardown, so we keep it persistent.
static StreamBufferHandle_t s_sb;

// Capture the icy-metaint and Content-Type response headers (esp_http_client_get_header
// does not return them reliably for ICY/streaming responses). Runs synchronously on the
// calling task during fetch_headers.
static esp_err_t stream_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
            s_metaint = atoi(evt->header_value);
        } else if (strcasecmp(evt->header_key, "Content-Type") == 0) {
            strlcpy(s_ctype, evt->header_value, sizeof(s_ctype));
        }
    }
    return ESP_OK;
}

// Current ICY StreamTitle (Shoutcast/Icecast "now playing"), written by the
// reader task and read by the UI. Guarded by a short critical section.
static char s_icy_title[128];
static portMUX_TYPE s_icy_lock = portMUX_INITIALIZER_UNLOCKED;

// Byte truncation (strlcpy/snprintf into a small buffer) can cut a multi-byte
// UTF-8 sequence; the partial tail then breaks the /api JSON and renders as
// garbage. Trim an incomplete trailing sequence so titles are always valid.
static void utf8_trim_partial(char *s)
{
    size_t n = strlen(s);
    size_t k = 0;  // trailing continuation bytes (10xxxxxx)
    while (k < 3 && n > k && ((unsigned char)s[n - 1 - k] & 0xC0) == 0x80) k++;
    if (n <= k) return;
    unsigned char lead = (unsigned char)s[n - 1 - k];
    size_t need = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : (lead >= 0xC0) ? 2 : 1;
    if (lead >= 0xC0 && need > k + 1) s[n - 1 - k] = '\0';
}

static void icy_set_title(const char *t)
{
    taskENTER_CRITICAL(&s_icy_lock);
    strlcpy(s_icy_title, t, sizeof(s_icy_title));
    utf8_trim_partial(s_icy_title);
    taskEXIT_CRITICAL(&s_icy_lock);
}

size_t source_stream_title(char *buf, size_t size)
{
    if (!buf || size == 0) return 0;
    taskENTER_CRITICAL(&s_icy_lock);
    strlcpy(buf, s_icy_title, size);
    taskEXIT_CRITICAL(&s_icy_lock);
    utf8_trim_partial(buf);  // the caller's buffer may be smaller than the stored title
    return strlen(buf);
}

// Parse StreamTitle='...' out of an ICY metadata block and store it.
static void icy_parse(const char *meta)
{
    const char *p = strstr(meta, "StreamTitle='");
    if (!p) return;
    p += 13;
    const char *e = strstr(p, "';");
    if (!e) e = strchr(p, '\'');
    char title[128];
    size_t n = e ? (size_t)(e - p) : strlen(p);
    if (n >= sizeof(title)) n = sizeof(title) - 1;
    memcpy(title, p, n);
    title[n] = '\0';
    icy_set_title(title);
}

// Read exactly n bytes (looping over short reads). Returns bytes read (< n on EOF/error).
static int read_full(esp_http_client_handle_t c, char *buf, int n)
{
    int got = 0;
    while (got < n && !s_stop) {
        int r = esp_http_client_read(c, buf + got, n - got);
        if (r <= 0) break;
        got += r;
    }
    return got;
}

// Reader task: pull from HTTP into the ring buffer. Blocking on a full buffer is
// the desired backpressure (e.g. while paused). Exits on stop, EOF, or error.
static void reader_task(void *arg)
{
    stream_ctx_t *s = arg;
    uint8_t *chunk = malloc(READ_CHUNK);
    if (chunk) {
        int left = s->metaint;  // audio bytes until the next ICY metadata block
        while (!s_stop) {
            // At an ICY metadata boundary: read the length byte and the block,
            // parse the StreamTitle, and feed none of it to the decoder.
            if (s->metaint > 0 && left == 0) {
                uint8_t lb;
                if (read_full(s->client, (char *)&lb, 1) != 1) break;
                int mlen = (int)lb * 16;
                if (mlen > 0) {
                    char meta[256];
                    int got = 0, remaining = mlen;
                    while (remaining > 0 && !s_stop) {
                        char tmp[64];
                        int want = remaining < (int)sizeof(tmp) ? remaining : (int)sizeof(tmp);
                        int r = esp_http_client_read(s->client, tmp, want);
                        if (r <= 0) { remaining = -1; break; }
                        if (got < (int)sizeof(meta) - 1) {
                            int cp = (int)sizeof(meta) - 1 - got;
                            if (cp > r) cp = r;
                            memcpy(meta + got, tmp, cp);
                            got += cp;
                        }
                        remaining -= r;
                    }
                    if (remaining < 0) break;  // read error mid-metadata
                    meta[got] = '\0';
                    icy_parse(meta);
                }
                left = s->metaint;
                continue;
            }

            int want = READ_CHUNK;
            if (s->metaint > 0 && left < want) want = left;
            int r = esp_http_client_read(s->client, (char *)chunk, want);
            if (r <= 0) {
                break;  // EOF or error
            }
            if (s->metaint > 0) left -= r;
            size_t sent = 0;
            while (sent < (size_t)r && !s_stop) {
                sent += xStreamBufferSend(s->sb, chunk + sent, (size_t)r - sent, pdMS_TO_TICKS(500));
            }
        }
        free(chunk);
    }
    s->reader_done = true;
    // WithCaps task (PSRAM stack): the plain vTaskDelete(NULL) must not free it.
    vTaskDeleteWithCaps(NULL);
}

// Open the client and follow redirects, leaving the body ready to read.
// Returns the final HTTP status code, or -1 on failure.
static int open_following_redirects(esp_http_client_handle_t client)
{
    for (int i = 0; i < MAX_REDIRECTS; i++) {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            return -1;
        }
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            continue;
        }
        return status;
    }
    ESP_LOGW(TAG, "too many redirects");
    return -1;
}

static bool url_has_ext(const char *url, const char *ext)
{
    size_t lu = strlen(url), le = strlen(ext);
    return lu >= le && strcasecmp(url + lu - le, ext) == 0;
}

static bool is_playlist(const char *url)
{
    return url_has_ext(url, ".m3u") || url_has_ext(url, ".m3u8") || url_has_ext(url, ".pls");
}

// Extract the first http(s) URL from playlist text (works for both .m3u bare
// URLs and .pls "FileN=URL" lines).
static bool playlist_extract(const char *text, char *out, size_t out_size)
{
    const char *p = strstr(text, "http://");
    const char *q = strstr(text, "https://");
    const char *u = (!p) ? q : (!q ? p : (p < q ? p : q));
    if (!u) {
        return false;
    }
    size_t n = 0;
    while (u[n] && u[n] != '\r' && u[n] != '\n' && u[n] != ' ' && u[n] != '\t' && n < out_size - 1) {
        out[n] = u[n];
        n++;
    }
    out[n] = '\0';
    return n > 0;
}

// Fetch a small text resource (playlist) into buf.
static bool fetch_text(const char *url, char *buf, size_t size)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return false;
    }
    bool ok = false;
    int status = open_following_redirects(client);
    if (status == 200) {
        int total = 0;
        while (total < (int)size - 1) {
            int r = esp_http_client_read(client, buf + total, (int)size - 1 - total);
            if (r <= 0) break;
            total += r;
        }
        buf[total] = '\0';
        ok = total > 0;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

static decode_format_t detect_format(esp_http_client_handle_t client, const char *url)
{
    // Prefer the Content-Type captured by the header event handler (reliable for
    // ICY/streaming responses); fall back to get_header for the plain-HTTP case.
    const char *ctype = s_ctype[0] ? s_ctype : NULL;
    char *hdr = NULL;
    if (!ctype && esp_http_client_get_header(client, "Content-Type", &hdr) == ESP_OK && hdr) {
        ctype = hdr;
    }
    if (ctype) {
        if (strstr(ctype, "flac") || strstr(ctype, "FLAC")) {
            return DECODE_FORMAT_FLAC;
        }
        // AAC: raw ADTS (audio/aac) or in an MP4 container (audio/mp4, audio/x-m4a).
        if (strstr(ctype, "aac") || strstr(ctype, "mp4") || strstr(ctype, "m4a")) {
            return DECODE_FORMAT_AAC;
        }
        // Ogg container (Opus or Vorbis): audio/ogg, audio/opus, application/ogg.
        if (strstr(ctype, "ogg") || strstr(ctype, "opus") || strstr(ctype, "vorbis")) {
            return DECODE_FORMAT_OGG;
        }
    }
    if (url_has_ext(url, ".flac")) {
        return DECODE_FORMAT_FLAC;
    }
    if (url_has_ext(url, ".aac") || url_has_ext(url, ".m4a") || url_has_ext(url, ".mp4")) {
        return DECODE_FORMAT_AAC;  // run_aac sniffs ADTS vs MP4 (streamed MP4 is Phase 2)
    }
    if (url_has_ext(url, ".ogg") || url_has_ext(url, ".opus") || url_has_ext(url, ".oga")) {
        return DECODE_FORMAT_OGG;
    }
    return DECODE_FORMAT_MP3;  // web radio and podcasts are MP3 by default
}

static size_t stream_read(void *ctx, void *buf, size_t bytes)
{
    stream_ctx_t *s = ctx;
    if (s_stop) {
        return 0;  // signal EOF so decode_run returns
    }
    // Reader gone (EOF or dead connection): only drain what is left in the
    // ring, never block. dr_mp3 re-polls the source once per frame it still
    // holds in its input cache, and a full RECV_TIMEOUT_MS block per poll
    // turns a dead radio into ~5 min of one-frame-per-8-s zombie playback
    // before decode_run returns (seen on the bench, 2026-07-10).
    TickType_t wait = s->reader_done ? 0 : pdMS_TO_TICKS(RECV_TIMEOUT_MS);
    size_t got = xStreamBufferReceive(s->sb, buf, bytes, wait);
    if (got == 0) {
        // Empty: real end of stream, or the network stalled past the timeout.
        if (!s->reader_done && !s_stop) {
            ESP_LOGW(TAG, "buffer underrun (no data for %d ms)", RECV_TIMEOUT_MS);
        }
        return 0;
    }
    s->pos += got;
    return got;
}

esp_err_t source_stream_init(void)
{
    return ESP_OK;
}

void source_stream_stop(void)
{
    s_stop = true;
    s_rf_gen++;  // invalidate any running radiofrance meta poller
}

bool source_stream_completed(void)
{
    return s_completed;
}

void source_stream_set_preroll_decoy(bool enable)
{
    s_preroll_decoy = enable;
}

// ---- Seekable HTTP byte source (for streaming a .m4a / MP4 container) ----
// A streamed .m4a keeps its index (moov) at the end of the file, so the demuxer
// needs random access. We give it a byte source whose seek reopens the HTTP
// connection with a Range request. minimp4 reads the moov sequentially and the
// AAC frames are contiguous, so in practice this is only a few reopens (ftyp,
// moov, then one sequential pass over mdat). Requires a Range-capable server
// (Radio France answers 206); a server that ignores Range (200 for off>0) fails.
// The whole moov (the index, at the end of these files) is fetched once into
// PSRAM. Demuxer reads that land in the moov are served from RAM (no HTTP), so
// parsing does no TLS round-trips. Reads in the mdat region (the AAC frames) go
// over one streaming HTTP connection, reopened only on a non-contiguous seek.
#define HSEEK_MOOV_MAX (6 * 1024 * 1024)  // refuse to buffer an absurdly large moov
#define HSEEK_SKIP_MAX (2 * 1024 * 1024)  // drain (not reopen) forward gaps up to this
typedef struct {
    char    url[URL_MAX];
    int64_t total;
    int64_t pos;        // logical position (next byte to return)
    esp_http_client_handle_t cl;
    int64_t conn_pos;   // byte offset the HTTP connection is positioned at
    bool    open;
    uint8_t *moov;      // cached moov bytes, file range [moov_off, moov_off+moov_sz)
    int64_t  moov_off;
    int64_t  moov_sz;
} hseek_t;

// Open the HTTP connection (following redirects) with a Range starting at off.
static bool hseek_http_open(hseek_t *h, int64_t off)
{
    if (h->cl) {
        esp_http_client_close(h->cl);
        esp_http_client_cleanup(h->cl);
        h->cl = NULL;
        h->open = false;
    }
    esp_http_client_config_t cfg = {
        .url = h->url, .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS, .buffer_size = 2048, .buffer_size_tx = 2048,
    };
    h->cl = esp_http_client_init(&cfg);
    if (!h->cl) return false;
    char range[40];
    snprintf(range, sizeof(range), "bytes=%lld-", (long long)off);
    esp_http_client_set_header(h->cl, "Range", range);
    int st = open_following_redirects(h->cl);
    if (st != 200 && st != 206) return false;
    if (st == 200 && off > 0) {
        ESP_LOGE(TAG, "server ignored Range, cannot stream this .m4a (download it)");
        return false;
    }
    h->conn_pos = off;
    h->open = true;
    return true;
}

static size_t hseek_read(void *ctx, void *buf, size_t bytes)
{
    hseek_t *h = ctx;
    if (s_stop || bytes == 0 || h->pos >= h->total) return 0;
    // Served from the cached moov window? (mdat outside it streams over HTTP, so a
    // faststart file with moov near the front does not pull the whole file into RAM.)
    if (h->moov && h->pos >= h->moov_off && h->pos < h->moov_off + h->moov_sz) {
        size_t avail = (size_t)(h->moov_off + h->moov_sz - h->pos);
        size_t take = bytes < avail ? bytes : avail;
        memcpy(buf, h->moov + (h->pos - h->moov_off), take);
        h->pos += take;
        return take;
    }
    // A small forward gap on the open connection (the mdat box header, or an intro
    // skip): drain it instead of reopening. Reopening would re-resolve the proxycast
    // redirect to a fresh signed CDN URL, and rapid reconnects get reset by the peer.
    if (h->open && h->pos > h->conn_pos && h->pos - h->conn_pos <= HSEEK_SKIP_MAX) {
        uint8_t skipbuf[1024];
        while (h->conn_pos < h->pos && !s_stop) {
            int64_t need = h->pos - h->conn_pos;
            int want = need < (int64_t)sizeof(skipbuf) ? (int)need : (int)sizeof(skipbuf);
            int r = esp_http_client_read(h->cl, (char *)skipbuf, want);
            if (r <= 0) { h->open = false; break; }
            h->conn_pos += r;
        }
    }
    // Otherwise from the HTTP connection (reopen if it is not at pos).
    if (!h->open || h->conn_pos != h->pos) {
        if (!hseek_http_open(h, h->pos)) return 0;
    }
    int r = esp_http_client_read(h->cl, buf, bytes);
    if (r <= 0) return 0;
    h->pos += r;
    h->conn_pos += r;
    return (size_t)r;
}

static bool hseek_seek(void *ctx, int offset, int origin)
{
    hseek_t *h = ctx;
    int64_t tgt = origin == 1 ? h->pos + offset
                : origin == 2 ? h->total + offset
                : offset;
    if (tgt < 0 || tgt > h->total) return false;
    h->pos = tgt;  // lazy: the next read opens/serves as needed
    return true;
}

static bool hseek_tell(void *ctx, int64_t *cursor)
{
    hseek_t *h = ctx;
    *cursor = h->pos;
    return true;
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3];
}

// Find the moov box offset by walking the top-level boxes (ftyp/free/mdat/moov).
// `front` holds the first `n` bytes of the file. On success returns the offset and
// stores the moov box length in *moov_sz; returns -1 if not determinable. A moov
// matched inside `front` (faststart, moov before mdat) yields its exact box size; a
// moov only reached by stepping past mdat (moov-at-end) is assumed to run to EOF.
static int64_t find_moov_off(const uint8_t *front, size_t n, int64_t total, int64_t *moov_sz)
{
    int64_t off = 0;
    while (off + 8 <= (int64_t)n) {
        uint32_t sz = be32(front + (size_t)off);
        const uint8_t *t = front + (size_t)off + 4;
        int64_t bsize = sz;
        if (sz == 1) {  // 64-bit size follows the type
            if (off + 16 > (int64_t)n) return -1;
            const uint8_t *e = front + (size_t)off + 8;
            bsize = ((int64_t)be32(e) << 32) | be32(e + 4);
        } else if (sz == 0) {
            bsize = total - off;  // extends to EOF
        }
        if (memcmp(t, "moov", 4) == 0) { *moov_sz = bsize; return off; }
        if (bsize < 8) return -1;
        off += bsize;  // ftyp/free/mdat sit before moov; this reaches moov's start
    }
    *moov_sz = total - off;  // moov-at-end: assume it spans to EOF
    return (off < total) ? off : -1;  // the box after the last header we could read
}

// True when an AAC stream is an MP4 container (.m4a) rather than raw ADTS.
static bool aac_is_mp4(esp_http_client_handle_t client, const char *url)
{
    char *ct = NULL;
    if (esp_http_client_get_header(client, "Content-Type", &ct) == ESP_OK && ct) {
        if (strstr(ct, "mp4") || strstr(ct, "m4a")) return true;
        if (strstr(ct, "aac")) return false;  // raw ADTS
    }
    return url_has_ext(url, ".m4a") || url_has_ext(url, ".mp4");
}

// Decode a streamed .m4a using the seekable HTTP source (the demuxer reuses the
// same run_aac MP4 path as the SD card).
static esp_err_t stream_decode_mp4_seek(const char *url, int64_t total)
{
    hseek_t h = {0};
    strlcpy(h.url, url, sizeof(h.url));
    h.total = total;

    // Read the first bytes to locate the moov box.
    if (!hseek_http_open(&h, 0)) {
        if (h.cl) { esp_http_client_close(h.cl); esp_http_client_cleanup(h.cl); }
        return ESP_FAIL;
    }
    uint8_t front[64];
    size_t fn = 0;
    while (fn < sizeof(front)) {
        int r = esp_http_client_read(h.cl, (char *)front + fn, sizeof(front) - fn);
        if (r <= 0) break;
        fn += r;
    }
    h.conn_pos = fn;
    int64_t moov_sz = -1;
    int64_t moov_off = find_moov_off(front, fn, total, &moov_sz);
    esp_err_t err = ESP_FAIL;
    if (moov_off < 0 || moov_sz <= 0) {
        ESP_LOGE(TAG, "streamed .m4a: moov not locatable (off=%lld sz=%lld); download it",
                 (long long)moov_off, (long long)moov_sz);
        goto cleanup;
    }
    bool faststart = (moov_off + moov_sz < total);  // moov sits before mdat
    if (faststart) {
        // Cache the whole header [0, moov_end) in PSRAM (ftyp + moov) and keep this
        // one connection open at moov_end, the mdat start. The demuxer then parses
        // the header from RAM and streams the mdat forward over the same connection
        // with no reopen (reopening would re-resolve the redirect and the CDN resets
        // on rapid reconnects). The remaining sequential reads share that connection.
        int64_t head_len = moov_off + moov_sz;
        if (head_len > HSEEK_MOOV_MAX) {
            ESP_LOGE(TAG, "streamed .m4a: header too large (%lld); download it", (long long)head_len);
            goto cleanup;
        }
        h.moov = heap_caps_malloc((size_t)head_len, MALLOC_CAP_SPIRAM);
        if (!h.moov) { err = ESP_ERR_NO_MEM; goto cleanup; }
        h.moov_off = 0;
        h.moov_sz = head_len;
        size_t got = fn < (size_t)head_len ? fn : (size_t)head_len;
        memcpy(h.moov, front, got);  // reuse the bytes already read for sniffing
        while (got < (size_t)head_len && !s_stop) {
            int r = esp_http_client_read(h.cl, (char *)h.moov + got, (size_t)head_len - got);
            if (r <= 0) break;
            got += r;
        }
        if (got < (size_t)head_len) { ESP_LOGE(TAG, "header fetch short (%u/%lld)", (unsigned)got, (long long)head_len); goto cleanup; }
        h.conn_pos = head_len;  // connection now sits at mdat start; leave it open
        ESP_LOGI(TAG, "streaming .m4a (faststart): header %lld B cached, decoding %lld B", (long long)head_len, (long long)total);
    } else {
        // moov at end: cache the moov in PSRAM; the mdat (before moov) streams
        // over HTTP. find_moov_off could only assume "to EOF" for the size, so
        // read the 8-byte box header first: when it really is the moov (the
        // usual layout) cache exactly its size instead of everything to EOF (a
        // trailing free/udta box would otherwise inflate the PSRAM window).
        if (!hseek_http_open(&h, moov_off)) goto cleanup;
        uint8_t hdr[8];
        size_t hn = 0;
        while (hn < sizeof(hdr) && !s_stop) {
            int r = esp_http_client_read(h.cl, (char *)hdr + hn, sizeof(hdr) - hn);
            if (r <= 0) break;
            hn += r;
        }
        if (hn < sizeof(hdr)) { ESP_LOGE(TAG, "moov header fetch short"); goto cleanup; }
        if (memcmp(hdr + 4, "moov", 4) == 0) {
            int64_t exact = be32(hdr);
            if (exact >= 8 && moov_off + exact <= total) moov_sz = exact;
        }
        if (moov_sz > HSEEK_MOOV_MAX) {
            ESP_LOGE(TAG, "streamed .m4a: moov too large (%lld); download it", (long long)moov_sz);
            goto cleanup;
        }
        h.moov = heap_caps_malloc((size_t)moov_sz, MALLOC_CAP_SPIRAM);
        if (!h.moov) { err = ESP_ERR_NO_MEM; goto cleanup; }
        h.moov_off = moov_off;
        h.moov_sz = moov_sz;
        memcpy(h.moov, hdr, sizeof(hdr));
        size_t got = sizeof(hdr);
        while (got < (size_t)moov_sz && !s_stop) {
            int r = esp_http_client_read(h.cl, (char *)h.moov + got, (size_t)moov_sz - got);
            if (r <= 0) break;
            got += r;
        }
        if (got < (size_t)moov_sz) { ESP_LOGE(TAG, "moov fetch short (%u/%lld)", (unsigned)got, (long long)moov_sz); goto cleanup; }
        ESP_LOGI(TAG, "streaming .m4a: moov %lld B cached, decoding %lld B", (long long)moov_sz, (long long)total);
    }
    h.pos = 0;
    {
        decode_source_t src = {
            .read = hseek_read, .seek = hseek_seek, .tell = hseek_tell,
            .ctx = &h, .total_bytes = total,
        };
        err = decode_run(DECODE_FORMAT_AAC, &src);
    }
cleanup:
    if (h.cl) { esp_http_client_close(h.cl); esp_http_client_cleanup(h.cl); }
    free(h.moov);
    return err;
}

// ---- Radio France out-of-band "now playing" poller ----
// Radio France icecast streams never send ICY metadata, so the in-band StreamTitle
// path above shows nothing for them. When the playing stream is a known Radio France
// webradio with no ICY metadata, this task polls the station's livemeta JSON API and
// pushes the parsed title through icy_set_title, so the UI and /api/playback pick it
// up with zero UI changes. Lifecycle is a generation counter (s_rf_gen), no join: the
// task exits on its own as soon as the generation it was created with goes stale.
#define RF_META_BUF (8 * 1024)  // live endpoint answers about 1 KB

typedef struct {
    uint32_t gen;
    int station_id;
} rf_poll_arg_t;

static void rf_poll_task(void *arg)
{
    rf_poll_arg_t a = *(rf_poll_arg_t *)arg;
    free(arg);
    uint32_t my_gen = a.gen;

    uint8_t *buf = heap_caps_malloc(RF_META_BUF, MALLOC_CAP_SPIRAM);
    if (!buf) {
        vTaskDeleteWithCaps(NULL);
        return;
    }
    char last_title[128] = "";

    while (s_rf_gen == my_gen) {
        char url[96];
        snprintf(url, sizeof(url), "https://api.radiofrance.fr/livemeta/live/%d/webrf_webradio_player",
                 a.station_id);

        esp_http_client_config_t cfg = {
            .url = url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 8000,
            .buffer_size = 2048,
            .buffer_size_tx = 2048,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        bool ok = false;
        if (client) {
            // This endpoint gzips its response unless identity is asked: required, not optional.
            esp_http_client_set_header(client, "Accept-Encoding", "identity");
            if (esp_http_client_open(client, 0) == ESP_OK) {
                esp_http_client_fetch_headers(client);
                if (esp_http_client_get_status_code(client) == 200) {
                    int total = 0;
                    while (total < RF_META_BUF - 1) {
                        int r = esp_http_client_read(client, (char *)buf + total, RF_META_BUF - 1 - total);
                        if (r <= 0) break;
                        total += r;
                    }
                    buf[total] = '\0';
                    ok = total > 0;
                }
            }
            // No keep-alive: polls are 15 to 120 s apart, the server would drop an
            // idle connection anyway, so close and clean up every iteration.
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
        }

        char title[128];
        int64_t end_epoch = 0;
        bool parsed = ok && rf_meta_parse((const char *)buf, title, sizeof(title), &end_epoch);
        if (parsed && s_rf_gen == my_gen) {
            if (strcmp(title, last_title) != 0) {
                ESP_LOGI(TAG, "radiofrance now playing: %s", title);
                strlcpy(last_title, title, sizeof(last_title));
            }
            icy_set_title(title);
        }

        int sleep_s;
        if (!parsed) {
            sleep_s = 60;  // fetch or parse failure: keep the previous title, back off
        } else {
            time_t now = time(NULL);
            if (end_epoch > now && now > 1600000000) {
                sleep_s = (int)(end_epoch - now) + 2;
            } else {
                sleep_s = 30;
            }
        }
        // 15 s is a politeness floor: the dev IP got temporarily banned by the API's
        // Akamai during recon for polling faster than this.
        if (sleep_s < 15) sleep_s = 15;
        if (sleep_s > 120) sleep_s = 120;

        int waited = 0;
        while (waited < sleep_s * 1000 && s_rf_gen == my_gen) {
            vTaskDelay(pdMS_TO_TICKS(500));
            waited += 500;
        }
    }

    free(buf);
    // WithCaps task (PSRAM stack): the plain vTaskDelete(NULL) must not free it.
    vTaskDeleteWithCaps(NULL);
}

// ---- Pre-roll ad decoy ----
// Some icecast stations (OUI FM on Infomaniak) inject a ~30 s ad into the first
// of several near-simultaneous connections from one IP; the others join the
// live stream instantly. Opening a throwaway connection first and keeping it
// open while the real connection is established makes the decoy the "first"
// connection, so the real one skips the ad. Every failure here falls open
// (normal single-connection play): the decoy must never block playback.

// Open a connection to url and drain up to 16 KB from it. Returns the still-open
// handle (caller closes it once the real connection is up), or NULL if the decoy
// could not be opened.
static esp_http_client_handle_t open_preroll_decoy(const char *url)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 5000,
        // No event_handler: this connection must not touch s_metaint/s_ctype.
        // Default buffer sizes: this connection only reads and discards.
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGW(TAG, "preroll decoy: init failed, skipping");
        return NULL;
    }
    int status = open_following_redirects(client);
    if (status != 200 && status != 206) {
        ESP_LOGW(TAG, "preroll decoy: open failed, status %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }
    ESP_LOGI(TAG, "preroll decoy: connected, absorbing");
    // Bounded by both the byte cap and the 5 s per-read timeout above (8 reads
    // of 2 KB max: at most a handful of seconds even if every read times out).
    uint8_t *buf = malloc(READ_CHUNK);
    if (buf) {
        int total = 0;
        while (total < 16 * 1024) {
            int r = esp_http_client_read(client, (char *)buf, READ_CHUNK);
            if (r <= 0) break;
            total += r;
        }
        free(buf);
    }
    return client;
}

// Close a decoy opened by open_preroll_decoy. No-op if NULL (decoy skipped or
// already closed).
static void close_preroll_decoy(esp_http_client_handle_t decoy)
{
    if (!decoy) {
        return;
    }
    esp_http_client_close(decoy);
    esp_http_client_cleanup(decoy);
    ESP_LOGI(TAG, "preroll decoy: closed");
}

esp_err_t source_stream_play(const char *url)
{
    s_rf_gen++;  // invalidate any poller left over from a previous stream
    icy_set_title("");  // clear any previous station's "now playing"
    char resolved[URL_MAX];
    strlcpy(resolved, url, sizeof(resolved));

    if (is_playlist(url)) {
        char text[PLAYLIST_MAX];
        if (!fetch_text(url, text, sizeof(text)) || !playlist_extract(text, resolved, sizeof(resolved))) {
            ESP_LOGE(TAG, "could not resolve playlist %s", url);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "playlist resolved to %s", resolved);
    }

    // One-shot: clear immediately so a decoy failure below cannot leave it stuck on.
    esp_http_client_handle_t decoy = NULL;
    if (s_preroll_decoy) {
        s_preroll_decoy = false;
        decoy = open_preroll_decoy(resolved);
    }

    s_metaint = 0;  // reset before this connection; the event handler fills it in
    s_ctype[0] = '\0';  // ditto for Content-Type (used by detect_format)
    esp_http_client_config_t cfg = {
        .url = resolved,
        .event_handler = stream_http_event,  // captures icy-metaint from the headers
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        // Long episode URLs/paths (e.g. Radio France) overflow the default 512 B
        // request/response buffers, so enlarge them.
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        close_preroll_decoy(decoy);
        return ESP_ERR_NO_MEM;
    }
    // Ask for ICY metadata (Shoutcast/Icecast now-playing). Stations that support
    // it answer with an icy-metaint header and interleave metadata in the body.
    esp_http_client_set_header(client, "Icy-MetaData", "1");

    esp_err_t err = ESP_FAIL;
    int status = open_following_redirects(client);
    // The decoy's job (absorbing the ad slot ahead of this connection) is done as
    // soon as this real connection is up, success or failure: close it now.
    close_preroll_decoy(decoy);
    ESP_LOGI(TAG, "http open %s -> status %d, content_length=%lld, chunked=%d", resolved, status,
             (long long)esp_http_client_get_content_length(client),
             (int)esp_http_client_is_chunked_response(client));
    if (status != 200 && status != 206) {
        ESP_LOGE(TAG, "stream open failed, status %d", status);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // A streamed .m4a (AAC in MP4) cannot be decoded forward-only: its index sits
    // at the end of the file. Switch to the seekable (Range) source, which the
    // demuxer drives directly. Needs a known length and a Range-capable server.
    if (detect_format(client, resolved) == DECODE_FORMAT_AAC && aac_is_mp4(client, resolved)) {
        int64_t total = esp_http_client_get_content_length(client);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (total <= 0) {
            ESP_LOGE(TAG, "streamed .m4a needs a Content-Length");
            return ESP_FAIL;
        }
        err = audio_arbiter_acquire(AUDIO_SOURCE_STREAM);
        if (err != ESP_OK) return err;
        s_stop = false;
        err = stream_decode_mp4_seek(resolved, total);
        s_completed = (err == ESP_OK) && !s_stop;
        s_stop = true;
        audio_arbiter_release(AUDIO_SOURCE_STREAM);
        return err;
    }

    err = audio_arbiter_acquire(AUDIO_SOURCE_STREAM);
    if (err != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    s_stop = false;
    // Create the persistent ring buffer once; on later plays just reset it. No
    // task is blocked on it here (the previous play already joined its reader),
    // so the reset is safe.
    if (!s_sb) {
        s_sb = xStreamBufferCreateWithCaps(STREAM_BUF_BYTES, 1, MALLOC_CAP_SPIRAM);
        if (!s_sb) {
            ESP_LOGE(TAG, "stream buffer alloc failed");
            audio_arbiter_release(AUDIO_SOURCE_STREAM);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_NO_MEM;
        }
    } else {
        xStreamBufferReset(s_sb);
    }
    stream_ctx_t sctx = { .client = client, .pos = 0, .metaint = s_metaint, .sb = s_sb };
    if (sctx.metaint > 0) {
        ESP_LOGI(TAG, "ICY metadata every %d bytes", sctx.metaint);
    }
    // Pin to core 1: keep the network/TLS read off core 0 where WiFi and the
    // LVGL UI run, so scrolling a large list cannot delay the audio pipeline.
    // PSRAM stack (WithCaps): this task never calls flash APIs, and 6 KB of
    // internal RAM matters during HTTPS streaming.
    if (xTaskCreatePinnedToCoreWithCaps(reader_task, "stream_rd", 6144, &sctx, 5, NULL, 1,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "reader task create failed");
        audio_arbiter_release(AUDIO_SOURCE_STREAM);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // No ICY metadata: if this is a known Radio France webradio, poll its
    // out-of-band livemeta API instead (ICY wins when present).
    if (sctx.metaint == 0) {
        // Match on the final (post-redirect) URL: legacy hosts like
        // direct.fipradio.fr 301 to icecast.radiofrance.fr, so this also covers
        // old bookmarked webradio URLs.
        char final_url[URL_MAX];
        if (esp_http_client_get_url(client, final_url, sizeof(final_url)) != ESP_OK) {
            strlcpy(final_url, resolved, sizeof(final_url));
        }
        int rf_id = rf_station_id(final_url);
        if (rf_id > 0) {
            rf_poll_arg_t *rf_arg = malloc(sizeof(*rf_arg));
            if (rf_arg) {
                rf_arg->gen = s_rf_gen;
                rf_arg->station_id = rf_id;
                // PSRAM stack: this task never touches NVS/LittleFS/flash APIs
                // (hard project rule: flash writes need an internal-RAM stack).
                // 8 KB: unlike reader_task this task performs its own TLS
                // handshakes, and the mbedTLS ECC math runs on the task stack.
                if (xTaskCreatePinnedToCoreWithCaps(rf_poll_task, "rf_meta", 8192, rf_arg, 3,
                                                     NULL, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
                    ESP_LOGW(TAG, "radiofrance meta poller create failed");
                    free(rf_arg);
                }
            }
        }
    }

    // Wait for a head start so a slow first few seconds does not stutter.
    while (!s_stop && !sctx.reader_done &&
           xStreamBufferBytesAvailable(sctx.sb) < PREBUFFER_BYTES) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // content_length is the episode size for podcasts (lets the decoder estimate
    // duration); it is negative for chunked live radio, where there is no length.
    int64_t clen = esp_http_client_get_content_length(client);
    decode_source_t src = {
        .read = stream_read,
        .seek = NULL,  // live stream: not seekable, decoders use forward-only path
        .tell = NULL,
        .ctx  = &sctx,
        .total_bytes = clen > 0 ? clen : 0,
    };
    ESP_LOGI(TAG, "streaming %s", resolved);
    err = decode_run(detect_format(client, resolved), &src);
    // A connection dropped mid-episode looks like a clean EOF to the decoder.
    // Compare bytes delivered against the announced length before calling it a
    // completion, or the UI would silently auto-advance and drop the resume
    // point. The 64 KB slack covers trailing tags a decoder may leave unread;
    // a real drop loses megabytes.
    bool truncated = (clen > 0) && (sctx.pos + 64 * 1024 < clen);
    if (truncated) {
        ESP_LOGW(TAG, "stream ended short: got %lld of %lld bytes",
                 (long long)sctx.pos, (long long)clen);
    }
    // Capture natural end before the cleanup below sets s_stop: a clean decode
    // that the user did not interrupt means the episode played to its end.
    s_completed = (err == ESP_OK) && !s_stop && !truncated;
    if (err == ESP_OK && truncated && !s_stop) {
        err = ESP_FAIL;  // surface the failure on the now-playing screen
    }

    // Stop the reader and wait until it has fully released the client before we
    // close it (closing under an in-flight read would crash).
    s_stop = true;
    while (!sctx.reader_done) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    // s_sb is persistent: not deleted here, reset on the next play.
    audio_arbiter_release(AUDIO_SOURCE_STREAM);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}
