// web_config: the configuration web page and its REST endpoints.
//
// Serves an embedded page (no CDN), reads and writes config.json through
// config_store, and accepts Wi-Fi credentials during provisioning. A 404
// handler redirects unknown paths to the portal root, which together with the
// DNS hijack in net drives the captive portal. Login and sessions are added in
// a later pass; in AP mode access is gated by the WPA2 AP password.
#include "web_config.h"
#include "config_store.h"
#include "board.h"
#include "net.h"
#include "logstore.h"
#include "ui.h"
#include "usage.h"
#include "source_sd.h"
#include "library.h"
#include "memo.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "mbedtls/base64.h"

static const char *TAG = "web_config";

#define WEB_MAX_BODY      32768  // bound on config.json uploads, must stay >= CONFIG_FILE_MAX
// Below this free space the SD card is "almost full": warn before/while caching
// episodes (a full podcast can be several hundred MB).
#define SD_LOW_FREE_BYTES (200ULL * 1024 * 1024)
#define WEB_MAX_WIFI_BODY 512    // bound on the small wifi credential post
#define SESSION_TOKEN_LEN 32     // hex chars
#define COOKIE_NAME       "bugne_session"

static httpd_handle_t s_server;
static char s_session[SESSION_TOKEN_LEN + 1];
static bool s_has_session;

// Cache of the last GitHub release check (web_config_gh_check), read by
// GET /api/ghota/status. RAM only, no NVS: a reboot just re-checks later.
static char s_gh_latest[32];
static bool s_gh_update;
static bool s_gh_checked;
static int64_t s_gh_check_time_us;

// The config page and login HTML live in www/*.html and are embedded via
// EMBED_FILES (see CMakeLists.txt). EMBED_FILES blobs are NOT null-terminated,
// so root_get sends them with an explicit length (end - start).
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t login_html_start[] asm("_binary_login_html_start");
extern const uint8_t login_html_end[]   asm("_binary_login_html_end");

// Generate a fresh random session token (hex).
static void new_session(void)
{
    uint8_t raw[SESSION_TOKEN_LEN / 2];
    esp_fill_random(raw, sizeof(raw));
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(raw); i++) {
        s_session[i * 2]     = hex[raw[i] >> 4];
        s_session[i * 2 + 1] = hex[raw[i] & 0x0F];
    }
    s_session[SESSION_TOKEN_LEN] = '\0';
    s_has_session = true;
}

static bool cookie_authed(httpd_req_t *req)
{
    if (!s_has_session) {
        return false;
    }
    char cookie[160];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) {
        return false;
    }
    char *t = strstr(cookie, COOKIE_NAME "=");
    if (!t) {
        return false;
    }
    t += strlen(COOKIE_NAME "=");
    return strncmp(t, s_session, SESSION_TOKEN_LEN) == 0;
}

// Stateless fallback for API clients (e.g. Home Assistant) that cannot keep a
// session cookie: HTTP Basic auth, checked on every request, no cookie set.
// The username is ignored; only the password after the first ':' matters.
static bool basic_authed(httpd_req_t *req)
{
    char hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    static const char prefix[] = "Basic ";
    size_t plen = strlen(prefix);
    if (strncmp(hdr, prefix, plen) != 0) {
        return false;
    }
    unsigned char creds[96];
    size_t out_len = 0;
    if (mbedtls_base64_decode(creds, sizeof(creds) - 1, &out_len,
                              (const unsigned char *)hdr + plen, strlen(hdr) - plen) != 0) {
        return false;
    }
    creds[out_len] = '\0';
    char *colon = strchr((char *)creds, ':');
    if (!colon) {
        return false;
    }
    return config_store_check_password(colon + 1) == ESP_OK;
}

// A request is authorized if no password is set yet (first-run setup), it
// carries the current session cookie, or it presents valid HTTP Basic auth.
static bool is_authed(httpd_req_t *req)
{
    if (!config_store_has_password()) {
        return true;
    }
    return cookie_authed(req) || basic_authed(req);
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t size, size_t *out_len)
{
    if (req->content_len >= size) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t off = 0;
    while (off < req->content_len) {
        int r = httpd_req_recv(req, buf + off, req->content_len - off);
        if (r <= 0) {
            return ESP_FAIL;
        }
        off += (size_t)r;
    }
    buf[off] = '\0';
    if (out_len) *out_len = off;
    return ESP_OK;
}

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    // EMBED_FILES blobs are NOT null-terminated: send an explicit length
    // (end - start), never HTTPD_RESP_USE_STRLEN.
    if (!is_authed(req)) {
        return httpd_resp_send(req, (const char *)login_html_start,
                               login_html_end - login_html_start);
    }
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

// Set the session cookie on the response. cookie_buf must outlive the send.
static void set_session_cookie(httpd_req_t *req, char *cookie_buf, size_t size)
{
    snprintf(cookie_buf, size, COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Strict", s_session);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie_buf);
}

static esp_err_t login_post(httpd_req_t *req)
{
    char body[WEB_MAX_WIFI_BODY];
    if (read_body(req, body, sizeof(body), NULL) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    const cJSON *pass = root ? cJSON_GetObjectItemCaseSensitive(root, "pass") : NULL;
    esp_err_t ok = (cJSON_IsString(pass) && pass->valuestring)
                   ? config_store_check_password(pass->valuestring) : ESP_FAIL;
    cJSON_Delete(root);
    if (ok != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "wrong password");
        return ESP_FAIL;
    }
    new_session();
    char cookie[96];
    set_session_cookie(req, cookie, sizeof(cookie));
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t password_post(httpd_req_t *req)
{
    // Changing an existing password requires an active session. Setting the
    // first password is open (first-run, behind the WPA2 AP).
    if (config_store_has_password() && !is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_FAIL;
    }
    char body[WEB_MAX_WIFI_BODY];
    if (read_body(req, body, sizeof(body), NULL) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    const cJSON *pass = root ? cJSON_GetObjectItemCaseSensitive(root, "pass") : NULL;
    const char *plain = (cJSON_IsString(pass) && pass->valuestring) ? pass->valuestring : NULL;
    if (plain && plain[0] == '\0') {
        // Empty password removes the password: the page opens without login.
        esp_err_t cerr = config_store_clear_password();
        cJSON_Delete(root);
        if (cerr != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "clear failed");
            return ESP_FAIL;
        }
        return httpd_resp_sendstr(req, "password removed");
    }
    bool valid = plain && strlen(plain) >= 4;
    esp_err_t err = valid ? config_store_set_password(plain) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(root);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password too short or save failed");
        return ESP_FAIL;
    }
    // Keep the caller logged in after setting the password.
    new_session();
    char cookie[96];
    set_session_cookie(req, cookie, sizeof(cookie));
    return httpd_resp_sendstr(req, "password set");
}

static esp_err_t scan_get(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_FAIL;
    }
    net_ap_t aps[24];
    size_t n = 0;
    if (net_scan(aps, sizeof(aps) / sizeof(aps[0]), &n) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
        return ESP_FAIL;
    }
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(o, "secure", aps[i].secure);
        cJSON_AddItemToArray(arr, o);
    }
    char *txt = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!txt) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, txt);
    cJSON_free(txt);
    return ESP_OK;
}

