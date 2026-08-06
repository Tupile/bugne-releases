#include "art.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "jpeg_decoder.h"

static const char *TAG = "art";

static SemaphoreHandle_t s_lock;
static uint8_t  *s_px;      // decoded RGB565, PSRAM, NULL when empty
static uint16_t  s_w, s_h;
static uint32_t  s_gen;

// The slot is touched by the playback worker (tag callbacks) and by the LVGL
// task (art_take), so it needs a lock; it is created on first use, which is
// always the worker before the UI can see a new generation.
static SemaphoreHandle_t lock_get(void)
{
    if (!s_lock) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        if (!m) return NULL;
        // No race in practice (first call is the worker), but keep it cheap
        // and idempotent rather than relying on that.
        if (s_lock) vSemaphoreDelete(m);
        else s_lock = m;
    }
    return s_lock;
}

static void slot_clear_locked(void)
{
    if (s_px) heap_caps_free(s_px);
    s_px = NULL;
    s_w = s_h = 0;
}

bool art_set_jpeg(const uint8_t *jpeg, size_t len)
{
    if (!jpeg || len < 4 || len > ART_MAX_JPEG) return false;

    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = (uint32_t)len,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t info = {0};
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) return false;

    // TJpgDec only scales by 1/2, 1/4 or 1/8 while decoding, which is too
    // coarse to land on ART_BOX. Decode at the smallest step that still
    // covers the box (so no full-size intermediate is allocated), then
    // resample down to the exact size below.
    uint16_t big = info.width > info.height ? info.width : info.height;
    if      (big <= ART_BOX * 2) cfg.out_scale = JPEG_IMAGE_SCALE_0;
    else if (big <= ART_BOX * 4) cfg.out_scale = JPEG_IMAGE_SCALE_1_2;
    else if (big <= ART_BOX * 8) cfg.out_scale = JPEG_IMAGE_SCALE_1_4;
    else                         cfg.out_scale = JPEG_IMAGE_SCALE_1_8;
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) return false;
    // NOTE: info.width/height stay the ORIGINAL pixel size here (out_scale is
    // only reflected in output_len and in what esp_jpeg_decode reports), so
    // bound the allocation on output_len, never on those two fields.
    if (info.output_len > (size_t)(ART_BOX * 2) * (ART_BOX * 2) * 2) {
        ESP_LOGW(TAG, "cover %ux%u too large even at 1/8, skipped", info.width, info.height);
        return false;
    }

    uint8_t *px = heap_caps_malloc(info.output_len, MALLOC_CAP_SPIRAM);
    if (!px) return false;
    cfg.outbuf = px;
    cfg.outbuf_size = info.output_len;
    if (esp_jpeg_decode(&cfg, &info) != ESP_OK) {
        heap_caps_free(px);
        return false;
    }

    uint16_t ow = info.width, oh = info.height;
    if (ow > ART_BOX || oh > ART_BOX) {
        // Nearest-neighbour to the exact box. The JPEG step above already put
        // us within 2x, so this only ever drops a few rows and columns;
        // proper filtering is not worth the code at 120 px on a 2.8" panel.
        uint16_t nw = ow >= oh ? ART_BOX : (uint16_t)(ow * ART_BOX / oh);
        uint16_t nh = oh >= ow ? ART_BOX : (uint16_t)(oh * ART_BOX / ow);
        if (nw == 0) nw = 1;
        if (nh == 0) nh = 1;
        uint16_t *dst = heap_caps_malloc((size_t)nw * nh * 2, MALLOC_CAP_SPIRAM);
        if (!dst) { heap_caps_free(px); return false; }
        const uint16_t *src = (const uint16_t *)px;
        for (uint16_t y = 0; y < nh; y++) {
            const uint16_t *srow = src + (size_t)(y * oh / nh) * ow;
            uint16_t *drow = dst + (size_t)y * nw;
            for (uint16_t x = 0; x < nw; x++) drow[x] = srow[x * ow / nw];
        }
        heap_caps_free(px);
        px = (uint8_t *)dst;
        info.width = nw;
        info.height = nh;
        info.output_len = (size_t)nw * nh * 2;
    }

    SemaphoreHandle_t m = lock_get();
    if (!m) { heap_caps_free(px); return false; }
    xSemaphoreTake(m, portMAX_DELAY);
    slot_clear_locked();  // an untaken older bitmap belongs to us
    s_px = px;
    s_w = info.width;
    s_h = info.height;
    s_gen++;
    xSemaphoreGive(m);
    ESP_LOGI(TAG, "cover %ux%u (%u B)", s_w, s_h, (unsigned)info.output_len);
    return true;
}

void art_clear(void)
{
    SemaphoreHandle_t m = lock_get();
    if (!m) return;
    xSemaphoreTake(m, portMAX_DELAY);
    slot_clear_locked();
    s_gen++;
    xSemaphoreGive(m);
}

uint32_t art_gen(void)
{
    return s_gen;  // aligned 32-bit read, no lock needed
}

bool art_take(uint8_t **px, uint16_t *w, uint16_t *h)
{
    SemaphoreHandle_t m = lock_get();
    if (!m) return false;
    xSemaphoreTake(m, portMAX_DELAY);
    bool have = s_px != NULL;
    if (have) {
        if (px) *px = s_px;
        if (w) *w = s_w;
        if (h) *h = s_h;
        s_px = NULL;  // ownership moves to the caller
        s_w = s_h = 0;
    }
    xSemaphoreGive(m);
    return have;
}
