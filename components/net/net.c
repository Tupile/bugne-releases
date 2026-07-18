// net: Wi-Fi state machine (station with AP provisioning fallback) and mDNS.
#include "net.h"
#include "board.h"
#include "config_store.h"

#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "mdns.h"
#include "lwip/sockets.h"

static const char *TAG = "net";

// Reconnect spacing. While STA-only (the normal case) there is no AP to protect,
// so retry fast for a quick connect and failover. Once the setup AP is up, a tight
// reconnect loop starves the AP beacon and hides the hotspot, so retry slowly.
#define STA_RETRY_DELAY_US (1 * 1000 * 1000)
#define AP_RETRY_DELAY_US  (10 * 1000 * 1000)

// Total failed station attempts (before the first successful connect) after which
// we give up on a quiet STA-only connect and raise the setup AP as a fallback, so
// the device stays reachable for reconfiguration. ~8 attempts is both networks
// tried more than a full round.
#define AP_FALLBACK_ATTEMPTS 8

// Stored station credentials. Up to CFG_WIFI_SLOTS networks. Only the configured
// (non-empty) ones are kept here. With 2+ networks the order is rewritten at boot
// to strongest-visible-first (see order_by_signal).
typedef struct {
    char ssid[CFG_WIFI_SSID_MAX];
    char pass[CFG_WIFI_PASS_MAX];
} wifi_cred_t;

// Consecutive failed connect attempts on the current network before scanning
// for the best configured one (only relevant when more than one is configured).
#define FAILOVER_THRESHOLD 3

// Roaming: while connected, periodically check the current AP's signal. Only when
// it is weak (<= ROAM_RSSI_TRIGGER) do we scan for a configured network that is at
// least ROAM_MARGIN_DB stronger and switch to it. Scanning briefly disturbs an
// active stream, so it happens only on already-weak signal and is rate-limited to
// once per ROAM_MIN_DWELL_US whether or not it leads to a switch (hysteresis: a
// weak link with no better AP nearby is not re-scanned every check, and two close
// APs do not cause flapping).
#define ROAM_CHECK_PERIOD_US (30 * 1000 * 1000)
#define ROAM_RSSI_TRIGGER    (-70)            // dBm, below this we look for better
#define ROAM_MARGIN_DB       8                // candidate must beat current by this
#define ROAM_MIN_DWELL_US    (90 * 1000 * 1000)

static net_state_t s_state = NET_STATE_BOOT;
static bool s_want_sta;        // we have credentials and want to be a station
static bool s_connected_once;  // first successful station connect happened
static bool s_mdns_started;    // mDNS started once after connecting
static bool s_sntp_started;    // SNTP started once after connecting
static bool s_ap_up;           // the setup AP is running (provisioning or fallback)
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_roam_timer;
static int64_t s_last_roam_us;   // when we last switched networks (dwell guard)

static wifi_cred_t s_creds[CFG_WIFI_SLOTS];
static int s_cred_count;   // number of configured networks (0..CFG_WIFI_SLOTS)
static int s_cur;          // index of the network we are currently trying
static int s_fail_count;   // consecutive failures on s_cur (drives failover)
static int s_total_fails;  // total failures before first connect (drives AP fallback)

static void start_mdns(void);    // defined below
static esp_err_t configure_ap(void);  // defined below
static void bring_up_ap(void);   // defined below

// Apply the credentials of slot s_cur to the station interface.
static void apply_sta_config(void)
{
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, s_creds[s_cur].ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, s_creds[s_cur].pass, sizeof(sta.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &sta);
}

// After too many failures on the current network, pick the strongest VISIBLE
// configured network instead of a blind round-robin: a transient AP glitch
// (3 quick disconnects fit in seconds) must not push a weak-spot device onto a
// much weaker backup, e.g. from the box at -68 dBm to a repeater at -80 dBm
// (observed flip-flop on a fleet unit, 2026-07-10). Runs in a short-lived task:
// the scan blocks and must not run on the event task.
static volatile bool s_failover_running;  // one failover task at a time
static int s_failover_stays;              // consecutive "stay on current" picks