static esp_err_t config_get(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_FAIL;
    }
    char *buf = heap_caps_malloc(WEB_MAX_BODY, MALLOC_CAP_SPIRAM);  // keep it out of internal RAM
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    size_t len = 0;
    esp_err_t err = config_store_read_json(buf, WEB_MAX_BODY, &len);
    if (err != ESP_OK) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no config");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}

static esp_err_t logs_get(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_FAIL;
    }
    char *buf = malloc(LOGSTORE_SIZE + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    size_t len = 0;
    logstore_read(buf, LOGSTORE_SIZE + 1, &len);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}

// Listening statistics (C3). The device writes /littlefs/stats.json atomically
// (temp + rename) from the UI task, so reading it here from the httpd task is
// safe without a lock. Streamed in chunks: the file can reach a few tens of KB.
static esp_err_t stats_get(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    FILE *f = fopen("/littlefs/stats.json", "r");
    if (!f) {  // never written yet: empty history
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    char *buf = malloc(1024);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    bool any = false;
    size_t n;
    while ((n = fread(buf, 1, 1024, f)) > 0) {
        any = true;
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
    }
    if (!any) httpd_resp_send_chunk(req, "[]", 2);  // an empty file is still valid JSON
    httpd_resp_send_chunk(req, NULL, 0);            // terminate the chunked response
    free(buf);
    fclose(f);
    return ESP_OK;
}

// Clear the stats (parental control). The actual RAM + file clear runs on the
// UI task (sole owner of both); this just requests it.
static esp_err_t stats_reset_post(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_FAIL;
    }
    ui_stats_reset();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// Firmware update over Wi-Fi: stream the uploaded .bin into the inactive OTA
// partition, then boot it. The body is the raw firmware image.
static esp_err_t ota_post(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_FAIL;
    }
    // Stop playback before touching flash: esp_ota_begin/write disable the flash
    // cache, and a non-IRAM ISR from active SD/I2S playback firing in that window
    // crashes (#32). Request a stop and wait for it to take effect.
    ui_remote(UI_REMOTE_STOP, 0);
    for (int i = 0; i < 30; i++) {  // up to ~3s for the decode loop to unwind
        ui_status_t st;
        ui_status(&st);
        if (!st.active) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(200));  // let the worker finish tearing the source down
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }
    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(part, OTA_SIZE_UNKNOWN, &handle) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
        return ESP_FAIL;
    }
    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    int remaining = req->content_len;
    esp_err_t err = ESP_OK;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < 4096 ? remaining : 4096);
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(handle, buf, r);
        if (err != ESP_OK) {
            break;
        }
        remaining -= r;
    }
    free(buf);
    if (err != ESP_OK) {
        esp_ota_abort(handle);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ota write failed");
        return ESP_FAIL;
    }
    if (esp_ota_end(handle) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid firmware image");
        return ESP_FAIL;
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "update ok, rebooting");
    ESP_LOGI(TAG, "OTA written to %s, rebooting", part->label);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// Manual firmware update from the latest GitHub release. The stable
// /releases/latest/download/<asset> URL always redirects to the newest
// release's asset, so the device needs no GitHub API and no JSON parsing.
// Forks: change the repo here.
#define GH_OTA_URL "https://github.com/Tupile/bugne-releases/releases/latest/download/bugne.bin"

static esp_err_t gh_ota_begin(esp_https_ota_handle_t *out)
{
    esp_http_client_config_t http = {
        .url = GH_OTA_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,  // the signed asset redirect URL is long
    };
    esp_https_ota_config_t cfg = { .http_config = &http };
    return esp_https_ota_begin(&cfg, out);
}

// Check: read only the new image's app descriptor (first kilobytes), compare
// its embedded version with the running one, then abort the transfer. An
// INEQUALITY means "update available" (a downgrade release is deliberate).
// Returns ESP_ERR_NOT_FOUND when no release is reachable (no network, no
// release yet, or a TLS failure), ESP_FAIL when the header read itself
// failed, ESP_OK otherwise. On success also refreshes the device-side cache
// (s_gh_latest/s_gh_update/s_gh_checked/s_gh_check_time_us) used by
// GET /api/ghota/status, regardless of which caller triggered the check.
esp_err_t web_config_gh_check(char *latest, size_t cap, bool *update)
{
    const esp_app_desc_t *cur = esp_app_get_description();
    esp_https_ota_handle_t h = NULL;
    if (gh_ota_begin(&h) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_app_desc_t img;
    esp_err_t err = esp_https_ota_get_img_desc(h, &img);
    esp_https_ota_abort(h);
    if (err != ESP_OK) {
        return ESP_FAIL;
    }
    bool upd = strcmp(cur->version, img.version) != 0;
    if (latest && cap) {
        strlcpy(latest, img.version, cap);
    }
    if (update) {
        *update = upd;
    }
    strlcpy(s_gh_latest, img.version, sizeof(s_gh_latest));
    s_gh_update = upd;
    s_gh_checked = true;
    s_gh_check_time_us = esp_timer_get_time();
    return ESP_OK;
}

static esp_err_t gh_check_get(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_FAIL;
    }
    const esp_app_desc_t *cur = esp_app_get_description();
    char resp[176];
    httpd_resp_set_type(req, "application/json");
    char latest[32];
    bool update = false;
    esp_err_t err = web_config_gh_check(latest, sizeof(latest), &update);
    if (err == ESP_ERR_NOT_FOUND) {
        // No release yet, no network, or TLS failure: report cleanly.
        snprintf(resp, sizeof(resp), "{\"current\":\"%s\",\"error\":\"no release\"}",
                 cur->version);
        return httpd_resp_sendstr(req, resp);
    }
    if (err != ESP_OK) {
        snprintf(resp, sizeof(resp), "{\"current\":\"%s\",\"error\":\"check failed\"}",
                 cur->version);
        return httpd_resp_sendstr(req, resp);
    }
    snprintf(resp, sizeof(resp), "{\"current\":\"%s\",\"latest\":\"%s\",\"update\":%s}",
             cur->version, latest, update ? "true" : "false");
    return httpd_resp_sendstr(req, resp);
}

