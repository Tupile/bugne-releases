// ha_reply: see ha_reply.h.
//
// One task at a time, created per send, on a PSRAM stack: it never touches
// flash (NVS, LittleFS, OTA), which is the condition for a PSRAM stack.
#include "ha_reply.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "mdns.h"
#include "notif.h"

static const char *TAG = "ha_reply";

#define REPLY_ATTEMPTS   3
#define REPLY_TIMEOUT_MS 3000
#define REPLY_GAP_MS     1000
#define MDNS_TIMEOUT_MS  2000

typedef struct {
    char url[NOTIF_URL_MAX];
    char body[NOTIF_REPLY_MAX];
} reply_req_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static ha_reply_state_t s_state = HA_REPLY_IDLE;

// getaddrinfo does not resolve mDNS names in this build
// (CONFIG_LWIP_HOOK_NETCONN_EXT_RESOLVE_NONE), and Home Assistant's internal
// URL is often http://homeassistant.local:8123. Replace such a host by its
// address. https is left alone: the certificate would not match an address.
// False when the name is a .local one and was not resolved: posting to it
// anyway would only wait on a DNS lookup that cannot succeed.
static bool resolve_local(char *url, size_t size)
{
    char host[64];
    const char *rest;
    if (strncmp(url, "http://", 7) != 0) return true;
    if (!notif_url_split(url, host, sizeof(host), &rest)) return true;
    size_t len = strlen(host);
    if (len <= 6 || strcasecmp(host + len - 6, ".local") != 0) return true;
    host[len - 6] = '\0';

    esp_ip4_addr_t addr = {0};
    esp_err_t err = mdns_query_a(host, MDNS_TIMEOUT_MS, &addr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s.local not resolved: %s", host, esp_err_to_name(err));
        return false;
    }
    char out[NOTIF_URL_MAX];
    int n = snprintf(out, sizeof(out), "http://" IPSTR "%s", IP2STR(&addr), rest);
    if (n <= 0 || (size_t)n >= sizeof(out)) return false;
    strlcpy(url, out, size);
    return true;
}

static bool post_once(const reply_req_t *req)
{
    esp_http_client_config_t config = {
        .url = req->url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = REPLY_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "http client init failed");
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, req->body, (int)strlen(req->body));

    bool ok = false;
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ok = status >= 200 && status < 300;
        if (!ok) ESP_LOGW(TAG, "answer refused: HTTP %d", status);
    } else {
        ESP_LOGW(TAG, "answer not sent: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return ok;
}

static void reply_task(void *arg)
{
    reply_req_t *req = arg;
    bool ok = false;
    for (int i = 0; i < REPLY_ATTEMPTS && !ok; i++) {
        if (i > 0) vTaskDelay(pdMS_TO_TICKS(REPLY_GAP_MS));
        reply_req_t attempt = *req;   // resolve again on each attempt
        ok = resolve_local(attempt.url, sizeof(attempt.url)) && post_once(&attempt);
    }
    ESP_LOGI(TAG, "answer %s", ok ? "delivered" : "failed");
    heap_caps_free(req);

    portENTER_CRITICAL(&s_lock);
    s_state = ok ? HA_REPLY_OK : HA_REPLY_FAIL;
    portEXIT_CRITICAL(&s_lock);
    vTaskDeleteWithCaps(NULL);
}

bool ha_reply_send(const char *url, const char *json)
{
    if (!notif_url_ok(url) || !json) return false;
    if (strlen(url) >= NOTIF_URL_MAX || strlen(json) >= NOTIF_REPLY_MAX) return false;

    reply_req_t *req = heap_caps_calloc(1, sizeof(*req), MALLOC_CAP_SPIRAM);
    if (!req) {
        ESP_LOGE(TAG, "no memory for the answer");
        return false;
    }
    strlcpy(req->url, url, sizeof(req->url));
    strlcpy(req->body, json, sizeof(req->body));

    portENTER_CRITICAL(&s_lock);
    bool busy = s_state == HA_REPLY_BUSY;
    if (!busy) s_state = HA_REPLY_BUSY;
    portEXIT_CRITICAL(&s_lock);
    if (busy) {
        heap_caps_free(req);
        return false;
    }

    if (xTaskCreateWithCaps(reply_task, "ha_reply", 8192, req, 5, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "task not created");
        portENTER_CRITICAL(&s_lock);
        s_state = HA_REPLY_IDLE;
        portEXIT_CRITICAL(&s_lock);
        heap_caps_free(req);
        return false;
    }
    return true;
}

ha_reply_state_t ha_reply_poll(void)
{
    portENTER_CRITICAL(&s_lock);
    ha_reply_state_t st = s_state;
    if (st == HA_REPLY_OK || st == HA_REPLY_FAIL) s_state = HA_REPLY_IDLE;
    portEXIT_CRITICAL(&s_lock);
    return st;
}
