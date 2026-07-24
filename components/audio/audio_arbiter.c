// audio_arbiter: the single active audio source, guarded by a mutex.
#include "audio_arbiter.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "audio_arbiter";

static SemaphoreHandle_t s_lock;
static audio_source_t s_active;

esp_err_t audio_arbiter_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_active = AUDIO_SOURCE_NONE;
    return ESP_OK;
}

// Every entry point below tolerates a NULL lock: audio_init is best-effort at
// boot, and on a codec/I2C hardware failure the arbiter may never be inited.
// sendspin_task polls audio_arbiter_active() every 10 ms, so taking a NULL
// semaphore there would assert and reboot in a loop.
esp_err_t audio_arbiter_acquire(audio_source_t src)
{
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
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
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active == src) {
        s_active = AUDIO_SOURCE_NONE;
    }
    xSemaphoreGive(s_lock);
}

audio_source_t audio_arbiter_active(void)
{
    if (s_lock == NULL) return AUDIO_SOURCE_NONE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    audio_source_t a = s_active;
    xSemaphoreGive(s_lock);
    return a;
}