// Cached status for the web page badge: what the last check (manual or the
// daily auto-check from ui.c) found. age_s is -1 when no check has run yet
// since boot (RAM-only cache).
static esp_err_t ghota_status_get(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_FAIL;
    }
    const esp_app_desc_t *cur = esp_app_get_description();
    long long age_s = s_gh_checked ? (long long)((esp_timer_get_time() - s_gh_check_time_us) / 1000000) : -1;
    char resp[224];
    httpd_resp_set_type(req, "application/json");
    snprintf(resp, sizeof(resp),
             "{\"current\":\"%s\",\"latest\":\"%s\",\"update\":%s,\"age_s\":%lld}",
             cur->version, s_gh_checked ? s_gh_latest : "",
             s_gh_update ? "true" : "false", age_s);
    return httpd_resp_sendstr(req, resp);
}

// Install the latest GitHub release. Runs in this httpd task (internal stack,
// flash writes allowed) like the local upload above; the web page only calls
// it after an explicit confirmation, so no version re-check here (re-publishing
// a fixed asset under the same version stays installable).
static esp_err_t gh_ota_post(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_FAIL;
    }
    // Stop playback before flash writes, same reason as ota_post above.
    ui_remote(UI_REMOTE_STOP, 0);
    for (int i = 0; i < 30; i++) {
        ui_status_t st;
        ui_status(&st);
        if (!st.active) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_https_ota_handle_t h = NULL;
    if (gh_ota_begin(&h) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no release");
        return ESP_FAIL;
    }
    esp_err_t err;
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        // Streaming ~2 MB into the inactive slot; nothing else to do here.
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        esp_https_ota_abort(h);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "download failed");
        return ESP_FAIL;
    }
    if (esp_https_ota_finish(h) != ESP_OK) {  // validates the image, sets boot
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid firmware image");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "update ok, rebooting");
    ESP_LOGI(TAG, "GitHub OTA installed, rebooting");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t config_post(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_FAIL;
    }
    char *buf = heap_caps_malloc(WEB_MAX_BODY, MALLOC_CAP_SPIRAM);  // keep it out of internal RAM
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    esp_err_t err = read_body(req, buf, WEB_MAX_BODY, NULL);
    if (err == ESP_OK) {
        // Log who replaces the config: full-replace POSTs from a stale page
        // silently drop fields it does not know, so the source matters.
        struct sockaddr_in6 addr;
        socklen_t alen = sizeof(addr);
        char ips[46] = "?";
        if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &alen) == 0) {
            inet_ntop(AF_INET6, &addr.sin6_addr, ips, sizeof(ips));
        }
        ESP_LOGI(TAG, "config POST from %s, %u bytes, quiet %s, favorites %s, alarms %s, sunrise %s, daily_limit %s", ips,
                 (unsigned)strlen(buf), strstr(buf, "\"quiet\"") ? "present" : "MISSING",
                 strstr(buf, "\"favorites\"") ? "present" : "MISSING",
                 strstr(buf, "\"alarms\"") ? "present" : "MISSING",
                 strstr(buf, "\"sunrise\"") ? "present" : "MISSING",
                 strstr(buf, "\"daily_limit\"") ? "present" : "MISSING");
        err = config_store_write_json(buf);
    }
    free(buf);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid config");
        return ESP_FAIL;
    }
    return httpd_resp_sendstr(req, "config saved");
}

// GET /api/wifi: the saved networks' SSIDs only -- never the passwords (secrets).
// {"networks":["Home","Office", ...]}.
static esp_err_t wifi_get(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = root ? cJSON_AddArrayToObject(root, "networks") : NULL;
    if (!arr) { cJSON_Delete(root); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    for (int s = 0; s < CFG_WIFI_SLOTS; s++) {
        char ss[CFG_WIFI_SSID_MAX], pp[CFG_WIFI_PASS_MAX];
        if (config_store_get_wifi_slot(s, ss, sizeof(ss), pp, sizeof(pp)) == ESP_OK && ss[0])
            cJSON_AddItemToArray(arr, cJSON_CreateString(ss));
    }
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, txt ? txt : "{\"networks\":[]}");
    cJSON_free(txt);
    return ESP_OK;
}