static void failover_task(void *arg)
{
    (void)arg;
    net_ap_t aps[24];
    size_t n = 0;
    int pick = -1;
    if (net_scan(aps, sizeof(aps) / sizeof(aps[0]), &n) == ESP_OK) {
        for (size_t i = 0; i < n && pick < 0; i++) {  // strongest first
            for (int c = 0; c < s_cred_count; c++) {
                if (strcmp(s_creds[c].ssid, aps[i].ssid) == 0) { pick = c; break; }
            }
        }
    }
    if (pick < 0) {
        // Nothing configured is visible: blind round-robin, nothing better to do.
        pick = (s_cur + 1) % s_cred_count;
        ESP_LOGW(TAG, "failover: no configured network visible, trying %s",
                 s_creds[pick].ssid);
        s_failover_stays = 0;
    } else if (pick == s_cur) {
        // Still the strongest visible: a transient glitch, stay put. But if
        // staying keeps failing (e.g. the AP answers beacons yet refuses us),
        // fall back to the round-robin escape hatch on the second stay.
        if (++s_failover_stays >= 2) {
            pick = (s_cur + 1) % s_cred_count;
            s_failover_stays = 0;
            ESP_LOGW(TAG, "failover: %s visible but not joinable, trying %s",
                     s_creds[s_cur].ssid, s_creds[pick].ssid);
        } else {
            ESP_LOGI(TAG, "failover: staying on %s (still the strongest visible)",
                     s_creds[s_cur].ssid);
        }
    } else {
        s_failover_stays = 0;
        ESP_LOGW(TAG, "failing over to SSID %s", s_creds[pick].ssid);
    }
    s_cur = pick;
    s_fail_count = 0;
    apply_sta_config();
    s_failover_running = false;
    esp_wifi_connect();
    vTaskDelete(NULL);
}

// Raise the setup AP from a task (heavy Wi-Fi calls need a real stack, not the
// timer task's). Skipped if we connected in the meantime.
static void ap_fallback_task(void *arg)
{
    (void)arg;
    bring_up_ap();
    vTaskDelete(NULL);
}

static void reconnect_cb(void *arg)
{
    (void)arg;
    if (!s_want_sta) {
        return;
    }
    // Could not reach any configured network: bring up the setup AP so the device
    // stays reachable, then keep retrying Wi-Fi in the background.
    if (!s_ap_up && s_total_fails >= AP_FALLBACK_ATTEMPTS) {
        s_ap_up = true;  // commit now so we do not spawn this twice
        xTaskCreate(ap_fallback_task, "ap_fallback", 4096, NULL, 5, NULL);
        return;
    }
    esp_wifi_connect();
}

