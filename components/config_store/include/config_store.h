// config_store: configuration in internal flash, always present.
//
// NVS holds Wi-Fi credentials and the hashed config-page password (added when
// the net component needs them). LittleFS holds config.json (see
// docs/config_schema.md). Works without an SD card.
//
// After config_store_init() returns ESP_OK, config_store_get() yields a valid,
// read-only, in-memory view of the configuration. The struct is owned by this
// component and stays valid for the program lifetime.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "podcast.h"  // PODCAST_URL_MAX, for config_resume_t.episode_url

#ifdef __cplusplus
extern "C" {
#endif

// Schema version this firmware understands. See docs/config_schema.md.
#define CFG_SCHEMA_VERSION 1

// Bounds. All strings are stored null-terminated within these sizes.
#define CFG_DEVICE_NAME_MAX   32
#define CFG_RADIO_NAME_MAX    48
#define CFG_PODCAST_TITLE_MAX 64
#define CFG_URL_MAX           256
#define CFG_MAX_WEBRADIOS     32
#define CFG_MAX_PODCASTS      50

typedef struct {
    int  id;
    char name[CFG_RADIO_NAME_MAX];
    char url[CFG_URL_MAX];
    int  skip_preroll;  // 1: open a decoy connection first to absorb a server pre-roll ad
} config_webradio_t;

typedef struct {
    int  id;
    char title[CFG_PODCAST_TITLE_MAX];
    char rss_url[CFG_URL_MAX];
    int  skip_seconds;  // intro/ads to skip: trimmed off at download, skipped at stream playback
} config_podcast_t;

// One alarm: time + weekday mask + source. See docs/config_schema.md. Up to
// CFG_MAX_ALARMS are stored (e.g. weekday / weekend / free use); only one can
// ring at a time (the alarm engine picks the lowest index on a tie).
#define CFG_ALARM_SD_TITLE_MAX 64
#define CFG_MAX_ALARMS 3
typedef struct {
    int  enabled;               // 0/1
    int  hour, minute;          // 0..23, 0..59
    int  days;                  // bit0=Monday .. bit6=Sunday; never 0 after load
    int  source;                // 0 = web radio, 1 = SD track
    int  radio_id;              // config_webradio_t.id (stable), NOT the array index
    char sd_path[CFG_URL_MAX];  // relative to the SD root, chosen on the web page
    char sd_title[CFG_ALARM_SD_TITLE_MAX]; // display title of the SD track
    int  volume;                // alarm ramp target, 5..100 (runtime-clamped by ui.volume_max)
    int  sunrise;               // sunrise light: backlight ramp minutes before fire time,
                                // 0 = off, else 1..15. Default 5.
} config_alarm_t;

// One parental no-playback window: time range + weekday mask. Web-only, the
// device never writes it. See docs/config_schema.md.
#define CFG_QUIET_WINDOWS 2

typedef struct {
    int enabled;        // 0/1
    int start_hour;     // 0..23
    int start_minute;   // 0..59
    int end_hour;       // 0..23
    int end_minute;     // 0..59
    int days;           // bit0=Monday .. bit6=Sunday; never 0 after load
} config_quiet_window_t;

// Parental daily usage limit: maximum listening + game minutes per day.
// Web-only, the device never writes it. The consumed-time counter itself is
// NVS (config_store_get/set_usage below), not config.json. See
// docs/config_schema.md.
typedef struct {
    int enabled;  // 0/1
    int minutes;  // 5..720
} config_limit_t;

// One favorite: a webradio (by stable id) or an SD track path. Added from the
// device's now-playing star button, managed on the web Play tab.
#define CFG_MAX_FAVORITES 12
#define CFG_FAV_PATH_MAX  256
#define CFG_FAV_TITLE_MAX 64

typedef struct {
    int  type;                     // 0 = webradio (by stable id), 1 = SD path
    int  radio_id;                 // config_webradio_t.id (type 0), NOT the array index
    char path[CFG_FAV_PATH_MAX];   // SD path relative to the SD root (type 1)
    char title[CFG_FAV_TITLE_MAX]; // display title
} config_favorite_t;

#define CFG_LANG_MAX 6
#define CFG_TZ_MAX   48
typedef struct {
    int  volume;               // 0..100
    int  volume_max;           // volume ceiling 1..100 (child-ear protection); default 100
    int  screen_sleep_seconds; // >= 0
    char lang[CFG_LANG_MAX];   // UI language ISO code ("en", "fr"); default "en"
    int  orientation;          // 0 = portrait (default), 1 = landscape
    int  dark;                 // 0 = light, 1 = dark (default)
    int  accent;               // button/accent color 0..4, see docs/config_schema.md
    int  game;                 // times-tables game on the home screen: 1 = shown (default)
    int  tuner;                // instrument tuner on the home screen: 1 = shown (default)
    char tz[CFG_TZ_MAX];       // POSIX TZ string; default Paris ("CET-1CEST,M3.5.0,M10.5.0/3")
} config_ui_t;

typedef struct {
    int  schema_version;
    char device_name[CFG_DEVICE_NAME_MAX];

    config_webradio_t webradios[CFG_MAX_WEBRADIOS];
    size_t            webradio_count;

    config_podcast_t podcasts[CFG_MAX_PODCASTS];
    size_t           podcast_count;

    config_ui_t ui;

    config_alarm_t alarms[CFG_MAX_ALARMS];

    config_quiet_window_t quiet[CFG_QUIET_WINDOWS];  // parental no-playback windows, web-configured

    config_limit_t daily_limit;  // parental daily usage limit, web-configured

    config_favorite_t favorites[CFG_MAX_FAVORITES];
    size_t            favorite_count;
} config_t;

// Init NVS, mount LittleFS, then load and validate config.json. A missing or
// unparseable file self-heals to defaults written back to flash. The in-memory
// config is always valid on ESP_OK.
esp_err_t config_store_init(void);

// Read-only view of the loaded configuration. Valid only after a successful
// config_store_init(). Never NULL after that.
const config_t *config_store_get(void);

// Set the UI language ISO code and persist it (used by the on-device selector;
// the web config sets it through the full-config save). Returns ESP_OK on success.
esp_err_t config_store_set_lang(const char *code);

// Set the display orientation (0 = portrait, 1 = landscape) and persist it.
// Used by the on-device toggle in Settings; the web config sets it through the
// full-config save. The ui component applies the change, this only stores it.
esp_err_t config_store_set_orientation(int orientation);

// Set the theme (dark 0/1 and accent color 0..4, see docs/config_schema.md)
// and persist it. Used by the on-device theme picker; the web config sets it
// through the full-config save. The ui component applies the change, this
// only stores it.
esp_err_t config_store_set_theme(int dark, int accent);

// Set one alarm (0..CFG_MAX_ALARMS-1) and persist it. Used by the on-device
// alarm settings screen; the web config sets it through the full-config save.
// Same clamping as config.json parsing: hour 0..23, minute 0..59, days
// bitmask coerced to a non-zero value (0 -> all days), source 0..1, volume
// 5..100. Returns ESP_ERR_INVALID_ARG on an out-of-range index. The alarm
// engine reads config_store_get()->alarms[i] each check.
esp_err_t config_store_set_alarm(int idx, const config_alarm_t *a);

// Append a favorite / remove the favorite at index, and persist (same
// precedent as config_store_set_theme: the device star button writes through
// these, the web page through the full-config save). add returns
// ESP_ERR_NO_MEM when the list is full or ESP_ERR_INVALID_ARG on a bad entry;
// remove returns ESP_ERR_INVALID_ARG on an out-of-range index.
esp_err_t config_store_favorite_add(const config_favorite_t *f);
esp_err_t config_store_favorite_remove(int index);

// Wi-Fi credentials live in NVS, not in config.json (they are secrets).
#define CFG_WIFI_SSID_MAX 33  // 32 chars + null
#define CFG_WIFI_PASS_MAX 64  // 63 chars + null

// Read stored Wi-Fi credentials. Returns ESP_ERR_NVS_NOT_FOUND if none are set.
esp_err_t config_store_get_wifi(char *ssid, size_t ssid_size, char *pass, size_t pass_size);

// Store Wi-Fi credentials, replacing any previous ones.
esp_err_t config_store_set_wifi(const char *ssid, const char *pass);

// Slotted Wi-Fi credentials: slot 0 is the primary network (same storage as the
// functions above), the rest are optional extra networks. The net layer picks the
// strongest visible one at connect time and roams between them. An empty SSID in a
// slot means "no network configured there". The web UI lets the user add as many
// as this cap. NVS keys are backward compatible: slot 0 = wifi_ssid/wifi_pass,
// slot N>0 = wifi_ssid<N+1>/wifi_pass<N+1> (so the old 3-slot layout is preserved).
#define CFG_WIFI_SLOTS 16
esp_err_t config_store_get_wifi_slot(int slot, char *ssid, size_t ssid_size,
                                     char *pass, size_t pass_size);
esp_err_t config_store_set_wifi_slot(int slot, const char *ssid, const char *pass);

// Background podcast download job, persisted in NVS so it survives a reboot and
// resumes on its own. The download engine lives in the ui component; this is just
// its durable state.
typedef struct {
    bool active;         // a job is pending / in progress
    bool scope_all;      // true: every podcast; false: just pod_id
    bool force;          // true: re-download all; false: only missing (refresh first)
    bool refresh_done;   // the feed-refresh step has completed for this job
    bool maint;          // auto-maintenance job: also rescan the music library at the end
    bool downloads_done; // refresh+download finished (resume goes straight to the scan)
    int  pod_id;         // target podcast id when !scope_all
    int  cur_pod_id;     // resume cursor: podcast id being processed (0 = from start)
    int  cur_ep_idx;     // resume cursor: next episode index within that podcast
} config_dljob_t;

// Read / write the persisted download job. get returns ESP_ERR_NVS_NOT_FOUND when
// none is stored (treat as "no job"). set with active=false effectively clears it.
esp_err_t config_store_get_dljob(config_dljob_t *job);
esp_err_t config_store_set_dljob(const config_dljob_t *job);

// Podcast playback resume point (the last interrupted episode only), persisted
// in NVS so it survives a reboot. There is no episode GUID in the manifest, so
// episode_url is the stable match key. pos_ms is on the intro-inclusive
// timeline; cached_trimmed_mp3 records whether it was saved while playing a
// physically-trimmed cached MP3 (see podcast_download_episode), needed to
// reconcile against skip_seconds if the episode's cache state changes before
// it is resumed. Owned by the ui component.
typedef struct {
    bool     active;             // a resume point is stored
    int      podcast_id;         // owning podcast, matches config_podcast_t.id
    uint32_t pos_ms;             // saved position, intro-inclusive timeline
    bool     cached_trimmed_mp3; // true if pos_ms was measured on a trimmed cached MP3
    char     episode_url[PODCAST_URL_MAX]; // stable match key across reboots/refreshes
} config_resume_t;

// Read / write the persisted resume point. get returns ESP_ERR_NVS_NOT_FOUND
// when none is stored (treat as "no resume point"). set with active=false
// clears it.
esp_err_t config_store_get_resume(config_resume_t *r);
esp_err_t config_store_set_resume(const config_resume_t *r);

// Multiplication game high score, persisted in NVS. get returns 0 when unset.
uint32_t config_store_get_highscore(void);
esp_err_t config_store_set_highscore(uint32_t score);

// Multiplication game max streak, persisted in NVS. get returns 0 when unset.
uint32_t config_store_get_maxstreak(void);
esp_err_t config_store_set_maxstreak(uint32_t streak);

// Daily usage counter for the parental limit, persisted in NVS so a power
// cycle cannot reset the child's consumed time. date is the local yyyymmdd
// day, seconds the usage consumed on it. get returns date 0 when never
// written. Owned by the ui component (it seeds usage.c at boot and writes
// back about once per minute of usage).
esp_err_t config_store_get_usage(uint32_t *date, uint32_t *seconds);
esp_err_t config_store_set_usage(uint32_t date, uint32_t seconds);

// Read the current config.json bytes into buf (null-terminated). Sets *out_len
// (excluding the null) if out_len is not NULL.
esp_err_t config_store_read_json(char *buf, size_t size, size_t *out_len);

// Validate JSON text against schema v1, then persist and apply it as the new
// configuration. Returns ESP_ERR_INVALID_ARG on parse or schema failure.
esp_err_t config_store_write_json(const char *json);

// Config-page password. Stored in NVS as a random salt plus the salted SHA-256
// hash of the password. The plaintext is never stored.
bool config_store_has_password(void);
esp_err_t config_store_set_password(const char *plain);
// Remove the password: the config page opens without login again.
esp_err_t config_store_clear_password(void);
// ESP_OK if it matches, ESP_ERR_NOT_FOUND if no password is set, ESP_FAIL on
// mismatch.
esp_err_t config_store_check_password(const char *plain);

#ifdef __cplusplus
}
#endif
