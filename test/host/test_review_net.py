import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
source = (ROOT / "components/net/net.c").read_text()

def extract(name):
    match = re.search(r"^(static )?[^\n]+\b" + name + r"\([^;\n]*\)\n\{.*?^\}", source, re.M | re.S)
    if not match:
        raise AssertionError("could not find " + name)
    return match.group(0)

prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define CFG_WIFI_SLOTS 4
#define CFG_WIFI_SSID_MAX 33
#define CFG_WIFI_PASS_MAX 64
#define FAILOVER_THRESHOLD 3
#define NET_BENCH_US (10LL * 60 * 1000 * 1000)
#define ROAM_CHECK_PERIOD_US (30 * 1000 * 1000)
#define ROAM_RSSI_TRIGGER (-70)
#define ROAM_MARGIN_DB 8
#define ROAM_MIN_DWELL_US (90 * 1000 * 1000)
#define STA_RETRY_DELAY_US 1000000
#define AP_RETRY_DELAY_US 10000000

typedef int esp_err_t;
typedef int esp_event_base_t;
typedef int esp_timer_handle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define pdPASS 1
#define pdTRUE 1
#define WIFI_MODE_STA 1
#define WIFI_MODE_AP 2
#define WIFI_MODE_APSTA 3
#define WIFI_AUTH_OPEN 0
#define WIFI_AUTH_WPA2_PSK 3

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)

typedef struct {
    char ssid[CFG_WIFI_SSID_MAX];
    char pass[CFG_WIFI_PASS_MAX];
} wifi_cred_t;

typedef enum {
    NET_STATE_BOOT = 0,
    NET_STATE_CONNECTING,
    NET_STATE_CONNECTED,
    NET_STATE_PROVISIONING,
} net_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} net_ap_t;

typedef struct {
    uint8_t ssid[33];
    int8_t rssi;
    uint8_t authmode;
} wifi_ap_record_t;

typedef int SemaphoreHandle_t;
static SemaphoreHandle_t s_scan_lock = 1;

static net_state_t s_state = NET_STATE_BOOT;
static atomic_bool s_ap_up;
static atomic_bool s_ap_pending;
static int64_t s_last_roam_us;

static wifi_cred_t s_creds[CFG_WIFI_SLOTS];
static int s_cred_count;
static int s_cur;
static int s_fail_count;
static int s_bench_idx = -1;
static int64_t s_bench_until_us;

static atomic_bool s_failover_running;
static int s_failover_stays;

static int64_t mock_time_us;
static int64_t esp_timer_get_time(void) { return mock_time_us; }

static int apply_sta_config_calls;
static esp_err_t apply_sta_config(void) { apply_sta_config_calls++; return ESP_OK; }

static int schedule_reconnect_calls;
static uint64_t last_reconnect_delay;
static void schedule_reconnect(uint64_t delay_us) { schedule_reconnect_calls++; last_reconnect_delay = delay_us; }

static int vTaskDelete_calls;
static void vTaskDelete(void *p) { (void)p; vTaskDelete_calls++; }

static int esp_wifi_disconnect_calls;
static esp_err_t esp_wifi_disconnect(void) { esp_wifi_disconnect_calls++; return ESP_OK; }

static int xTaskCreate_calls;
static void (*queued_task)(void *);
static int xTaskCreate(void (*fn)(void *), const char *name, int stack, void *arg, int pri, void *out) {
    (void)name; (void)stack; (void)arg; (void)pri; (void)out;
    xTaskCreate_calls++;
    queued_task = fn;
    return pdPASS;
}

// net_scan mock
static net_ap_t mock_scan_results[10];
static size_t mock_scan_count;
static esp_err_t mock_scan_result = ESP_OK;
esp_err_t net_scan(net_ap_t *out, size_t max, size_t *count) {
    if (mock_scan_result != ESP_OK) return mock_scan_result;
    *count = mock_scan_count > max ? max : mock_scan_count;
    for (size_t i = 0; i < *count; i++) {
        out[i] = mock_scan_results[i];
    }
    return ESP_OK;
}

// esp_wifi_sta_get_ap_info mock
static wifi_ap_record_t mock_ap_info;
static esp_err_t mock_ap_info_result = ESP_OK;
static esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *info) {
    if (mock_ap_info_result != ESP_OK) return mock_ap_info_result;
    *info = mock_ap_info;
    return ESP_OK;
}

static size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t len = strlen(src);
    if (size > 0) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}

static void roam_task(void *arg);
'''

tests = r'''
static void reset(void) {
    s_state = NET_STATE_CONNECTED;
    s_ap_pending = false;
    s_failover_running = false;
    s_failover_stays = 0;
    s_cred_count = 0;
    s_cur = 0;
    s_fail_count = 0;
    s_bench_idx = -1;
    s_bench_until_us = 0;
    mock_time_us = 0;
    s_last_roam_us = 0;

    apply_sta_config_calls = 0;
    schedule_reconnect_calls = 0;
    last_reconnect_delay = 0;
    vTaskDelete_calls = 0;
    esp_wifi_disconnect_calls = 0;
    xTaskCreate_calls = 0;
    queued_task = NULL;

    mock_scan_count = 0;
    mock_scan_result = ESP_OK;
    mock_ap_info_result = ESP_OK;
    memset(&mock_ap_info, 0, sizeof(mock_ap_info));
}