static void schedule_reconnect(uint64_t delay_us)
{
    esp_timer_stop(s_reconnect_timer);  // ignore error if not running
    esp_timer_start_once(s_reconnect_timer, delay_us);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_want_sta) esp_wifi_connect();  // first attempt immediately
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_want_sta) {
            const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
            int reason = d ? d->reason : 0;
            s_fail_count++;
            if (!s_connected_once) s_total_fails++;
            if (s_cred_count >= 2 && s_fail_count >= FAILOVER_THRESHOLD) {
                // Repeated failures: let the failover task scan, pick the best
                // configured network and reconnect (it clears s_fail_count).
                if (!s_failover_running) {
                    s_failover_running = true;
                    ESP_LOGW(TAG, "station disconnected (reason %d), failover scan", reason);
                    xTaskCreate(failover_task, "wifi_failover", 4096, NULL, 5, NULL);
                }
            } else if (s_connected_once) {
                // Drop after a successful join: reconnect immediately for fast recovery.
                ESP_LOGW(TAG, "station disconnected (reason %d), reconnecting", reason);
                esp_wifi_connect();
            } else {
                // Never connected yet: retry fast (STA-only) until we fall back to
                // the AP, then slowly so the AP keeps beaconing.
                ESP_LOGW(TAG, "station disconnected (reason %d), retrying", reason);
                schedule_reconnect(s_ap_up ? AP_RETRY_DELAY_US : STA_RETRY_DELAY_US);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        esp_timer_stop(s_reconnect_timer);
        s_connected_once = true;
        s_fail_count = 0;
        s_total_fails = 0;
        s_failover_stays = 0;
        // Initialize mDNS before announcing CONNECTED: other tasks (sendspin)
        // advertise services as soon as they see CONNECTED, and mdns_service_add
        // fails with INVALID_ARG if mdns_init has not run yet.
        if (!s_mdns_started) {
            start_mdns();
            s_mdns_started = true;
        }
        // Start SNTP once. lwIP re-syncs hourly on its own
        // (CONFIG_LWIP_SNTP_UPDATE_DELAY) and survives Wi-Fi drops/reconnects.
        if (!s_sntp_started) {
            esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            if (esp_netif_sntp_init(&sc) == ESP_OK) {
                s_sntp_started = true;
                ESP_LOGI(TAG, "SNTP started (pool.ntp.org)");
            } else {
                ESP_LOGW(TAG, "SNTP init failed");
            }
        }
        s_state = NET_STATE_CONNECTED;
    }
}

static void start_mdns(void)
{
    char host[16];
    snprintf(host, sizeof(host), "bugne-%s", board_device_id());
    for (char *p = host; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }
    char instance[24];
    snprintf(instance, sizeof(instance), "Bugne %s", board_device_id());

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(host);
        mdns_instance_name_set(instance);
        ESP_LOGI(TAG, "mDNS hostname %s.local", host);

        // Advertise the device API for discovery (Home Assistant and similar).
        // "name" is the raw user-set device name (empty if unset); consumers
        // apply their own fallback, this does not send "Bugne <id>".
        const config_t *c = config_store_get();
        mdns_txt_item_t txt[] = {
            {"id", board_device_id()},
            {"version", esp_app_get_description()->version},
            {"name", (c && c->device_name[0]) ? c->device_name : ""},
        };
        if (mdns_service_add(instance, "_bugne", "_tcp", 80, txt, 3) == ESP_OK) {
            ESP_LOGI(TAG, "mDNS service _bugne._tcp registered");
        } else {
            ESP_LOGW(TAG, "mDNS service _bugne._tcp registration failed");
        }
    } else {
        ESP_LOGW(TAG, "mDNS init failed");
    }
}

int net_memo_peers(net_peer_t *out, int max)
{
    if (s_state != NET_STATE_CONNECTED || max <= 0) return 0;
    mdns_result_t *results = NULL;
    if (mdns_query_ptr("_bugne", "_tcp", 2500, 8, &results) != ESP_OK) return 0;
    int count = 0;
    for (mdns_result_t *r = results; r && count < max; r = r->next) {
        const char *id = NULL, *name = NULL;
        for (size_t i = 0; i < r->txt_count; i++) {
            if (strcmp(r->txt[i].key, "id") == 0) id = r->txt[i].value;
            else if (strcmp(r->txt[i].key, "name") == 0) name = r->txt[i].value;
        }
        if (id && strcasecmp(id, board_device_id()) == 0) continue;  // this device
        mdns_ip_addr_t *a = r->addr;
        while (a && a->addr.type != ESP_IPADDR_TYPE_V4) a = a->next;
        if (!a) continue;
        net_peer_t *p = &out[count];
        if (name && name[0]) strlcpy(p->name, name, sizeof(p->name));
        else snprintf(p->name, sizeof(p->name), "Bugne %s", id ? id : "?");
        esp_ip4addr_ntoa(&a->addr.u_addr.ip4, p->ip, sizeof(p->ip));
        p->port = r->port ? r->port : 80;
        count++;
    }
    mdns_query_results_free(results);
    return count;
}

