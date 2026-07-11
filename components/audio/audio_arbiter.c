// audio_arbiter: single active source plus a syncing flag, guarded by a mutex.
#include "audio_arbiter.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "audio_arbiter";

static SemaphoreHandle_t s_lock;
static audio_source_t s_active;
static bool s_syncing;

esp_err_t audio_arbiter_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_active = AUDIO_SOURCE_NONE;
    s_syncing = false;
    return ESP_OK;
}

esp_err_t audio_arbiter_acquire(audio_source_t src)
{
    esp_err_t ret = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active == AUDIO_SOURCE_NONE || s_active == src) {
        s_active = src;
    } else {
        ret = ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_lock);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "source %d busy, active is %d", src, s_active);
    }
    return ret;
}

void audio_arbiter_release(audio_source_t src)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active == src) {
        s_active = AUDIO_SOURCE_NONE;
    }
    xSemaphoreGive(s_lock);
}

audio_source_t audio_arbiter_active(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    audio_source_t a = s_active;
    xSemaphoreGive(s_lock);
    return a;
}

void audio_arbiter_set_syncing(bool syncing)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_syncing = syncing;
    xSemaphoreGive(s_lock);
}

bool audio_arbiter_is_syncing(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool s = s_syncing;
    xSemaphoreGive(s_lock);
    return s;
}