// POST /api/wifi: {"networks":[{"ssid":"..","pass":".."}, ...]}. Stores up to
// CFG_WIFI_SLOTS networks and clears the rest. A blank pass keeps the password
// already stored for that SSID (the UI never receives passwords), so the user can
// reorder/keep networks without retyping. Reboots to apply.
static esp_err_t wifi_post(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_FAIL;
    }
    char *buf = malloc(4096);  // up to 16 networks; off the small httpd stack
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    esp_err_t rb = read_body(req, buf, 4096, NULL);
    cJSON *root = (rb == ESP_OK) ? cJSON_Parse(buf) : NULL;
    free(buf);
    cJSON *nets = root ? cJSON_GetObjectItemCaseSensitive(root, "networks") : NULL;
    if (!cJSON_IsArray(nets)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing networks");
        return ESP_FAIL;
    }

    // Snapshot the currently-stored SSID->password pairs so a blank password keeps
    // the existing one. Static to stay off the handler stack.
    static char ex_ssid[CFG_WIFI_SLOTS][CFG_WIFI_SSID_MAX];
    static char ex_pass[CFG_WIFI_SLOTS][CFG_WIFI_PASS_MAX];
    int ex_n = 0;
    for (int s = 0; s < CFG_WIFI_SLOTS; s++) {
        char ss[CFG_WIFI_SSID_MAX], pp[CFG_WIFI_PASS_MAX];
        if (config_store_get_wifi_slot(s, ss, sizeof(ss), pp, sizeof(pp)) == ESP_OK && ss[0]) {
            strlcpy(ex_ssid[ex_n], ss, CFG_WIFI_SSID_MAX);
            strlcpy(ex_pass[ex_n], pp, CFG_WIFI_PASS_MAX);
            ex_n++;
        }
    }

    esp_err_t err = ESP_OK;
    int slot = 0, count = cJSON_GetArraySize(nets);
    for (int i = 0; i < count && slot < CFG_WIFI_SLOTS && err == ESP_OK; i++) {
        const cJSON *n = cJSON_GetArrayItem(nets, i);
        const cJSON *js = cJSON_GetObjectItemCaseSensitive(n, "ssid");
        const cJSON *jp = cJSON_GetObjectItemCaseSensitive(n, "pass");
        if (!cJSON_IsString(js) || !js->valuestring || !js->valuestring[0]) continue;  // skip blank rows
        const char *ssid = js->valuestring;
        const char *pass = (cJSON_IsString(jp) && jp->valuestring) ? jp->valuestring : "";
        if (pass[0] == '\0') {
            for (int e = 0; e < ex_n; e++)
                if (strcmp(ex_ssid[e], ssid) == 0) { pass = ex_pass[e]; break; }
        }
        err = config_store_set_wifi_slot(slot++, ssid, pass);
    }
    for (; slot < CFG_WIFI_SLOTS && err == ESP_OK; slot++)
        err = config_store_set_wifi_slot(slot, "", "");  // clear unused slots
    cJSON_Delete(root);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "saved, rebooting");
    ESP_LOGI(TAG, "wifi networks saved, rebooting to apply");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// Redirect any unknown path to the portal root. With the DNS hijack this is what
// makes phones and laptops pop the captive portal.
static esp_err_t captive_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---- SD card browser / upload (#29) ----

#define WEB_SD_PATH_MAX 320
#define WEB_SD_BROWSE_MAX 256

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// In-place percent-decode (%XX). '+' is left as-is (encodeURIComponent never
// emits it, so a literal '+' in a name must stay a '+').
static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        int hi, lo;
        if (*s == '%' && (hi = hexv(s[1])) >= 0 && (lo = hexv(s[2])) >= 0) {
            *o++ = (char)((hi << 4) | lo);
            s += 3;
        } else {
            *o++ = *s++;
        }
    }
    *o = '\0';
}

// Extract and URL-decode a query parameter ("" if absent).
static void query_param(httpd_req_t *req, const char *key, char *out, size_t size)
{
    out[0] = '\0';
    char q[WEB_SD_PATH_MAX * 3];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, key, out, size) == ESP_OK) {
        url_decode(out);
    } else {
        out[0] = '\0';
    }
}

static void sd_query_path(httpd_req_t *req, char *out, size_t size)
{
    query_param(req, "path", out, size);
}

// GET /api/sd/list?path=<dir>: JSON {present,path,entries:[{name,dir,size}]}.
static esp_err_t sd_list_get(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_OK;
    }
    char path[WEB_SD_PATH_MAX];
    sd_query_path(req, path, sizeof(path));
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    cJSON_AddBoolToObject(o, "present", source_sd_present());
    cJSON_AddStringToObject(o, "path", path);
    uint64_t sd_total = 0, sd_free = 0;
    if (source_sd_usage(&sd_total, &sd_free)) {
        cJSON_AddNumberToObject(o, "total", (double)sd_total);
        cJSON_AddNumberToObject(o, "free", (double)sd_free);
    }
    cJSON *arr = cJSON_AddArrayToObject(o, "entries");
    if (source_sd_present() && arr) {
        source_sd_entry_t *ents = malloc(sizeof(source_sd_entry_t) * WEB_SD_BROWSE_MAX);
        if (ents) {
            size_t n = 0;
            if (source_sd_browse(path, ents, WEB_SD_BROWSE_MAX, &n, true) == ESP_OK) {
                for (size_t i = 0; i < n; i++) {
                    cJSON *e = cJSON_CreateObject();
                    if (!e) break;
                    cJSON_AddStringToObject(e, "name", ents[i].name);
                    cJSON_AddBoolToObject(e, "dir", ents[i].is_dir);
                    cJSON_AddNumberToObject(e, "size", ents[i].size);
                    cJSON_AddItemToArray(arr, e);
                }
            }
            free(ents);
        }
    }
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!txt) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, txt);
    cJSON_free(txt);
    return ESP_OK;
}

// POST /api/sd/upload?path=<rel/path.ext>: the body is the raw file, streamed to
// the SD card (parent directories created as needed).
static esp_err_t sd_upload_post(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_OK;
    }
    char path[WEB_SD_PATH_MAX];
    sd_query_path(req, path, sizeof(path));
    if (!path[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing path");
        return ESP_OK;
    }
    FILE *f = source_sd_create(path);
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot create file");
        return ESP_OK;
    }
    char *buf = malloc(4096);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    int remaining = req->content_len;
    bool ok = true;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < 4096 ? remaining : 4096);
        if (r <= 0 || fwrite(buf, 1, r, f) != (size_t)r) {
            ok = false;
            break;
        }
        remaining -= r;
    }
    free(buf);
    fclose(f);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "ok");
}

