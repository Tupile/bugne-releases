// net: home-grown Wi-Fi manager and state machine.
//
// With stored credentials it connects as a station; otherwise it brings up the
// setup AP (Bugne-Setup-XXXX, WPA2, per-device MAC-derived password) for
// provisioning. mDNS (bugne-xxxx.local) and the config web server come up once
// connected. The captive portal (DNS hijack and HTTP captive routes) is added
// with web_config. SD playback is unaffected by the network state, so the
// device is never stuck waiting on provisioning.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_STATE_BOOT = 0,
    NET_STATE_CONNECTING,    // trying stored credentials
    NET_STATE_CONNECTED,     // station with an IP
    NET_STATE_PROVISIONING,  // setup AP up, waiting for credentials
} net_state_t;

// One scanned access point.
typedef struct {
    char    ssid[33];
    int8_t  rssi;
    bool    secure;  // true if not an open network
} net_ap_t;

// Start the Wi-Fi state machine. Returns once the initial decision is made
// (connected as station, or AP provisioning started).
esp_err_t net_start(void);

// Current network state.
net_state_t net_state(void);

// Copy the current station IPv4 address ("192.0.2.42") into buf. Returns false
// (and sets buf to "") if not connected or no IP yet.
bool net_ip(char *buf, size_t size);

// Scan for nearby access points (blocking, ~1-2s). Fills out (up to max,
// deduplicated by SSID, strongest first) and sets *count. Works during AP
// provisioning. Returns an esp_err_t from the scan.
esp_err_t net_scan(net_ap_t *out, size_t max, size_t *count);

// Another Bugne discovered on the LAN.
typedef struct {
    char     name[32];  // TXT "name", or "Bugne <id>" when unset
    char     ip[16];
    uint16_t port;
} net_peer_t;

// Browse mDNS for other Bugnes (_bugne._tcp), excluding this device
// (blocking, ~2.5s: call from a worker task, never from the LVGL task).
// Fills out (up to max) and returns the count; 0 when none or not connected.
int net_memo_peers(net_peer_t *out, int max);

#ifdef __cplusplus
}
#endif
