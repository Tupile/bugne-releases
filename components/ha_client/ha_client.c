// ha_client: drives a Home Assistant light for the on-device Lamp screen.
//
// Concurrency model: at most ONE ha_task runs at a time. A request that
// arrives while one is running is stored in a single pending slot (newest
// wins) and drained by the running task before it exits. This matters because
// the color arc and brightness slider fire an event per drag tick: the old
// spawn-per-request design created dozens of 8 KB tasks and sockets per
// second of dragging, against a budgeted LWIP socket pool.
#include "ha_client.h"
#include "config_store.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdbool.h>

static const char *TAG = "ha_client";

#define HA_URL_MAX     256
#define HA_AUTH_MAX    350
#define HA_PAYLOAD_MAX 128

typedef struct {
    char url[HA_URL_MAX];
    char auth_header[HA_AUTH_MAX];
    char payload[HA_PAYLOAD_MAX];
} ha_req_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_busy;                       // a ha_task exists and is working
static bool s_pend_valid;
static char s_pend_url[HA_URL_MAX];
static char s_pend_payload[HA_PAYLOAD_MAX];

static void ha_perform(const ha_req_t *req)
{
    esp_http_client_config_t config = {
        .url = req->url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 3000,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client) {
        esp_http_client_set_header(client, "Authorization", req->auth_header);
        esp_http_client_set_header(client, "Content-Type", "application/json");

        if (req->payload[0]) {
            esp_http_client_set_post_field(client, req->payload, strlen(req->payload));
        }

        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            int status_code = esp_http_client_get_status_code(client);
            ESP_LOGI(TAG, "HTTP POST Status = %d", status_code);
        } else {
            ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);
    } else {
        ESP_LOGE(TAG, "Failed to init HTTP client");
    }
}

static void ha_task(void *arg)
{
    ha_req_t *req = (ha_req_t *)arg;

    for (;;) {
        ha_perform(req);

        // Drain the pending slot (newest wins) or hand the busy flag back.
        // Both happen under one critical section so a request stored right
        // here is always seen by this loop, never lost.
        portENTER_CRITICAL(&s_lock);
        if (s_pend_valid) {
            snprintf(req->url, sizeof(req->url), "%s", s_pend_url);
            strlcpy(req->payload, s_pend_payload, sizeof(req->payload));
            s_pend_valid = false;
            portEXIT_CRITICAL(&s_lock);
            continue;
        }
        s_busy = false;
        portEXIT_CRITICAL(&s_lock);
        break;
    }

    free(req);
    vTaskDelete(NULL);
}

static void ha_client_send_request(const char *service, const char *payload)
{
    const config_t *cfg = config_store_get();
    if (!cfg || !cfg->ha.url[0] || !cfg->ha.entity_id[0]) {
        ESP_LOGW(TAG, "HA not configured");
        return;
    }

    // The long-lived token grants full API control of the home: sending it
    // over plain HTTP exposes it to anything sniffing the LAN. Warn once.
    static bool warned_cleartext;
    if (!warned_cleartext && strncmp(cfg->ha.url, "http://", 7) == 0) {
        warned_cleartext = true;
        ESP_LOGW(TAG, "ha.url is http:// : the HA token travels unencrypted");
    }

    char token[300] = {0};
    if (config_store_get_ha_token(token, sizeof(token)) != ESP_OK || !token[0]) {
        ESP_LOGW(TAG, "HA token not configured");
        return;
    }

    ha_req_t *req = heap_caps_malloc(sizeof(ha_req_t), MALLOC_CAP_SPIRAM);
    if (!req) {
        ESP_LOGE(TAG, "Failed to allocate HA request");
        return;
    }
    memset(req, 0, sizeof(ha_req_t));

    snprintf(req->url, sizeof(req->url), "%s/api/services/light/%s", cfg->ha.url, service);
    snprintf(req->auth_header, sizeof(req->auth_header), "Bearer %s", token);

    if (payload) {
        strlcpy(req->payload, payload, sizeof(req->payload));
    }

    portENTER_CRITICAL(&s_lock);
    if (s_busy) {
        // A request is in flight: coalesce, newest wins.
        strlcpy(s_pend_url, req->url, sizeof(s_pend_url));
        strlcpy(s_pend_payload, req->payload, sizeof(s_pend_payload));
        s_pend_valid = true;
        portEXIT_CRITICAL(&s_lock);
        free(req);
        return;
    }
    s_busy = true;
    portEXIT_CRITICAL(&s_lock);

    // Launch task with a PSRAM stack to avoid using internal RAM.
    BaseType_t res = xTaskCreateWithCaps(ha_task, "ha_task", 8192, req, 5, NULL, MALLOC_CAP_SPIRAM);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create HA task");
        portENTER_CRITICAL(&s_lock);
        s_busy = false;
        portEXIT_CRITICAL(&s_lock);
        free(req);
    }
}

void ha_client_toggle_light(void) {
    const config_t *cfg = config_store_get();
    if (!cfg || !cfg->ha.entity_id[0]) return;

    char payload[HA_PAYLOAD_MAX];
    snprintf(payload, sizeof(payload), "{\"entity_id\":\"%s\"}", cfg->ha.entity_id);
    ha_client_send_request("toggle", payload);
}

void ha_client_set_light_color(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
    const config_t *cfg = config_store_get();
    if (!cfg || !cfg->ha.entity_id[0]) return;

    char payload[HA_PAYLOAD_MAX];
    snprintf(payload, sizeof(payload),
             "{\"entity_id\":\"%s\",\"rgb_color\":[%d,%d,%d],\"brightness\":%d}",
             cfg->ha.entity_id, r, g, b, brightness);
    ha_client_send_request("turn_on", payload);
}