// POST /api/sd/mkdir?path=<rel/dir>: create a directory (and parents).
static esp_err_t sd_mkdir_post(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_OK;
    }
    char path[WEB_SD_PATH_MAX];
    sd_query_path(req, path, sizeof(path));
    if (!path[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing path");
        return ESP_OK;
    }
    if (source_sd_mkdir(path) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mkdir failed");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "ok");
}

// POST /api/sd/delete?path=<rel>: delete a file, or a folder and its contents.
static esp_err_t sd_delete_post(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_OK;
    }
    char path[WEB_SD_PATH_MAX];
    sd_query_path(req, path, sizeof(path));
    if (!path[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing path");
        return ESP_OK;
    }
    if (source_sd_delete(path) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "delete failed");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "ok");
}

// POST /api/memo?from=<name>: receive a voice memo from another Bugne on the
// LAN. Deliberately NOT behind is_authed (peers do not know this device's web
// password); bounded instead: ui.memo_rx kill switch, content length required
// and capped, stored-memo cap (refuse, never purge), WAV format check, sender
// name sanitized, storage path chosen here. See docs/config_schema.md.
static esp_err_t memo_post(httpd_req_t *req)
{
    if (config_store_get()->ui.memo_rx == 0) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "memo receiving disabled");
        return ESP_OK;
    }
    if (!source_sd_present()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "no sd card");
        return ESP_OK;
    }
    // A chunked request has content_len 0 and is rejected by the same check.
    if (req->content_len <= MEMO_WAV_HEADER_BYTES || req->content_len > MEMO_RX_MAX_BYTES) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "bad memo size");
        return ESP_OK;
    }

    char query[96] = "", raw_from[64] = "", talkie_v[4] = "", from[MEMO_SENDER_MAX];
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "from", raw_from, sizeof(raw_from));
    httpd_query_key_value(query, "talkie", talkie_v, sizeof(talkie_v));
    memo_sanitize_sender(from, sizeof(from), raw_from);
    // A talkie message is ephemeral (auto-played then deleted) only when this
    // device is on the talkie screen right now; otherwise it falls back to a
    // normal stored memo (answered 202, never lost to a leave-screen race).
    bool talkie = (talkie_v[0] == '1');
    bool ephemeral = talkie && ui_talkie_active();

    uint64_t free_b = 0;
    bool full = (!ephemeral && memo_count() >= MEMO_MAX_COUNT) ||
                !source_sd_usage(NULL, &free_b) || free_b < req->content_len + (4u << 20);
    if (full) {
        httpd_resp_set_status(req, "507 Insufficient Storage");
        httpd_resp_sendstr(req, "memo box full");
        return ESP_OK;
    }

    char final_abs[96], part_abs[104];
    FILE *f = ephemeral
            ? memo_tk_create(final_abs, sizeof(final_abs), part_abs, sizeof(part_abs))
            : memo_rx_create(from, final_abs, sizeof(final_abs), part_abs, sizeof(part_abs));
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot create file");
        return ESP_OK;
    }
    // PSRAM buffer: receives can coincide with auto-maintenance HTTPS, and the
    // bench cap-loop test drove internal RAM down to ~500 B free with heavier
    // internal allocations in flight. Data buffers are fine in PSRAM.
    char *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        remove(part_abs);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    // The first 512 bytes are stashed and format-checked as soon as they are
    // complete, so a non-WAV body is refused without writing megabytes first.
    uint8_t head[512];
    size_t head_len = 0, total = req->content_len, rcvd = 0;
    bool ok = true, head_ok = false, bad_format = false;
    while (rcvd < total) {
        size_t want = total - rcvd < 4096 ? total - rcvd : 4096;
        int r = httpd_req_recv(req, buf, want);
        if (r <= 0) { ok = false; break; }
        if (head_len < sizeof(head)) {
            size_t c = sizeof(head) - head_len;
            if (c > (size_t)r) c = (size_t)r;
            memcpy(head + head_len, buf, c);
            head_len += c;
        }
        rcvd += (size_t)r;
        if (!head_ok && (head_len >= sizeof(head) || rcvd >= total)) {
            uint32_t off, len;
            if (!memo_wav_parse(head, head_len, &off, &len)) {
                ok = false;
                bad_format = true;
                break;
            }
            head_ok = true;
        }
        if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) { ok = false; break; }
    }
    free(buf);
    fclose(f);
    if (ok && rename(part_abs, final_abs) != 0) ok = false;
    if (!ok) {
        remove(part_abs);
        if (bad_format) httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not a memo wav");
        else httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "receive failed");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "memo received from %s (%u bytes%s)", from, (unsigned)total,
             ephemeral ? ", talkie" : "");
    if (ephemeral) {
        ui_talkie_received(from, final_abs);
    } else {
        ui_memo_received(from);
        if (talkie) {  // talkie message but not in talkie mode: kept as a memo
            httpd_resp_set_status(req, "202 Accepted");
        }
    }
    return httpd_resp_sendstr(req, "ok");
}

// GET /api/playback: a JSON snapshot of what the device is playing.
static esp_err_t playback_get(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_OK;
    }
    ui_status_t st;
    ui_status(&st);
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    cJSON_AddBoolToObject(o, "active", st.active);
    cJSON_AddBoolToObject(o, "paused", st.paused);
    cJSON_AddNumberToObject(o, "volume", st.volume);
    cJSON_AddStringToObject(o, "source", st.source);
    cJSON_AddStringToObject(o, "title", st.title);
    cJSON_AddStringToObject(o, "artist", st.artist);
    cJSON_AddNumberToObject(o, "pos_ms", st.pos_ms);
    cJSON_AddNumberToObject(o, "dur_ms", st.dur_ms);
    cJSON_AddNumberToObject(o, "sleep_min", st.sleep_min);  // A2: 0=off, -1=end-of-track
    cJSON_AddBoolToObject(o, "seekable", st.seekable);
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!txt) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, txt);
    cJSON_free(txt);
    return ESP_OK;
}

