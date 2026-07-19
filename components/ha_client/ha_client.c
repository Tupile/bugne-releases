#include "ha_client.h"
#include "config_store.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "ha_client";

typedef struct {
    char url[256];
    char auth_header[350];
    char payload[128];
} ha_req_t;

static void ha_task(void *arg) {
    ha_req_t *req = (ha_req_t *)arg;

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

    free(req);
    vTaskDelete(NULL);
}

static void ha_client_send_request(const char *service, const char *payload) {
    const config_t *cfg = config_store_get();
    if (!cfg || !cfg->ha.url[0] || !cfg->ha.entity_id[0]) {
        ESP_LOGW(TAG, "HA not configured");
        return;
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

    // Launch task with a PSRAM stack to avoid using internal RAM.
    BaseType_t res = xTaskCreateWithCaps(ha_task, "ha_task", 8192, req, 5, NULL, MALLOC_CAP_SPIRAM);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create HA task");
        free(req);
    }
}

void ha_client_toggle_light(void) {
    const config_t *cfg = config_store_get();
    if (!cfg || !cfg->ha.entity_id[0]) return;

    char payload[128];
    snprintf(payload, sizeof(payload), "{\"entity_id\":\"%s\"}", cfg->ha.entity_id);
    ha_client_send_request("toggle", payload);
}

void ha_client_set_light_color(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
    const config_t *cfg = config_store_get();
    if (!cfg || !cfg->ha.entity_id[0]) return;

    char payload[128];
    snprintf(payload, sizeof(payload), 
             "{\"entity_id\":\"%s\",\"rgb_color\":[%d,%d,%d],\"brightness\":%d}", 
             cfg->ha.entity_id, r, g, b, brightness);
    ha_client_send_request("turn_on", payload);
}