int main(void) {
    // Test net_is_benched
    reset();
    s_bench_idx = 1;
    s_bench_until_us = 1000;
    mock_time_us = 500;
    assert(net_is_benched(1) == true);
    assert(net_is_benched(0) == false);
    mock_time_us = 1500;
    assert(net_is_benched(1) == false);

    // Test roam_check_cb conditions
    reset();
    s_state = NET_STATE_CONNECTING;
    roam_check_cb(NULL);
    assert(xTaskCreate_calls == 0); // Not connected

    reset();
    s_cred_count = 1;
    roam_check_cb(NULL);
    assert(xTaskCreate_calls == 0); // Only 1 network

    reset();
    s_cred_count = 2;
    mock_ap_info_result = ESP_FAIL;
    roam_check_cb(NULL);
    assert(xTaskCreate_calls == 0); // Cannot get ap info

    reset();
    s_cred_count = 2;
    mock_ap_info.rssi = -60; // Stronger than ROAM_RSSI_TRIGGER (-70)
    roam_check_cb(NULL);
    assert(xTaskCreate_calls == 0); // Signal too strong

    reset();
    s_cred_count = 2;
    mock_ap_info.rssi = -80; // Weak signal
    s_last_roam_us = 0;
    mock_time_us = ROAM_MIN_DWELL_US - 1;
    roam_check_cb(NULL);
    assert(xTaskCreate_calls == 0); // Dwell time not met

    reset();
    s_cred_count = 2;
    mock_ap_info.rssi = -80;
    s_last_roam_us = 0;
    mock_time_us = ROAM_MIN_DWELL_US + 1;
    roam_check_cb(NULL);
    assert(xTaskCreate_calls == 1); // Should trigger scan

    // Test roam_scan_and_switch
    reset();
    s_cred_count = 2;
    strlcpy(s_creds[0].ssid, "NetA", sizeof(s_creds[0].ssid));
    strlcpy(s_creds[1].ssid, "NetB", sizeof(s_creds[1].ssid));
    s_cur = 0;

    // We are on NetA, signal -80
    strlcpy((char*)mock_ap_info.ssid, "NetA", sizeof(mock_ap_info.ssid));
    mock_ap_info.rssi = -80;

    // Scan finds NetB at -75 (not enough margin)
    strlcpy(mock_scan_results[0].ssid, "NetB", sizeof(mock_scan_results[0].ssid));
    mock_scan_results[0].rssi = -75;
    mock_scan_count = 1;

    roam_scan_and_switch();
    assert(esp_wifi_disconnect_calls == 0); // No roam

    // Scan finds NetB at -70 (margin is 10, beats ROAM_MARGIN_DB of 8)
    mock_scan_results[0].rssi = -70;
    roam_scan_and_switch();
    assert(esp_wifi_disconnect_calls == 1);
    assert(s_cur == 1);
    assert(apply_sta_config_calls == 1);

    // Test failover_task
    reset();
    s_cred_count = 2;
    strlcpy(s_creds[0].ssid, "NetA", sizeof(s_creds[0].ssid));
    strlcpy(s_creds[1].ssid, "NetB", sizeof(s_creds[1].ssid));
    s_cur = 0;

    // Scan finds nothing, should round robin to 1
    mock_scan_count = 0;
    failover_task(NULL);
    assert(s_cur == 1);
    assert(apply_sta_config_calls == 1);
    assert(schedule_reconnect_calls == 1);

    // Scan finds NetA (current), s_failover_stays increments
    reset();
    s_cred_count = 2;
    strlcpy(s_creds[0].ssid, "NetA", sizeof(s_creds[0].ssid));
    strlcpy(s_creds[1].ssid, "NetB", sizeof(s_creds[1].ssid));
    s_cur = 0;

    strlcpy(mock_scan_results[0].ssid, "NetA", sizeof(mock_scan_results[0].ssid));
    mock_scan_count = 1;

    failover_task(NULL);
    assert(s_cur == 0);

    failover_task(NULL);
    assert(s_cur == 1); // Round robin fallback
    assert(s_bench_idx == 0); // NetA is benched

    // order_by_signal test
    reset();
    s_cred_count = 3;
    strlcpy(s_creds[0].ssid, "NetA", sizeof(s_creds[0].ssid));
    strlcpy(s_creds[1].ssid, "NetB", sizeof(s_creds[1].ssid));
    strlcpy(s_creds[2].ssid, "NetC", sizeof(s_creds[2].ssid));

    strlcpy(mock_scan_results[0].ssid, "NetC", sizeof(mock_scan_results[0].ssid));
    strlcpy(mock_scan_results[1].ssid, "NetA", sizeof(mock_scan_results[1].ssid));
    mock_scan_count = 2;

    order_by_signal();
    assert(strcmp(s_creds[0].ssid, "NetC") == 0);
    assert(strcmp(s_creds[1].ssid, "NetA") == 0);
    assert(strcmp(s_creds[2].ssid, "NetB") == 0);

    puts("review net tests: passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="bugne-review-net-") as tmp:
    path = pathlib.Path(tmp)
    unit = path / "net_test.c"
    unit.write_text(prelude + "\n" + extract("net_is_benched") + "\n" + extract("roam_scan_and_switch") + "\n" + extract("roam_task") + "\n" + extract("roam_check_cb") + "\n" + extract("failover_task") + "\n" + extract("order_by_signal") + "\n" + tests)
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-Wno-unused-variable", str(unit), "-o", str(path / "net_test")], check=True)
    subprocess.run([str(path / "net_test")], check=True)