// POST /api/playback: {"action":"toggle|stop|next|prev|volume|radio|path|sleep|seek","value":N|str}
static esp_err_t playback_post(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_OK;
    }
    char body[512];  // "path" carries a library path up to LIB_PATH_MAX chars
    if (read_body(req, body, sizeof(body), NULL) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    const cJSON *action = root ? cJSON_GetObjectItemCaseSensitive(root, "action") : NULL;
    const cJSON *value = root ? cJSON_GetObjectItemCaseSensitive(root, "value") : NULL;
    int v = cJSON_IsNumber(value) ? value->valueint : 0;
    bool ok = cJSON_IsString(action) && action->valuestring;
    if (ok) {
        const char *a = action->valuestring;
        if      (!strcmp(a, "toggle")) ui_remote(UI_REMOTE_TOGGLE, 0);
        else if (!strcmp(a, "stop"))   ui_remote(UI_REMOTE_STOP, 0);
        else if (!strcmp(a, "next"))   ui_remote(UI_REMOTE_NEXT, 0);
        else if (!strcmp(a, "prev"))   ui_remote(UI_REMOTE_PREV, 0);
        else if (!strcmp(a, "volume")) ui_remote(UI_REMOTE_VOLUME, v);
        // Seek to value ms; no-op unless the playing file is seekable (see
        // GET "seekable"). Clamped to the track duration on the UI task.
        else if (!strcmp(a, "seek"))   ui_remote(UI_REMOTE_SEEK, v);
        else if (!strcmp(a, "radio"))  ui_remote(UI_REMOTE_PLAY_RADIO, v);
        // A2 sleep timer: 0=off, -1=end-of-track, else minutes (1..180, clamped
        // on the UI task in ui_remote_apply).
        else if (!strcmp(a, "sleep"))  ui_remote(UI_REMOTE_SLEEP, v);
        else if (!strcmp(a, "path")) {
            size_t len = cJSON_IsString(value) ? strlen(value->valuestring) : 0;
            if (cJSON_IsString(value) && len > 0 && len < LIB_PATH_MAX &&
                value->valuestring[0] != '/' && !strstr(value->valuestring, "..")) {
                // Optional display title (the library tag title); falls back to
                // the file name inside ui_remote_play_path when absent.
                const cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "title");
                ui_remote_play_path(value->valuestring,
                                    cJSON_IsString(title) ? title->valuestring : NULL);
            } else {
                ok = false;
            }
        }
        else ok = false;
    }
    cJSON_Delete(root);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad action");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "ok");
}

// POST /api/podcasts/refresh: refresh every configured podcast feed. The refresh
// runs on the device's playback worker, so it is refused while audio is playing.
static esp_err_t podcasts_refresh_post(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_OK;
    }
    ui_status_t st;
    ui_status(&st);
    if (st.active) {
        return httpd_resp_sendstr(req, "Stop playback first, then refresh.");
    }
    ui_remote(UI_REMOTE_REFRESH_PODCASTS, 0);
    return httpd_resp_sendstr(req, "Refreshing all podcast feeds...");
}

// GET /api/podcasts/refresh: whether a refresh is still running, for polling.
static esp_err_t podcasts_refresh_get(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ui_podcast_refreshing()
                              ? "{\"refreshing\":true}" : "{\"refreshing\":false}");
}

// POST /api/podcasts/download {"id":N}: download all not-yet-cached episodes of
// podcast N to SD. Like refresh it runs on the playback worker, so it needs an
// SD card and is refused while audio plays.
static esp_err_t podcasts_download_post(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_OK;
    }
    if (!source_sd_present()) {
        return httpd_resp_sendstr(req, "Insert an SD card first.");
    }
    char body[64];
    if (read_body(req, body, sizeof(body), NULL) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    const cJSON *id = root ? cJSON_GetObjectItemCaseSensitive(root, "id") : NULL;
    const cJSON *force = root ? cJSON_GetObjectItemCaseSensitive(root, "force") : NULL;
    const cJSON *all = root ? cJSON_GetObjectItemCaseSensitive(root, "all") : NULL;
    bool f = cJSON_IsTrue(force);
    bool scope_all = cJSON_IsTrue(all);
    bool have_id = cJSON_IsNumber(id);
    int v = have_id ? id->valueint : 0;
    cJSON_Delete(root);
    if (!scope_all && !have_id) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad id");
        return ESP_OK;
    }
    // Schedule a background job (the engine runs it when the device is idle and
    // pauses it during playback); no need to stop audio first.
    if (scope_all) {
        ui_remote(f ? UI_REMOTE_REDOWNLOAD_ALL : UI_REMOTE_DOWNLOAD_ALL, 0);
    } else {
        ui_remote(f ? UI_REMOTE_REDOWNLOAD_PODCAST : UI_REMOTE_DOWNLOAD_PODCAST, v);
    }
    // Warn (but still proceed) when the card is almost full.
    uint64_t freeb = 0;
    char msg[112];
    if (source_sd_usage(NULL, &freeb) && freeb < SD_LOW_FREE_BYTES) {
        snprintf(msg, sizeof(msg), "Scheduled (runs when idle). SD almost full: %llu MB free.",
                 (unsigned long long)(freeb / (1024 * 1024)));
    } else {
        strlcpy(msg, "Scheduled: runs in the background when the device is idle.", sizeof(msg));
    }
    return httpd_resp_sendstr(req, msg);
}

// GET /api/podcasts/download: download progress, for polling.
static esp_err_t podcasts_download_get(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_OK;
    }
    ui_dl_status_t st;
    ui_download_status(&st);
    const char *phase = st.phase == UI_DL_SCHEDULED  ? "scheduled"
                      : st.phase == UI_DL_PAUSED      ? "paused"
                      : st.phase == UI_DL_REFRESHING  ? "refreshing"
                      : st.phase == UI_DL_DOWNLOADING ? "downloading"
                      : st.phase == UI_DL_SCANNING    ? "scanning"
                      : st.phase == UI_DL_SDFULL      ? "sdfull"
                      : "idle";
    char json[128];
    snprintf(json, sizeof(json),
             "{\"pending\":%s,\"active\":%s,\"done\":%d,\"total\":%d,\"phase\":\"%s\"}",
             st.pending ? "true" : "false", st.active ? "true" : "false",
             st.done, st.total, phase);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// POST /api/podcasts/download/cancel: stop a running download.
