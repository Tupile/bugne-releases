// Bugne app entry point.
//
// Boots the foundation in dependency order. config_store and board are required
// (a failure there is fatal). The rest are best-effort: a failure is logged but
// the device keeps running, so a missing codec, SD card, or network never
// reboot-loops the whole unit.

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "logstore.h"
#include "config_store.h"
#include "board.h"
#include "audio.h"
#include "net.h"
#include "web_config.h"
#include "ui.h"
#include "source_sd.h"
#include "library.h"
#include "source_stream.h"
#include "source_sendspin.h"
#include "played.h"
#include "stats.h"
#include "memo.h"

static const char *TAG = "bugne";

// cJSON has no PSRAM allocator by default, so its DOM nodes and duplicated
// strings land in internal RAM (the scarce resource). A single config save
// (config_store's save_to_disk, ~50 podcasts + 32 webradios + favorites +
// alarms) builds a ~500-node tree plus a cJSON_Print buffer: a 40-55 KB
// internal transient. During an active HTTPS stream (internal RAM already
// down to ~25 KB free) this drove min-free to 16 bytes on the bench
// (2026-07-05). Routing cJSON's malloc/free through these hooks moves that
// transient (and every other cJSON user: web_config, podcast, stats) to
// PSRAM. Safe with LittleFS: lfs_file_write copies the source into its own
// internal prog cache before any flash-cache-disable window, so a PSRAM
// source is fully consumed beforehand.
static void *json_psram_malloc(size_t sz) { return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM); }
static void  json_psram_free(void *p)     { heap_caps_free(p); }

// After an OTA update the new image boots as "pending verify". Confirm it healthy
// only after a stable uptime, so an image that crash-loops or dies a few seconds
// in is rolled back to the previous slot by the bootloader. On a USB-flashed boot
// the image is not pending verify, so this is a no-op.
#define OTA_VALIDATE_DELAY_MS 30000

static void ota_validate_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(OTA_VALIDATE_DELAY_MS));
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "OTA image confirmed healthy, rollback canceled");
        }
    }
    vTaskDelete(NULL);
}

// Periodic memory telemetry, retrievable over /api/logs. Internal RAM is the
// scarce resource (PSRAM is plentiful): the free and minimum-free numbers are
// what to watch when judging a memory tuning change during HTTPS streaming.
static void heap_log_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        ESP_LOGI(TAG, "internal RAM: %u free, %u min free",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    }
}

// Run a best-effort init: log on failure but keep going.
#define TRY(call) do {                                            \
    esp_err_t _err = (call);                                      \
    if (_err != ESP_OK) {                                         \
        ESP_LOGE(TAG, #call " failed: %s", esp_err_to_name(_err));\
    }                                                             \
} while (0)

// Slow subsystems, brought up after the UI is already on screen (see app_main).
// Order matters: web_config after net, library after the SD mount. Internal
// stack (plain xTaskCreate): net/Wi-Fi init writes NVS, which is a flash
// operation and therefore forbidden from a PSRAM stack.
static void bg_init_task(void *arg)
{
    (void)arg;
    played_init();  // LittleFS is already mounted (config_store_init ran in app_main)
    stats_init();   // load listening stats from LittleFS (C3)
    TRY(net_start());
    // Serve the config page once the network is up. On success, also wire the
    // daily GitHub update check (A1) into ui's maintenance engine: ui cannot
    // REQUIRE web_config back (web_config already depends on ui), so this
    // function pointer is the one-way bridge. Left NULL on failure, which
    // makes the scheduled check phase in worker_run_job a silent no-op.
    esp_err_t wc_err = web_config_start();
    if (wc_err == ESP_OK) {
        ui_set_ghota_check_fn(web_config_gh_check);
    } else {
        ESP_LOGE(TAG, "web_config_start() failed: %s", esp_err_to_name(wc_err));
    }
    TRY(source_sd_init());
    memo_clean_parts();  // drop memo temporaries left by a power cut mid-record/receive
    memo_clean_talkie(); // drop ephemeral walkie-talkie files left by a power cut
    library_load();  // load the SD music index if present (best-effort, no card = no-op)
    TRY(source_stream_init());
    TRY(source_sendspin_init());
    ESP_LOGI(TAG, "background init complete");
    vTaskDelete(NULL);
}

void app_main(void)
{
    // Route cJSON's heap to PSRAM before the first cJSON user (config_store_init,
    // right below) runs. See json_psram_malloc/free above.
    cJSON_Hooks json_hooks = { .malloc_fn = json_psram_malloc, .free_fn = json_psram_free };
    cJSON_InitHooks(&json_hooks);

    // Capture logs into a PSRAM ring buffer first, so boot logs are retrievable
    // over Wi-Fi (/api/logs). Best-effort: the console still works without it.
    TRY(logstore_init());

    ESP_LOGI(TAG, "Bugne booting");

    // Dynamic frequency scaling: run at 240 MHz under load (decode, active UI),
    // drop to 160 MHz when idle to save power. The audio (I2S), display (SPI) and
    // SD drivers hold an APB-max lock during transfers, so playback and drawing
    // stay glitch-free; the CPU only slows when nothing is using those buses.
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 160,
        .light_sleep_enable = false,
    };
    TRY(esp_pm_configure(&pm));
#endif

    // Required: configuration and the shared I2C bus / device identity.
    ESP_ERROR_CHECK(config_store_init());
    ESP_ERROR_CHECK(board_init());
    i2c_master_bus_handle_t i2c_bus = board_i2c_bus();
    ESP_LOGI(TAG, "device id: %s", board_device_id());

    // Best-effort from here: the UI and network must come up even if audio or a
    // source fails to initialize.
    TRY(audio_init(i2c_bus));
    // Screen first: the display is usable in well under a second instead of
    // waiting behind the Wi-Fi scan (~2-4 s with several networks) and the SD
    // mount. The slow subsystems follow on bg_init_task; the UI tolerates them
    // arriving late (network sources grey out until connected, SD screens show
    // "no card" until the mount lands, and ui.c rebuilds on state changes).
    TRY(ui_start(i2c_bus));
    xTaskCreate(bg_init_task, "bg_init", 8192, NULL, 4, NULL);

    ESP_LOGI(TAG, "Bugne boot complete (UI up, background init running)");

    // Confirm an OTA image healthy after a stable uptime (rollback safety net).
    xTaskCreate(ota_validate_task, "ota_valid", 3072, NULL, 1, NULL);
    xTaskCreate(heap_log_task, "heap_log", 3072, NULL, 1, NULL);  // vprintf needs headroom
}
