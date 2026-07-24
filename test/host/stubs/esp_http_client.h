// Host-test stub: a scriptable esp_http_client, just enough surface for
// memo_send.c. Single-translation-unit use only: the test #includes the .c
// under test so both sides share g_http below. Not the real header.
#pragma once
#include <stddef.h>
#include <string.h>
#include "esp_err.h"

typedef struct esp_http_client *esp_http_client_handle_t;

typedef struct {
    const char *url;
    int method;
    int timeout_ms;
    int buffer_size;
    int buffer_size_tx;
} esp_http_client_config_t;

#define HTTP_METHOD_POST 1

// Scripted behavior; the test resets and tweaks this between cases.
static struct {
    char url[256];          // last URL passed to init
    int  init_fail;         // init returns NULL
    esp_err_t open_err;     // returned by open
    long write_budget;      // bytes accepted before write fails; -1 = unlimited
    int  write_chunk_max;   // per-call acceptance clamp; 0 = none
    int  fetch_headers_ret; // < 0 = failure
    int  status;            // status code the peer answers
    long written;           // bytes accepted so far
    int  closed, cleaned;
} g_http;

static struct esp_http_client { int unused; } g_http_client;

static inline esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    if (g_http.init_fail) return NULL;
    strncpy(g_http.url, cfg->url, sizeof(g_http.url) - 1);
    g_http.url[sizeof(g_http.url) - 1] = '\0';
    return &g_http_client;
}

static inline esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len)
{
    (void)c; (void)write_len;
    return g_http.open_err;
}

static inline int esp_http_client_write(esp_http_client_handle_t c, const char *buf, int len)
{
    (void)c; (void)buf;
    if (g_http.write_budget >= 0) {
        long left = g_http.write_budget - g_http.written;
        if (left <= 0) return -1;
        if (len > left) len = (int)left;
    }
    if (g_http.write_chunk_max > 0 && len > g_http.write_chunk_max)
        len = g_http.write_chunk_max;
    g_http.written += len;
    return len;
}

static inline int esp_http_client_fetch_headers(esp_http_client_handle_t c)
{
    (void)c;
    return g_http.fetch_headers_ret;
}

static inline int esp_http_client_get_status_code(esp_http_client_handle_t c)
{
    (void)c;
    return g_http.status;
}

static inline esp_err_t esp_http_client_close(esp_http_client_handle_t c)
{
    (void)c;
    g_http.closed++;
    return ESP_OK;
}

static inline esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c)
{
    (void)c;
    g_http.cleaned++;
    return ESP_OK;
}