static esp_err_t podcasts_download_cancel_post(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_OK;
    }
    ui_remote(UI_REMOTE_CANCEL_DOWNLOAD, 0);
    return httpd_resp_sendstr(req, "Cancelling download...");
}

// POST /api/library/scan: (re)build the SD music index on a background task.
static esp_err_t library_scan_post(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_OK;
    }
    if (!library_scan_start()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy or no SD card");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "scan started");
}

// GET /api/library[?artist=A[&album=B]]: browse the index.
//   no params      -> {scanning,count,artists:[...]}
//   ?artist=A      -> {albums:[...]}
//   ?artist=A&album=B -> {tracks:[{title,path}]}
static esp_err_t library_get(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_OK;
    }
    char artist[LIB_NAME_MAX], album[LIB_NAME_MAX];
    query_param(req, "artist", artist, sizeof(artist));
    query_param(req, "album", album, sizeof(album));

    cJSON *o = cJSON_CreateObject();
    if (!o) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    cJSON_AddBoolToObject(o, "scanning", library_scanning());
    cJSON_AddNumberToObject(o, "count", library_track_count());

    if (artist[0] && album[0]) {
        char (*titles)[LIB_NAME_MAX] = heap_caps_malloc(128 * LIB_NAME_MAX, MALLOC_CAP_SPIRAM);
        char (*paths)[LIB_PATH_MAX] = heap_caps_malloc(128 * LIB_PATH_MAX, MALLOC_CAP_SPIRAM);
        cJSON *arr = cJSON_AddArrayToObject(o, "tracks");
        if (titles && paths && arr) {
            size_t n = library_album_tracks(artist, album, titles, paths, 128);
            for (size_t i = 0; i < n; i++) {
                cJSON *e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "title", titles[i]);
                cJSON_AddStringToObject(e, "path", paths[i]);
                cJSON_AddItemToArray(arr, e);
            }
        }
        free(titles);
        free(paths);
    } else {
        char (*names)[LIB_NAME_MAX] = heap_caps_malloc(128 * LIB_NAME_MAX, MALLOC_CAP_SPIRAM);
        const char *field = artist[0] ? "albums" : "artists";
        cJSON *arr = cJSON_AddArrayToObject(o, field);
        if (names && arr) {
            size_t n = artist[0] ? library_albums(artist, names, 128)
                                 : library_artists(names, 128);
            for (size_t i = 0; i < n; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
        }
        free(names);
    }

    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!txt) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, txt);
    cJSON_free(txt);
    return ESP_OK;
}

// GET /api/screenshot: the current on-device screen as a 16bpp BMP (BI_BITFIELDS,
// top-down). Permanent debug aid so the UI can be verified remotely on the bench
// (tools/screenshot.py fetches and converts it). Serialized by ui_screenshot.
static esp_err_t screenshot_get(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_OK;
    }
    const uint8_t *px = NULL;
    int w = 0, h = 0;
    esp_err_t err = ui_screenshot(&px, &w, &h, 3000);
    if (err != ESP_OK) {
        ui_screenshot_release();
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, err == ESP_ERR_TIMEOUT ? "screenshot timeout" : "screenshot failed");
        return ESP_OK;
    }
    uint32_t pixbytes = (uint32_t)w * 2 * (uint32_t)h;
    uint32_t filesize = 66 + pixbytes;   // 14 file + 40 info + 12 masks headers
    uint8_t hdr[66];
    memset(hdr, 0, sizeof(hdr));
    // BITMAPFILEHEADER
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = filesize; hdr[3] = filesize >> 8; hdr[4] = filesize >> 16; hdr[5] = filesize >> 24;
    hdr[10] = 66;                        // bfOffBits
    // BITMAPINFOHEADER
    hdr[14] = 40;                        // biSize
    hdr[18] = w; hdr[19] = w >> 8;       // biWidth
    { int32_t nh = -h; hdr[22] = nh; hdr[23] = nh >> 8; hdr[24] = nh >> 16; hdr[25] = nh >> 24; }  // biHeight < 0 = top-down
    hdr[26] = 1;                         // biPlanes
    hdr[28] = 16;                        // biBitCount
    hdr[30] = 3;                         // biCompression = BI_BITFIELDS
    hdr[34] = pixbytes; hdr[35] = pixbytes >> 8; hdr[36] = pixbytes >> 16; hdr[37] = pixbytes >> 24;  // biSizeImage
    // RGB565 channel masks
    hdr[54] = 0x00; hdr[55] = 0xF8;      // red   0x0000F800
    hdr[58] = 0xE0; hdr[59] = 0x07;      // green 0x000007E0
    hdr[62] = 0x1F;                      // blue  0x0000001F

    httpd_resp_set_type(req, "image/bmp");
    esp_err_t se = httpd_resp_send_chunk(req, (const char *)hdr, sizeof(hdr));
    if (se == ESP_OK) se = httpd_resp_send_chunk(req, (const char *)px, pixbytes);
    if (se == ESP_OK) se = httpd_resp_send_chunk(req, NULL, 0);  // terminate
    ui_screenshot_release();
    return se == ESP_OK ? ESP_OK : ESP_FAIL;
}

