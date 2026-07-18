// memo_send: stream a stored WAV to another Bugne over plain HTTP (the peer
// listens on its LAN web port, no TLS). First outbound POST in the codebase;
// client sizing follows the podcast download client.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "memo.h"

#define SEND_TIMEOUT_MS 15000
#define SEND_BUF_BYTES  4096

static const char *TAG = "memo";

esp_err_t memo_send(const char *ip, uint16_t port, const char *from,
                    const char *abs_path, bool talkie, int *http_status,
                    volatile int *pct)
{
    if (http_status) *http_status = 0;
    struct stat st;
    if (stat(abs_path, &st) != 0 || st.st_size <= MEMO_WAV_HEADER_BYTES)
        return ESP_ERR_INVALID_ARG;
    FILE *f = fopen(abs_path, "rb");
    if (!f) return ESP_FAIL;

    char url[160];
    snprintf(url, sizeof(url), "http://%s:%u/api/memo?from=%s%s", ip, (unsigned)port,
             from, talkie ? "&talkie=1" : "");
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = SEND_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    char *buf = heap_caps_malloc(SEND_BUF_BYTES, MALLOC_CAP_SPIRAM);
    bool ok = false;
    if (client && buf && esp_http_client_open(client, st.st_size) == ESP_OK) {
        size_t sent = 0;
        ok = true;
        for (;;) {
            size_t n = fread(buf, 1, SEND_BUF_BYTES, f);
            if (n == 0) break;
            size_t off = 0;
            while (off < n) {                 // esp_http_client_write may be partial
                int w = esp_http_client_write(client, buf + off, n - off);
                if (w <= 0) { ok = false; break; }
                off += (size_t)w;
            }
            if (!ok) break;
            sent += n;
            if (pct) *pct = (int)(sent * 100 / (size_t)st.st_size);
        }
        if (ok && sent != (size_t)st.st_size) ok = false;
        if (ok) {
            if (esp_http_client_fetch_headers(client) < 0) ok = false;
            int status = esp_http_client_get_status_code(client);
            if (http_status) *http_status = status;
            // 202 = talkie message accepted but stored as a normal memo
            // (receiver not in talkie mode); delivery still succeeded.
            if (status != 200 && status != 202) {
                ESP_LOGW(TAG, "peer answered %d", status);
                ok = false;
            }
        }
        esp_http_client_close(client);
    }
    if (buf) free(buf);
    if (client) esp_http_client_cleanup(client);
    fclose(f);
    return ok ? ESP_OK : ESP_FAIL;
}
