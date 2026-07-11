// LVGL custom allocator: serve LVGL's heap from PSRAM instead of a 64 KB static
// array in internal RAM (the scarce resource on this board). Enabled by
// CONFIG_LV_USE_CUSTOM_MALLOC. LVGL allocates objects, styles and label text
// here; the SPI draw buffers are allocated separately by esp_lvgl_port, so
// rendering speed is unaffected. PSRAM is up well before lv_init() runs.
#include "lvgl.h"
#include "esp_heap_caps.h"

void lv_mem_init(void) { }
void lv_mem_deinit(void) { }

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    (void)mem;
    (void)bytes;
    return NULL;  // heap_caps owns the PSRAM pool; no extra pool to register
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }

void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    lv_memzero(mon_p, sizeof(lv_mem_monitor_t));  // not tracked with heap_caps
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}
