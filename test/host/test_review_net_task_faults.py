import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
source = (ROOT / "components/net/net.c").read_text()


def function(name):
    match = re.search(r"^static [^\n]+\b" + name + r"\([^;\n]*\)\n\{.*?^\}", source, re.M | re.S)
    if not match:
        raise AssertionError(name)
    return match.group(0)


prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
typedef int esp_err_t;
typedef int esp_event_base_t;
typedef int esp_timer_handle_t;
typedef struct { int reason; } wifi_event_sta_disconnected_t;
typedef struct { int value; } esp_sntp_config_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define pdPASS 1
#define WIFI_EVENT 1
#define IP_EVENT 2
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define IP_EVENT_STA_GOT_IP 3
#define WIFI_MODE_STA 1
#define WIFI_MODE_APSTA 2
#define WIFI_PS_NONE 0
#define NET_STATE_BOOT 0
#define NET_STATE_CONNECTED 1
#define NET_STATE_CONNECTING 2
#define NET_STATE_PROVISIONING 3
#define STA_RETRY_DELAY_US 1000000
#define AP_RETRY_DELAY_US 10000000
#define AP_FALLBACK_ATTEMPTS 8
#define FAILOVER_THRESHOLD 3
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(x) ((esp_sntp_config_t){0})
static int s_state, s_fail_count, s_total_fails, s_cred_count, s_failover_stays;
static bool s_want_sta, s_connected_once, s_mdns_started, s_sntp_started;
static atomic_bool s_ap_up, s_ap_pending, s_failover_running;
static esp_timer_handle_t s_reconnect_timer;
static int task_result, task_calls, connect_calls, start_calls, stop_calls, mode;
static int start_failures, connect_result, scheduled, deleted;
static uint64_t last_delay;
static void (*queued_task)(void *);
static void failover_task(void *arg) { (void)arg; }
static void captive_dns_task(void *arg) { (void)arg; }
static void start_mdns(void) {}
static int esp_netif_sntp_init(void *p) { (void)p; return ESP_OK; }
static int esp_timer_stop(int timer) { (void)timer; scheduled = 0; return ESP_OK; }
static int esp_timer_start_once(int timer, uint64_t delay) {
    (void)timer; scheduled = 1; last_delay = delay; return ESP_OK;
}
static int xTaskCreate(void (*fn)(void *), const char *name, int stack, void *arg, int pri, void *out) {
    (void)name; (void)stack; (void)arg; (void)pri; (void)out;
    task_calls++;
    if (task_result == pdPASS) queued_task = fn;
    return task_result;
}
static void vTaskDelete(void *p) { (void)p; deleted++; }
static int esp_wifi_connect(void) { connect_calls++; return connect_result; }
static int esp_wifi_stop(void) { stop_calls++; return ESP_OK; }
static int esp_wifi_set_mode(int value) { mode = value; return ESP_OK; }
static int esp_wifi_start(void) {
    start_calls++;
    if (start_failures > 0) { start_failures--; return ESP_FAIL; }
    return ESP_OK;
}
static void (*wifi_ps_hook)(void);
static int esp_wifi_set_ps(int value) {
    (void)value;
    if (wifi_ps_hook) wifi_ps_hook();
    return ESP_OK;
}
static int configure_ap(void) { return ESP_OK; }
static int apply_sta_config(void) { return ESP_OK; }
static esp_err_t bring_up_ap(void);
static void schedule_reconnect(uint64_t delay_us);
'''

tests = r'''
static void reset(void) {
    s_state = NET_STATE_CONNECTING;
    s_want_sta = true;
    s_connected_once = s_mdns_started = s_sntp_started = false;
    s_ap_up = s_ap_pending = s_failover_running = false;
    s_fail_count = s_total_fails = s_failover_stays = 0;
    s_cred_count = 2;
    task_result = pdPASS;
    task_calls = connect_calls = start_calls = stop_calls = deleted = 0;
    start_failures = 0;
    connect_result = ESP_OK;
    scheduled = 0;
    last_delay = 0;
    queued_task = NULL;
    mode = WIFI_MODE_STA;
    wifi_ps_hook = NULL;
}
static void got_ip_then_disconnect(void) {
    assert(s_ap_pending && s_ap_up);
    on_wifi_event(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(s_state == NET_STATE_CONNECTED && !scheduled);
    int count = task_calls;
    on_wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(s_state == NET_STATE_PROVISIONING);
    assert(s_ap_pending && task_calls == count && connect_calls == 0 && !scheduled);
}
int main(void) {
    reset();
    task_result = ESP_FAIL;
    s_fail_count = FAILOVER_THRESHOLD - 1;
    on_wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(task_calls == 1 && !s_failover_running && s_want_sta);
    assert(scheduled && last_delay == STA_RETRY_DELAY_US);
    reconnect_cb(NULL);
    assert(connect_calls == 1);
    task_result = pdPASS;
    on_wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(s_failover_running && queued_task == failover_task);

    reset();
    s_total_fails = AP_FALLBACK_ATTEMPTS;
    task_result = ESP_FAIL;
    for (int i = 0; i < 3; i++) {
        reconnect_cb(NULL);
        assert(!s_ap_pending && !s_ap_up && s_want_sta);
        assert(scheduled && last_delay == AP_RETRY_DELAY_US);
    }
    assert(task_calls == 3 && connect_calls == 3);
    task_result = pdPASS;
    reconnect_cb(NULL);
    assert(s_ap_pending && !s_ap_up && queued_task == ap_fallback_task);
    int count = task_calls;
    reconnect_cb(NULL);
    assert(task_calls == count);
    s_state = NET_STATE_CONNECTED;
    ap_fallback_task(NULL);
    assert(!s_ap_pending && !s_ap_up && start_calls == 0);
    assert(s_state == NET_STATE_CONNECTED);

    reset();
    s_total_fails = AP_FALLBACK_ATTEMPTS;
    reconnect_cb(NULL);
    start_failures = 1;
    ap_fallback_task(NULL);
    assert(!s_ap_pending && !s_ap_up && s_want_sta);
    assert(s_state == NET_STATE_CONNECTING && mode == WIFI_MODE_STA);
    assert(start_calls == 2 && stop_calls == 2);
    assert(scheduled && last_delay == AP_RETRY_DELAY_US);
    reconnect_cb(NULL);
    assert(s_ap_pending && !s_ap_up);
    ap_fallback_task(NULL);
    assert(!s_ap_pending && s_ap_up && mode == WIFI_MODE_APSTA);
    assert(s_state == NET_STATE_PROVISIONING);
    assert(scheduled && last_delay == AP_RETRY_DELAY_US);
    reconnect_cb(NULL);
    assert(connect_calls == 1);

    reset();
    s_total_fails = AP_FALLBACK_ATTEMPTS - 1;
    s_fail_count = FAILOVER_THRESHOLD;
    on_wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(task_calls == 0 && scheduled && last_delay == AP_RETRY_DELAY_US);
    reconnect_cb(NULL);
    assert(queued_task == ap_fallback_task);
    on_wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    on_wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(connect_calls == 0 && task_calls == 1);

    reset();
    s_total_fails = AP_FALLBACK_ATTEMPTS;
    reconnect_cb(NULL);
    wifi_ps_hook = got_ip_then_disconnect;
    ap_fallback_task(NULL);
    assert(!s_ap_pending && s_ap_up && s_state == NET_STATE_PROVISIONING);
    assert(scheduled && last_delay == AP_RETRY_DELAY_US);
    reconnect_cb(NULL);
    assert(connect_calls == 1);

    reset();
    s_total_fails = AP_FALLBACK_ATTEMPTS;
    reconnect_cb(NULL);
    on_wifi_event(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    on_wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(s_state == NET_STATE_CONNECTING && s_ap_pending && !scheduled);
    ap_fallback_task(NULL);
    assert(!s_ap_pending && scheduled && last_delay == AP_RETRY_DELAY_US);
    reconnect_cb(NULL);
    assert(connect_calls == 1);

    reset();
    connect_result = ESP_FAIL;
    reconnect_cb(NULL);
    assert(scheduled && last_delay == STA_RETRY_DELAY_US);
    puts("review net fault tests: passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="bugne-review-net-") as tmp:
    path = pathlib.Path(tmp)
    unit = path / "faults.c"
    unit.write_text(prelude + "\n".join(function(name) for name in (
        "ap_fallback_task", "reconnect_cb", "schedule_reconnect", "on_wifi_event", "bring_up_ap"
    )) + tests)
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-Wno-unused-variable", str(unit), "-o", str(path / "faults")], check=True)
    subprocess.run([str(path / "faults")], check=True)