// Minimal captive DNS: answer every A query with the AP gateway (192.168.4.1),
// so any hostname a client probes resolves to us and pops the captive portal.
#define DNS_PORT 53

static void captive_dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "captive DNS socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "captive DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    while (1) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
        if (len < 12) {
            continue;
        }
        buf[2] |= 0x80;  // mark as response
        buf[3] = 0x00;   // no error
        buf[6] = 0x00; buf[7] = 0x01;  // one answer

        int p = 12;  // walk past the question name
        while (p < len && buf[p] != 0) {
            p += buf[p] + 1;
        }
        p += 1 + 4;  // null label + qtype + qclass
        if (p < 0 || p + 16 > (int)sizeof(buf) || p > len) {
            continue;
        }
        buf[p++] = 0xC0; buf[p++] = 0x0C;            // pointer to the question name
        buf[p++] = 0x00; buf[p++] = 0x01;            // type A
        buf[p++] = 0x00; buf[p++] = 0x01;            // class IN
        buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x3C; // TTL 60s
        buf[p++] = 0x00; buf[p++] = 0x04;            // RDLENGTH 4
        buf[p++] = 192; buf[p++] = 168; buf[p++] = 4; buf[p++] = 1;
        sendto(sock, buf, p, 0, (struct sockaddr *)&src, slen);
    }
}

// Configure the setup AP (Bugne-Setup-XXXX, WPA2, per-device MAC-derived password).
static esp_err_t configure_ap(void)
{
    wifi_config_t ap = {0};
    int n = snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "Bugne-Setup-%s", board_device_id());
    ap.ap.ssid_len = (uint8_t)n;
    strlcpy((char *)ap.ap.password, board_ap_password(), sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;
    return esp_wifi_set_config(WIFI_IF_AP, &ap);
}

// Switch from STA-only to APSTA to raise the setup AP as a fallback. A bare
// set_mode does not reliably start the AP on this stack, so do a clean stop/start.
// Keeps the station config so it keeps retrying once the AP is up.
static void bring_up_ap(void)
{
    if (s_state == NET_STATE_CONNECTED) {
        return;  // connected in the meantime: no AP needed
    }
    ESP_LOGW(TAG, "no configured network reachable, raising setup AP");
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    configure_ap();
    apply_sta_config();
    esp_wifi_start();  // STA_START fires and the connect retries resume
    esp_wifi_set_ps(WIFI_PS_NONE);
    s_state = NET_STATE_PROVISIONING;
    xTaskCreate(captive_dns_task, "captive_dns", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "setup AP up: SSID Bugne-Setup-%s (still retrying Wi-Fi)", board_device_id());
}

// Reorder s_creds strongest-visible-first via a one-shot scan. Networks seen in
// the scan come first (strongest RSSI first), any not currently visible follow in
// their original order so they are still tried. Leaves the order unchanged on a
// scan failure. Only meaningful with 2+ configured networks.
static void order_by_signal(void)
{
    net_ap_t aps[24];
    size_t n = 0;
    if (net_scan(aps, sizeof(aps) / sizeof(aps[0]), &n) != ESP_OK || n == 0) {
        return;
    }
    // Static (not on the stack): with up to CFG_WIFI_SLOTS networks these would add
    // ~1.6 KB to a tight task stack. order_by_signal is only called from the
    // serialized connect path, so a single shared copy is safe.
    static wifi_cred_t ordered[CFG_WIFI_SLOTS];
    static bool used[CFG_WIFI_SLOTS];
    memset(used, 0, sizeof(used));
    int on = 0;
    for (size_t i = 0; i < n && on < s_cred_count; i++) {  // aps are strongest first
        for (int c = 0; c < s_cred_count; c++) {
            if (!used[c] && strcmp(s_creds[c].ssid, aps[i].ssid) == 0) {
                ordered[on++] = s_creds[c];
                used[c] = true;
                break;
            }
        }
    }
    for (int c = 0; c < s_cred_count; c++) {  // append not-visible networks
        if (!used[c]) ordered[on++] = s_creds[c];
    }
    memcpy(s_creds, ordered, sizeof(wifi_cred_t) * s_cred_count);
    ESP_LOGI(TAG, "ordered networks by signal, strongest visible: %s", s_creds[0].ssid);
}