// POST /api/debug/nav {"screen":"<name>"}: navigate the on-device UI to a named
// screen. Permanent debug aid for verifying screens remotely. 400 on an unknown
// name (ui_remote_nav validates against its screen table).
static esp_err_t debug_nav_post(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_OK;
    }
    char body[128];
    if (read_body(req, body, sizeof(body), NULL) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    const cJSON *screen = root ? cJSON_GetObjectItemCaseSensitive(root, "screen") : NULL;
    bool ok = cJSON_IsString(screen) && screen->valuestring && ui_remote_nav(screen->valuestring);
    cJSON_Delete(root);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown screen");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// GET /api/status: a compact device snapshot for external integrations (Home
// Assistant discovers the device via the _bugne._tcp mDNS service, then polls
// this endpoint).
static esp_err_t status_get(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_OK;
    }
    const config_t *c = config_store_get();
    wifi_ap_record_t ap;
    int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
    char ip[16];
    if (!net_ip(ip, sizeof(ip))) {
        ip[0] = '\0';
    }
    bool sd_present = source_sd_present();
    uint64_t sd_total = 0, sd_free = 0;
    if (sd_present) {
        source_sd_usage(&sd_total, &sd_free);
    }

    cJSON *o = cJSON_CreateObject();
    if (!o) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    cJSON_AddStringToObject(o, "id", board_device_id());
    cJSON_AddStringToObject(o, "name", (c && c->device_name[0]) ? c->device_name : "");
    cJSON_AddStringToObject(o, "version", esp_app_get_description()->version);
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(o, "heap_free", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddStringToObject(o, "reset_reason", board_reset_reason_name());
    cJSON_AddNumberToObject(o, "rssi", rssi);
    cJSON_AddStringToObject(o, "ip", ip);
    // Daily usage consumed today (parental limit counter, listening + game
    // seconds). 0 before the first SNTP sync or on a fresh day. The counter's
    // ints are written from the LVGL task; aligned word reads are atomic on
    // this target, same tolerance as the stats file read.
    {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        int today = (tmv.tm_year + 1900 >= 2025)
                  ? (tmv.tm_year + 1900) * 10000 + (tmv.tm_mon + 1) * 100 + tmv.tm_mday
                  : 0;
        cJSON_AddNumberToObject(o, "usage_today_s", usage_today(today));
    }
    cJSON *sd = cJSON_AddObjectToObject(o, "sd");
    if (sd) {
        cJSON_AddBoolToObject(sd, "present", sd_present);
        cJSON_AddNumberToObject(sd, "total", (double)sd_total);
        cJSON_AddNumberToObject(sd, "free", (double)sd_free);
    }
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!txt) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, txt);
    cJSON_free(txt);
    return ESP_OK;
}

// POST /api/reboot: stop playback (same reason as ota_post: an ISR firing
// during a flash-cache-disabling window can crash, and a clean stop is just
// good manners anyway), reply, then restart.
static esp_err_t reboot_post(httpd_req_t *req)
{
    if (!is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_OK;
    }
    ui_remote(UI_REMOTE_STOP, 0);
    for (int i = 0; i < 30; i++) {
        ui_status_t st;
        ui_status(&st);
        if (!st.active) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    httpd_resp_sendstr(req, "ok");
    ESP_LOGI(TAG, "reboot requested via /api/reboot");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

esp_err_t web_config_start(void)
{
    if (s_server) {
        return ESP_OK; // already running
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.stack_size = 12288;    // the GitHub OTA handlers run a TLS handshake in
                               // this task; the 4 KB default overflows (same
                               // class of bug as the Sendspin httpd fix). Stays
                               // internal RAM: these handlers write flash.
    cfg.max_open_sockets = 3;  // keep lwip sockets free for streaming + Sendspin
    cfg.max_uri_handlers = 36; // default 8 is below our route count (registration
                               // past the limit fails silently, e.g. /api/playback)
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd start failed");

    const httpd_uri_t routes[] = {
        {.uri = "/",             .method = HTTP_GET,  .handler = root_get},
        {.uri = "/login",        .method = HTTP_POST, .handler = login_post},
        {.uri = "/api/scan",     .method = HTTP_GET,  .handler = scan_get},
        {.uri = "/api/config",   .method = HTTP_GET,  .handler = config_get},
        {.uri = "/api/config",   .method = HTTP_POST, .handler = config_post},
        {.uri = "/api/logs",     .method = HTTP_GET,  .handler = logs_get},
        {.uri = "/api/ota",      .method = HTTP_POST, .handler = ota_post},
        {.uri = "/api/ghota/check", .method = HTTP_GET,  .handler = gh_check_get},
        {.uri = "/api/ghota/status", .method = HTTP_GET,  .handler = ghota_status_get},
        {.uri = "/api/ghota",    .method = HTTP_POST, .handler = gh_ota_post},
        {.uri = "/api/wifi",     .method = HTTP_GET,  .handler = wifi_get},
        {.uri = "/api/wifi",     .method = HTTP_POST, .handler = wifi_post},
        {.uri = "/api/password", .method = HTTP_POST, .handler = password_post},
        {.uri = "/api/playback", .method = HTTP_GET,  .handler = playback_get},
        {.uri = "/api/playback", .method = HTTP_POST, .handler = playback_post},
        {.uri = "/api/sd/list",   .method = HTTP_GET,  .handler = sd_list_get},
        {.uri = "/api/sd/upload", .method = HTTP_POST, .handler = sd_upload_post},
        {.uri = "/api/sd/mkdir",  .method = HTTP_POST, .handler = sd_mkdir_post},
        {.uri = "/api/sd/delete", .method = HTTP_POST, .handler = sd_delete_post},
        {.uri = "/api/memo",      .method = HTTP_POST, .handler = memo_post},
        {.uri = "/api/library",      .method = HTTP_GET,  .handler = library_get},
        {.uri = "/api/library/scan", .method = HTTP_POST, .handler = library_scan_post},
        {.uri = "/api/podcasts/refresh", .method = HTTP_POST, .handler = podcasts_refresh_post},
        {.uri = "/api/podcasts/refresh", .method = HTTP_GET,  .handler = podcasts_refresh_get},
        {.uri = "/api/podcasts/download", .method = HTTP_POST, .handler = podcasts_download_post},
        {.uri = "/api/podcasts/download", .method = HTTP_GET,  .handler = podcasts_download_get},
        {.uri = "/api/podcasts/download/cancel", .method = HTTP_POST, .handler = podcasts_download_cancel_post},
        {.uri = "/api/screenshot", .method = HTTP_GET,  .handler = screenshot_get},
        {.uri = "/api/debug/nav",  .method = HTTP_POST, .handler = debug_nav_post},
        {.uri = "/api/stats",       .method = HTTP_GET,  .handler = stats_get},
        {.uri = "/api/stats/reset", .method = HTTP_POST, .handler = stats_reset_post},
        {.uri = "/api/status",      .method = HTTP_GET,  .handler = status_get},
        {.uri = "/api/reboot",      .method = HTTP_POST, .handler = reboot_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, captive_redirect);

    ESP_LOGI(TAG, "config web server up (device %s)", board_device_id());
    return ESP_OK;
}