// Run a scan and, if a configured network is meaningfully stronger than the one
// we are on, switch to it. Called from a short-lived task (scanning needs a real
// stack and blocks). The disconnect makes the event handler reconnect using the
// freshly applied config.
static void roam_scan_and_switch(void)
{
    wifi_ap_record_t cur;
    if (esp_wifi_sta_get_ap_info(&cur) != ESP_OK) {
        return;
    }
    net_ap_t aps[24];
    size_t n = 0;
    if (net_scan(aps, sizeof(aps) / sizeof(aps[0]), &n) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < n; i++) {  // strongest first: first qualifying is best
        if (strcmp(aps[i].ssid, (const char *)cur.ssid) == 0) {
            continue;  // that is the network we are already on
        }
        if (aps[i].rssi <= cur.rssi + ROAM_MARGIN_DB) {
            continue;  // not enough margin to bother
        }
        for (int c = 0; c < s_cred_count; c++) {
            if (strcmp(s_creds[c].ssid, aps[i].ssid) == 0) {
                s_cur = c;
                apply_sta_config();
                s_last_roam_us = esp_timer_get_time();
                ESP_LOGI(TAG, "roaming to %s (%d dBm) from %s (%d dBm)",
                         s_creds[c].ssid, aps[i].rssi, (const char *)cur.ssid, cur.rssi);
                esp_wifi_disconnect();  // handler reconnects to the new config
                return;
            }
        }
    }
}

static void roam_task(void *arg)
{
    (void)arg;
    roam_scan_and_switch();
    vTaskDelete(NULL);
}

// Cheap periodic check (no scan): only when connected, with 2+ networks, on weak
// signal, and after the dwell time, hand off to roam_task for the actual scan.
static void roam_check_cb(void *arg)
{
    (void)arg;
    if (s_state != NET_STATE_CONNECTED || s_cred_count < 2) {
        return;
    }
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) != ESP_OK || info.rssi > ROAM_RSSI_TRIGGER) {
        return;  // no current AP info, or signal is fine
    }
    if (esp_timer_get_time() - s_last_roam_us < ROAM_MIN_DWELL_US) {
        return;  // scanned/roamed recently: let it settle
    }
    // Rate-limit scans regardless of outcome: a weak link with no stronger
    // configured AP nearby must not be re-scanned on every check.
    s_last_roam_us = esp_timer_get_time();
    xTaskCreate(roam_task, "wifi_roam", 4096, NULL, 4, NULL);
}

esp_err_t net_start(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL), TAG, "wifi evt reg failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL), TAG, "ip evt reg failed");

    const esp_timer_create_args_t rc_args = { .callback = reconnect_cb, .name = "wifi_reconnect" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&rc_args, &s_reconnect_timer), TAG, "reconnect timer create failed");

    const esp_timer_create_args_t roam_args = { .callback = roam_check_cb, .name = "wifi_roam_check" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&roam_args, &s_roam_timer), TAG, "roam timer create failed");

    // Load all credential slots, keeping only the configured ones in slot order.
    s_cred_count = 0;
    for (int slot = 0; slot < CFG_WIFI_SLOTS; slot++) {
        char ssid[CFG_WIFI_SSID_MAX] = {0};
        char pass[CFG_WIFI_PASS_MAX] = {0};
        if (config_store_get_wifi_slot(slot, ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK
            && ssid[0] != '\0') {
            strlcpy(s_creds[s_cred_count].ssid, ssid, sizeof(s_creds[s_cred_count].ssid));
            strlcpy(s_creds[s_cred_count].pass, pass, sizeof(s_creds[s_cred_count].pass));
            s_cred_count++;
        }
    }

    if (s_cred_count > 0) {
        // Normal case: STA-only, no AP overhead, fast connect. The setup AP comes
        // up only if we cannot reach any configured network (see reconnect_cb).
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA mode failed");
        if (s_cred_count == 1) {
            // Single network: keep the validated fast path (no scan, ~2.5s connect).
            s_cur = 0;
            apply_sta_config();
            s_want_sta = true;
            ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
            esp_wifi_set_ps(WIFI_PS_NONE);  // disable power save: fewer flaky drops
        } else {
            // Multiple networks: scan first, then connect to the strongest visible
            // one. Keep s_want_sta off so STA_START does not auto-connect to the
            // primary before we have picked; connect manually after ordering.
            s_want_sta = false;
            ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
            esp_wifi_set_ps(WIFI_PS_NONE);
            order_by_signal();
            s_cur = 0;
            apply_sta_config();
            s_want_sta = true;
            esp_wifi_connect();
        }
        s_state = NET_STATE_CONNECTING;
        esp_timer_start_periodic(s_roam_timer, ROAM_CHECK_PERIOD_US);
        ESP_LOGI(TAG, "%d network(s) configured, connecting (STA-only) to SSID %s",
                 s_cred_count, s_creds[s_cur].ssid);
    } else {
        // No credentials: bring up the setup AP for initial provisioning.
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set APSTA mode failed");
        ESP_RETURN_ON_ERROR(configure_ap(), TAG, "set AP config failed");
        s_ap_up = true;
        s_state = NET_STATE_PROVISIONING;
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
        esp_wifi_set_ps(WIFI_PS_NONE);
        xTaskCreate(captive_dns_task, "captive_dns", 4096, NULL, 5, NULL);
        ESP_LOGI(TAG, "no stored credentials, provisioning AP up: SSID Bugne-Setup-%s",
                 board_device_id());
    }
    return ESP_OK;
}

net_state_t net_state(void)
{
    return s_state;
}

bool net_ip(char *buf, size_t size)
{
    if (size) buf[0] = '\0';
    if (s_state != NET_STATE_CONNECTED) {
        return false;
    }
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK || ip.ip.addr == 0) {
        return false;
    }
    esp_ip4addr_ntoa(&ip.ip, buf, (int)size);
    return true;
}

esp_err_t net_scan(net_ap_t *out, size_t max, size_t *count)
{
    *count = 0;
    // Scanning needs the STA interface. In AP provisioning we run plain AP, so
    // switch to APSTA for the scan, then restore AP.
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    bool temp_apsta = (mode == WIFI_MODE_AP);
    if (temp_apsta) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    esp_err_t err = esp_wifi_scan_start(NULL, true);  // blocking, scan all channels
    uint16_t num = 0;
    wifi_ap_record_t *recs = NULL;
    if (err == ESP_OK) {
        esp_wifi_scan_get_ap_num(&num);
        if (num > 0) {
            recs = calloc(num, sizeof(wifi_ap_record_t));
            if (recs) {
                esp_wifi_scan_get_ap_records(&num, recs);  // sorted by RSSI, strongest first
            }
        }
    }
    if (temp_apsta) {
        esp_wifi_set_mode(WIFI_MODE_AP);  // restore the provisioning AP after reading results
    }
    if (err != ESP_OK) {
        return err;
    }
    if (num == 0 || !recs) {
        free(recs);
        return num == 0 ? ESP_OK : ESP_ERR_NO_MEM;
    }

    for (uint16_t i = 0; i < num && *count < max; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (ssid[0] == '\0') {
            continue;  // hidden network
        }
        bool dup = false;  // dedupe same SSID (multiple APs / bands)
        for (size_t j = 0; j < *count; j++) {
            if (strcmp(out[j].ssid, ssid) == 0) { dup = true; break; }
        }
        if (dup) {
            continue;
        }
        strlcpy(out[*count].ssid, ssid, sizeof(out[*count].ssid));
        out[*count].rssi = recs[i].rssi;
        out[*count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        (*count)++;
    }
    free(recs);
    return ESP_OK;
}
