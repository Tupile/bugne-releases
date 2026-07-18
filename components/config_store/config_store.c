// config_store: NVS + LittleFS, plus loading and validating config.json.
#include "config_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_littlefs.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"

static const char *TAG = "config_store";

#define LFS_BASE_PATH    "/littlefs"
#define LFS_PARTITION    "littlefs"
#define CONFIG_PATH      LFS_BASE_PATH "/config.json"
#define CONFIG_TMP_PATH  LFS_BASE_PATH "/config.tmp"
#define CONFIG_FILE_MAX  32768  // hard cap on config.json size

// The in-memory config is ~31 KB (dominated by the 50 podcast slots): it is
// allocated from PSRAM at the top of config_store_init instead of sitting in
// internal .bss (the scarce resource). The alias macro keeps every existing
// s_config.x / &s_config call site unchanged. Nothing touches the config
// before config_store_init (it is the first init in app_main, and fatal).
static config_t *s_cfg;
#define s_config (*s_cfg)
static bool s_ready;

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void set_defaults(config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->schema_version = CFG_SCHEMA_VERSION;
    c->device_name[0] = '\0';  // empty -> consumers fall back to "Bugne <id>"
    c->webradio_count = 0;
    c->podcast_count = 0;
    c->ui.volume = 60;
    c->ui.volume_max = 100;
    c->ui.screen_sleep_seconds = 30;
    strlcpy(c->ui.lang, "en", sizeof(c->ui.lang));
    c->ui.orientation = 0;
    c->ui.dark = 1;    // dark: matches the look Bugne always had
    c->ui.accent = 0;  // blue
    c->ui.game = 1;    // times-tables game shown on home
    c->ui.tuner = 1;   // instrument tuner shown on home
    c->ui.memo_rx = 1; // accept voice memos from other Bugnes
    strlcpy(c->ui.tz, "CET-1CEST,M3.5.0,M10.5.0/3", sizeof(c->ui.tz));  // Paris

    for (int i = 0; i < CFG_MAX_ALARMS; i++) {
        c->alarms[i].enabled = 0;
        c->alarms[i].hour = 7;
        c->alarms[i].minute = 0;
        c->alarms[i].days = 0x7F;  // every day
        c->alarms[i].source = 0;   // web radio
        c->alarms[i].radio_id = 0;
        c->alarms[i].sd_path[0] = '\0';
        c->alarms[i].sd_title[0] = '\0';
        c->alarms[i].volume = 50;
        c->alarms[i].sunrise = 5;  // sunrise light on by default (only acts while asleep)
    }

    // Quiet hours: prefills only, both windows disabled by default.
    c->quiet[0].enabled = 0;
    c->quiet[0].start_hour = 19;
    c->quiet[0].start_minute = 0;
    c->quiet[0].end_hour = 7;
    c->quiet[0].end_minute = 0;
    c->quiet[0].days = 0x7F;  // every day
    c->quiet[1].enabled = 0;
    c->quiet[1].start_hour = 13;
    c->quiet[1].start_minute = 0;
    c->quiet[1].end_hour = 14;
    c->quiet[1].end_minute = 0;
    c->quiet[1].days = 0x7F;  // every day

    // Daily usage limit: prefill only, disabled by default.
    c->daily_limit.enabled = 0;
    c->daily_limit.minutes = 120;
}

// Overlay one alarm object's fields onto an already-defaulted config_alarm_t.
// Shared by the legacy single "alarm" object and each entry of the "alarms"
// array, so both parse and clamp identically.
static void parse_one_alarm(const cJSON *alarm, config_alarm_t *a)
{
    const cJSON *en = cJSON_GetObjectItemCaseSensitive(alarm, "enabled");
    if (cJSON_IsNumber(en)) a->enabled = clampi(en->valueint, 0, 1);
    const cJSON *hr = cJSON_GetObjectItemCaseSensitive(alarm, "hour");
    if (cJSON_IsNumber(hr)) a->hour = clampi(hr->valueint, 0, 23);
    const cJSON *mn = cJSON_GetObjectItemCaseSensitive(alarm, "minute");
    if (cJSON_IsNumber(mn)) a->minute = clampi(mn->valueint, 0, 59);
    const cJSON *dy = cJSON_GetObjectItemCaseSensitive(alarm, "days");
    if (cJSON_IsNumber(dy)) {
        int d = dy->valueint & 0x7F;
        a->days = d ? d : 0x7F;  // 0 or missing -> every day
    }
    const cJSON *src = cJSON_GetObjectItemCaseSensitive(alarm, "source");
    if (cJSON_IsNumber(src)) a->source = clampi(src->valueint, 0, 1);
    const cJSON *rid = cJSON_GetObjectItemCaseSensitive(alarm, "radio_id");
    if (cJSON_IsNumber(rid)) a->radio_id = rid->valueint;
    const cJSON *sp = cJSON_GetObjectItemCaseSensitive(alarm, "sd_path");
    if (cJSON_IsString(sp) && sp->valuestring)
        strlcpy(a->sd_path, sp->valuestring, sizeof(a->sd_path));
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(alarm, "sd_title");
    if (cJSON_IsString(st) && st->valuestring)
        strlcpy(a->sd_title, st->valuestring, sizeof(a->sd_title));
    const cJSON *vol = cJSON_GetObjectItemCaseSensitive(alarm, "volume");
    if (cJSON_IsNumber(vol)) a->volume = clampi(vol->valueint, 5, 100);
    const cJSON *sr = cJSON_GetObjectItemCaseSensitive(alarm, "sunrise");
    if (cJSON_IsNumber(sr)) a->sunrise = clampi(sr->valueint, 0, 15);
}

// Overlay parsed JSON onto the already-defaulted s_config. Missing fields keep
// their defaults. All strings are bounded. An entry without its required URL is
// skipped. Treat every value as untrusted.
static void load_from_json(const cJSON *root)
{
    const cJSON *dev = cJSON_GetObjectItemCaseSensitive(root, "device");
    if (cJSON_IsObject(dev)) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(dev, "name");
        if (cJSON_IsString(name) && name->valuestring) {
            strlcpy(s_config.device_name, name->valuestring, sizeof(s_config.device_name));
        }
    }

    const cJSON *radios = cJSON_GetObjectItemCaseSensitive(root, "webradios");
    if (cJSON_IsArray(radios)) {
        int n = cJSON_GetArraySize(radios);
        for (int i = 0; i < n && s_config.webradio_count < CFG_MAX_WEBRADIOS; i++) {
            const cJSON *it = cJSON_GetArrayItem(radios, i);
            if (!cJSON_IsObject(it)) continue;
            const cJSON *url = cJSON_GetObjectItemCaseSensitive(it, "url");
            if (!cJSON_IsString(url) || !url->valuestring || url->valuestring[0] == '\0') continue;
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
            const cJSON *nm = cJSON_GetObjectItemCaseSensitive(it, "name");
            config_webradio_t *w = &s_config.webradios[s_config.webradio_count++];
            w->id = cJSON_IsNumber(id) ? id->valueint : (int)s_config.webradio_count;
            strlcpy(w->name, (cJSON_IsString(nm) && nm->valuestring) ? nm->valuestring : "", sizeof(w->name));
            strlcpy(w->url, url->valuestring, sizeof(w->url));
            // The web form posts checkbox values as a number; accept a string too.
            const cJSON *sp = cJSON_GetObjectItemCaseSensitive(it, "skip_preroll");
            int spv = cJSON_IsNumber(sp) ? sp->valueint
                    : (cJSON_IsString(sp) && sp->valuestring) ? atoi(sp->valuestring) : 0;
            w->skip_preroll = spv ? 1 : 0;
        }
    }

    const cJSON *pods = cJSON_GetObjectItemCaseSensitive(root, "podcasts");
    if (cJSON_IsArray(pods)) {
        int n = cJSON_GetArraySize(pods);
        for (int i = 0; i < n && s_config.podcast_count < CFG_MAX_PODCASTS; i++) {
            const cJSON *it = cJSON_GetArrayItem(pods, i);
            if (!cJSON_IsObject(it)) continue;
            const cJSON *rss = cJSON_GetObjectItemCaseSensitive(it, "rss_url");
            if (!cJSON_IsString(rss) || !rss->valuestring || rss->valuestring[0] == '\0') continue;
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
            const cJSON *title = cJSON_GetObjectItemCaseSensitive(it, "title");
            config_podcast_t *p = &s_config.podcasts[s_config.podcast_count++];
            p->id = cJSON_IsNumber(id) ? id->valueint : (int)s_config.podcast_count;
            strlcpy(p->title, (cJSON_IsString(title) && title->valuestring) ? title->valuestring : "", sizeof(p->title));
            strlcpy(p->rss_url, rss->valuestring, sizeof(p->rss_url));
            // The web form posts numbers as strings, so accept both forms.
            const cJSON *skip = cJSON_GetObjectItemCaseSensitive(it, "skip_seconds");
            int sk = cJSON_IsNumber(skip) ? skip->valueint
                   : (cJSON_IsString(skip) && skip->valuestring) ? atoi(skip->valuestring) : 0;
            p->skip_seconds = sk < 0 ? 0 : sk;
        }
    }

    const cJSON *ui = cJSON_GetObjectItemCaseSensitive(root, "ui");
    if (cJSON_IsObject(ui)) {
        const cJSON *vol = cJSON_GetObjectItemCaseSensitive(ui, "volume");
        if (cJSON_IsNumber(vol)) s_config.ui.volume = clampi(vol->valueint, 0, 100);
        const cJSON *vmax = cJSON_GetObjectItemCaseSensitive(ui, "volume_max");
        if (cJSON_IsNumber(vmax)) s_config.ui.volume_max = clampi(vmax->valueint, 1, 100);
        const cJSON *ss = cJSON_GetObjectItemCaseSensitive(ui, "screen_sleep_seconds");
        if (cJSON_IsNumber(ss)) s_config.ui.screen_sleep_seconds = ss->valueint < 0 ? 0 : ss->valueint;
        const cJSON *lang = cJSON_GetObjectItemCaseSensitive(ui, "lang");
        if (cJSON_IsString(lang) && lang->valuestring && lang->valuestring[0])
            strlcpy(s_config.ui.lang, lang->valuestring, sizeof(s_config.ui.lang));
        const cJSON *ori = cJSON_GetObjectItemCaseSensitive(ui, "orientation");
        if (cJSON_IsNumber(ori)) s_config.ui.orientation = clampi(ori->valueint, 0, 1);
        const cJSON *dk = cJSON_GetObjectItemCaseSensitive(ui, "dark");
        if (cJSON_IsNumber(dk)) s_config.ui.dark = clampi(dk->valueint, 0, 1);
        const cJSON *ac = cJSON_GetObjectItemCaseSensitive(ui, "accent");
        if (cJSON_IsNumber(ac)) s_config.ui.accent = clampi(ac->valueint, 0, 4);
        const cJSON *gm = cJSON_GetObjectItemCaseSensitive(ui, "game");
        if (cJSON_IsNumber(gm)) s_config.ui.game = clampi(gm->valueint, 0, 1);
        const cJSON *tn = cJSON_GetObjectItemCaseSensitive(ui, "tuner");
        if (cJSON_IsNumber(tn)) s_config.ui.tuner = clampi(tn->valueint, 0, 1);
        const cJSON *mr = cJSON_GetObjectItemCaseSensitive(ui, "memo_rx");
        if (cJSON_IsNumber(mr)) s_config.ui.memo_rx = clampi(mr->valueint, 0, 1);
        const cJSON *tz = cJSON_GetObjectItemCaseSensitive(ui, "tz");
        if (cJSON_IsString(tz) && tz->valuestring && tz->valuestring[0])
            strlcpy(s_config.ui.tz, tz->valuestring, sizeof(s_config.ui.tz));
    }

    // Legacy single "alarm" object, kept forever for backward compat: maps to
    // alarms[0]. Parsed BEFORE "alarms" below so a config carrying both (a
    // page saved by old firmware, mid-transition) has the array win.
    const cJSON *alarm = cJSON_GetObjectItemCaseSensitive(root, "alarm");
    if (cJSON_IsObject(alarm)) {
        parse_one_alarm(alarm, &s_config.alarms[0]);
    }

    const cJSON *alarms = cJSON_GetObjectItemCaseSensitive(root, "alarms");
    if (cJSON_IsArray(alarms)) {
        int n = cJSON_GetArraySize(alarms);
        if (n > CFG_MAX_ALARMS) n = CFG_MAX_ALARMS;
        for (int i = 0; i < n; i++) {
            const cJSON *it = cJSON_GetArrayItem(alarms, i);
            if (!cJSON_IsObject(it)) continue;
            parse_one_alarm(it, &s_config.alarms[i]);
        }
    }

    const cJSON *favs = cJSON_GetObjectItemCaseSensitive(root, "favorites");
    if (cJSON_IsArray(favs)) {
        int n = cJSON_GetArraySize(favs);
        for (int i = 0; i < n && s_config.favorite_count < CFG_MAX_FAVORITES; i++) {
            const cJSON *it = cJSON_GetArrayItem(favs, i);
            if (!cJSON_IsObject(it)) continue;
            const cJSON *ty = cJSON_GetObjectItemCaseSensitive(it, "type");
            int type = cJSON_IsNumber(ty) ? clampi(ty->valueint, 0, 1) : 0;
            const cJSON *pa = cJSON_GetObjectItemCaseSensitive(it, "path");
            // An SD favorite without its path is unplayable: skip it.
            if (type == 1 && (!cJSON_IsString(pa) || !pa->valuestring || pa->valuestring[0] == '\0')) continue;
            config_favorite_t *f = &s_config.favorites[s_config.favorite_count++];
            f->type = type;
            const cJSON *rid = cJSON_GetObjectItemCaseSensitive(it, "radio_id");
            f->radio_id = cJSON_IsNumber(rid) ? rid->valueint : 0;
            strlcpy(f->path, (type == 1) ? pa->valuestring : "", sizeof(f->path));
            const cJSON *ti = cJSON_GetObjectItemCaseSensitive(it, "title");
            strlcpy(f->title, (cJSON_IsString(ti) && ti->valuestring) ? ti->valuestring : "", sizeof(f->title));
        }
    }

    const cJSON *quiet = cJSON_GetObjectItemCaseSensitive(root, "quiet");
    if (cJSON_IsArray(quiet)) {
        int n = cJSON_GetArraySize(quiet);
        if (n > CFG_QUIET_WINDOWS) n = CFG_QUIET_WINDOWS;
        for (int i = 0; i < n; i++) {
            const cJSON *it = cJSON_GetArrayItem(quiet, i);
            if (!cJSON_IsObject(it)) continue;
            config_quiet_window_t *q = &s_config.quiet[i];
            const cJSON *en = cJSON_GetObjectItemCaseSensitive(it, "enabled");
            if (cJSON_IsNumber(en)) q->enabled = clampi(en->valueint, 0, 1);
            const cJSON *sh = cJSON_GetObjectItemCaseSensitive(it, "start_hour");
            if (cJSON_IsNumber(sh)) q->start_hour = clampi(sh->valueint, 0, 23);
            const cJSON *sm = cJSON_GetObjectItemCaseSensitive(it, "start_minute");
            if (cJSON_IsNumber(sm)) q->start_minute = clampi(sm->valueint, 0, 59);
            const cJSON *eh = cJSON_GetObjectItemCaseSensitive(it, "end_hour");
            if (cJSON_IsNumber(eh)) q->end_hour = clampi(eh->valueint, 0, 23);
            const cJSON *em = cJSON_GetObjectItemCaseSensitive(it, "end_minute");
            if (cJSON_IsNumber(em)) q->end_minute = clampi(em->valueint, 0, 59);
            const cJSON *dy = cJSON_GetObjectItemCaseSensitive(it, "days");
            if (cJSON_IsNumber(dy)) {
                int d = dy->valueint & 0x7F;
                q->days = d ? d : 0x7F;  // 0 or missing -> every day
            }
        }
    }

    const cJSON *lim = cJSON_GetObjectItemCaseSensitive(root, "daily_limit");
    if (cJSON_IsObject(lim)) {
        const cJSON *en = cJSON_GetObjectItemCaseSensitive(lim, "enabled");
        if (cJSON_IsNumber(en)) s_config.daily_limit.enabled = clampi(en->valueint, 0, 1);
        const cJSON *mi = cJSON_GetObjectItemCaseSensitive(lim, "minutes");
        if (cJSON_IsNumber(mi)) s_config.daily_limit.minutes = clampi(mi->valueint, 5, 720);
    }
}

// Serialize a config to config.json. Writes a temp file then renames it, so a
// crash mid-write never corrupts the live config.
static esp_err_t save_to_disk(const config_t *c)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    cJSON_AddNumberToObject(root, "schema_version", c->schema_version);
    cJSON *dev = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(dev, "name", c->device_name);

    cJSON *radios = cJSON_AddArrayToObject(root, "webradios");
    for (size_t i = 0; i < c->webradio_count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", c->webradios[i].id);
        cJSON_AddStringToObject(o, "name", c->webradios[i].name);
        cJSON_AddStringToObject(o, "url", c->webradios[i].url);
        cJSON_AddNumberToObject(o, "skip_preroll", c->webradios[i].skip_preroll);
        cJSON_AddItemToArray(radios, o);
    }

    cJSON *pods = cJSON_AddArrayToObject(root, "podcasts");
    for (size_t i = 0; i < c->podcast_count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", c->podcasts[i].id);
        cJSON_AddStringToObject(o, "title", c->podcasts[i].title);
        cJSON_AddStringToObject(o, "rss_url", c->podcasts[i].rss_url);
        cJSON_AddNumberToObject(o, "skip_seconds", c->podcasts[i].skip_seconds);
        cJSON_AddItemToArray(pods, o);
    }

    cJSON *ui = cJSON_AddObjectToObject(root, "ui");
    cJSON_AddNumberToObject(ui, "volume", c->ui.volume);
    cJSON_AddNumberToObject(ui, "volume_max", c->ui.volume_max);
    cJSON_AddNumberToObject(ui, "screen_sleep_seconds", c->ui.screen_sleep_seconds);
    cJSON_AddStringToObject(ui, "lang", c->ui.lang[0] ? c->ui.lang : "en");
    cJSON_AddNumberToObject(ui, "orientation", c->ui.orientation);
    cJSON_AddNumberToObject(ui, "dark", c->ui.dark);
    cJSON_AddNumberToObject(ui, "accent", c->ui.accent);
    cJSON_AddNumberToObject(ui, "game", c->ui.game);
    cJSON_AddNumberToObject(ui, "tuner", c->ui.tuner);
    cJSON_AddNumberToObject(ui, "memo_rx", c->ui.memo_rx);
    cJSON_AddStringToObject(ui, "tz", c->ui.tz[0] ? c->ui.tz : "CET-1CEST,M3.5.0,M10.5.0/3");

    // Only "alarms" is written (the legacy "alarm" object is read forever but
    // never re-emitted): a save always upgrades a config to the new shape.
    cJSON *alarms = cJSON_AddArrayToObject(root, "alarms");
    for (int i = 0; i < CFG_MAX_ALARMS; i++) {
        const config_alarm_t *a = &c->alarms[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "enabled", a->enabled);
        cJSON_AddNumberToObject(o, "hour", a->hour);
        cJSON_AddNumberToObject(o, "minute", a->minute);
        cJSON_AddNumberToObject(o, "days", a->days);
        cJSON_AddNumberToObject(o, "source", a->source);
        cJSON_AddNumberToObject(o, "radio_id", a->radio_id);
        cJSON_AddStringToObject(o, "sd_path", a->sd_path);
        cJSON_AddStringToObject(o, "sd_title", a->sd_title);
        cJSON_AddNumberToObject(o, "volume", a->volume);
        cJSON_AddNumberToObject(o, "sunrise", a->sunrise);
        cJSON_AddItemToArray(alarms, o);
    }

    cJSON *favs = cJSON_AddArrayToObject(root, "favorites");
    for (size_t i = 0; i < c->favorite_count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "type", c->favorites[i].type);
        cJSON_AddNumberToObject(o, "radio_id", c->favorites[i].radio_id);
        cJSON_AddStringToObject(o, "path", c->favorites[i].path);
        cJSON_AddStringToObject(o, "title", c->favorites[i].title);
        cJSON_AddItemToArray(favs, o);
    }

    cJSON *quiet = cJSON_AddArrayToObject(root, "quiet");
    for (int i = 0; i < CFG_QUIET_WINDOWS; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "enabled", c->quiet[i].enabled);
        cJSON_AddNumberToObject(o, "start_hour", c->quiet[i].start_hour);
        cJSON_AddNumberToObject(o, "start_minute", c->quiet[i].start_minute);
        cJSON_AddNumberToObject(o, "end_hour", c->quiet[i].end_hour);
        cJSON_AddNumberToObject(o, "end_minute", c->quiet[i].end_minute);
        cJSON_AddNumberToObject(o, "days", c->quiet[i].days);
        cJSON_AddItemToArray(quiet, o);
    }

    cJSON *lim = cJSON_AddObjectToObject(root, "daily_limit");
    cJSON_AddNumberToObject(lim, "enabled", c->daily_limit.enabled);
    cJSON_AddNumberToObject(lim, "minutes", c->daily_limit.minutes);

    char *txt = cJSON_Print(root);
    cJSON_Delete(root);
    if (!txt) return ESP_ERR_NO_MEM;

    // Refuse to write a config larger than load_config accepts: writing it would
    // succeed, but the next boot rejects it as out-of-bounds and overwrites with
    // defaults, silently wiping every radio/podcast/alarm/parental setting. Keep
    // the good on-disk config instead and surface the error to the caller.
    if (strlen(txt) >= CONFIG_FILE_MAX) {
        ESP_LOGE(TAG, "config too large to persist (%u >= %d), not written",
                 (unsigned)strlen(txt), CONFIG_FILE_MAX);
        cJSON_free(txt);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = ESP_OK;
    FILE *f = fopen(CONFIG_TMP_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s for write", CONFIG_TMP_PATH);
        err = ESP_FAIL;
    } else {
        size_t len = strlen(txt);
        size_t wr = fwrite(txt, 1, len, f);
        fclose(f);
        if (wr != len) {
            ESP_LOGE(TAG, "short write to %s", CONFIG_TMP_PATH);
            err = ESP_FAIL;
        } else if (rename(CONFIG_TMP_PATH, CONFIG_PATH) != 0) {
            ESP_LOGE(TAG, "rename to %s failed", CONFIG_PATH);
            err = ESP_FAIL;
        }
    }

    cJSON_free(txt);
    return err;
}

// Load config.json into s_config. Always leaves a valid in-memory config.
// Missing or unparseable files self-heal by writing defaults back to flash. An
// unknown schema_version keeps the file untouched and uses defaults in memory.
static esp_err_t load_config(void)
{
    set_defaults(&s_config);

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "no config.json, writing defaults");
        return save_to_disk(&s_config);
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > CONFIG_FILE_MAX) {
        fclose(f);
        ESP_LOGE(TAG, "config.json size %ld out of bounds, restoring defaults", sz);
        return save_to_disk(&s_config);
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "config.json parse failed, restoring defaults");
        return save_to_disk(&s_config);
    }

    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (!cJSON_IsNumber(ver) || ver->valueint != CFG_SCHEMA_VERSION) {
        ESP_LOGW(TAG, "unknown schema_version, using defaults without overwriting the file");
        cJSON_Delete(root);
        return ESP_OK;
    }

    load_from_json(root);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "config loaded: %u webradios, %u podcasts, %u favorites",
             (unsigned)s_config.webradio_count, (unsigned)s_config.podcast_count,
             (unsigned)s_config.favorite_count);
    return ESP_OK;
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing NVS (%s)", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase failed");
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t mount_littlefs(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = LFS_BASE_PATH,
        .partition_label = LFS_PARTITION,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t config_store_init(void)
{
    s_cfg = heap_caps_calloc(1, sizeof(*s_cfg), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_cfg, ESP_ERR_NO_MEM, TAG, "config alloc failed");
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "nvs init failed");
    ESP_RETURN_ON_ERROR(mount_littlefs(), TAG, "littlefs init failed");
    ESP_RETURN_ON_ERROR(load_config(), TAG, "config load failed");
    s_ready = true;
    return ESP_OK;
}

const config_t *config_store_get(void)
{
    if (!s_ready) {
        ESP_LOGE(TAG, "config_store_get before successful init");
    }
    return &s_config;
}

esp_err_t config_store_set_lang(const char *code)
{
    if (!code || !code[0]) return ESP_ERR_INVALID_ARG;
    strlcpy(s_config.ui.lang, code, sizeof(s_config.ui.lang));
    return save_to_disk(&s_config);
}

esp_err_t config_store_set_orientation(int orientation)
{
    s_config.ui.orientation = (orientation == 1) ? 1 : 0;
    return save_to_disk(&s_config);
}

esp_err_t config_store_set_theme(int dark, int accent)
{
    s_config.ui.dark = clampi(dark, 0, 1);
    s_config.ui.accent = clampi(accent, 0, 4);
    return save_to_disk(&s_config);
}

esp_err_t config_store_set_alarm(int idx, const config_alarm_t *a)
{
    if (!a || idx < 0 || idx >= CFG_MAX_ALARMS) return ESP_ERR_INVALID_ARG;
    config_alarm_t *dst = &s_config.alarms[idx];
    dst->enabled = clampi(a->enabled, 0, 1);
    dst->hour = clampi(a->hour, 0, 23);
    dst->minute = clampi(a->minute, 0, 59);
    int d = a->days & 0x7F;
    dst->days = d ? d : 0x7F;  // 0 -> every day
    dst->source = clampi(a->source, 0, 1);
    dst->radio_id = a->radio_id;
    strlcpy(dst->sd_path, a->sd_path, sizeof(dst->sd_path));
    strlcpy(dst->sd_title, a->sd_title, sizeof(dst->sd_title));
    dst->volume = clampi(a->volume, 5, 100);
    dst->sunrise = clampi(a->sunrise, 0, 15);
    return save_to_disk(&s_config);
}

esp_err_t config_store_favorite_add(const config_favorite_t *f)
{
    if (!f) return ESP_ERR_INVALID_ARG;
    int type = clampi(f->type, 0, 1);
    if (type == 1 && f->path[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (s_config.favorite_count >= CFG_MAX_FAVORITES) return ESP_ERR_NO_MEM;
    config_favorite_t *dst = &s_config.favorites[s_config.favorite_count++];
    dst->type = type;
    dst->radio_id = f->radio_id;
    strlcpy(dst->path, (type == 1) ? f->path : "", sizeof(dst->path));
    strlcpy(dst->title, f->title, sizeof(dst->title));
    return save_to_disk(&s_config);
}

esp_err_t config_store_favorite_remove(int index)
{
    if (index < 0 || (size_t)index >= s_config.favorite_count) return ESP_ERR_INVALID_ARG;
    for (size_t i = (size_t)index; i + 1 < s_config.favorite_count; i++) {
        s_config.favorites[i] = s_config.favorites[i + 1];
    }
    s_config.favorite_count--;
    return save_to_disk(&s_config);
}

#define NVS_NAMESPACE   "bugne"
#define NVS_KEY_SSID    "wifi_ssid"   // slot 0; higher slots use wifi_ssid<N+1> (see wifi_keys)
#define NVS_KEY_PASS    "wifi_pass"
#define NVS_KEY_PW_SALT "pw_salt"
#define NVS_KEY_PW_HASH "pw_hash"
#define NVS_KEY_DLJOB   "dljob"
#define NVS_KEY_RESUME  "resume"
#define NVS_KEY_HISCORE "highscore"
#define NVS_KEY_MAXSTRK "maxstreak"
#define NVS_KEY_USE_DAY "use_day"
#define NVS_KEY_USE_SEC "use_sec"
#define PW_SALT_LEN     16
#define PW_HASH_LEN     32

// Slot 0 is the primary network, slots 1 and 2 are optional extra networks.
// Each slot is a pair of NVS keys.
// Backward-compatible NVS keys. Slot 0 keeps the original "wifi_ssid"/"wifi_pass";
// slot N>0 is "wifi_ssid<N+1>"/"wifi_pass<N+1>" (slot 1 -> wifi_ssid2, slot 2 ->
// wifi_ssid3, matching the previous 3-slot layout, and extending past it).
// Keys stay within NVS's 15-char limit through slot 15 ("wifi_ssid16").
static void wifi_keys(int slot, char *ssid_key, char *pass_key, size_t sz)
{
    if (slot <= 0) {
        strlcpy(ssid_key, NVS_KEY_SSID, sz);
        strlcpy(pass_key, NVS_KEY_PASS, sz);
    } else {
        snprintf(ssid_key, sz, "wifi_ssid%d", slot + 1);
        snprintf(pass_key, sz, "wifi_pass%d", slot + 1);
    }
}

esp_err_t config_store_get_wifi_slot(int slot, char *ssid, size_t ssid_size,
                                     char *pass, size_t pass_size)
{
    char ssid_key[24], pass_key[24];
    wifi_keys(slot, ssid_key, pass_key, sizeof(ssid_key));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err; // ESP_ERR_NVS_NOT_FOUND if the namespace was never written
    }
    size_t len = ssid_size;
    err = nvs_get_str(h, ssid_key, ssid, &len);
    if (err == ESP_OK) {
        len = pass_size;
        err = nvs_get_str(h, pass_key, pass, &len);
    }
    nvs_close(h);
    return err;
}

esp_err_t config_store_set_wifi_slot(int slot, const char *ssid, const char *pass)
{
    char ssid_key[24], pass_key[24];
    wifi_keys(slot, ssid_key, pass_key, sizeof(ssid_key));
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open failed");
    esp_err_t err = nvs_set_str(h, ssid_key, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, pass_key, pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t config_store_get_wifi(char *ssid, size_t ssid_size, char *pass, size_t pass_size)
{
    return config_store_get_wifi_slot(0, ssid, ssid_size, pass, pass_size);
}

esp_err_t config_store_set_wifi(const char *ssid, const char *pass)
{
    return config_store_set_wifi_slot(0, ssid, pass);
}

esp_err_t config_store_read_json(char *buf, size_t size, size_t *out_len)
{
    if (size == 0) return ESP_ERR_INVALID_ARG;
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return ESP_ERR_NOT_FOUND;
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return ESP_OK;
}

esp_err_t config_store_write_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (!cJSON_IsNumber(ver) || ver->valueint != CFG_SCHEMA_VERSION) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    set_defaults(&s_config);
    load_from_json(root);
    cJSON_Delete(root);
    return save_to_disk(&s_config);
}

static void hash_password(const uint8_t *salt, const char *plain, uint8_t *out)
{
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0); // 0 selects SHA-256
    mbedtls_sha256_update(&c, salt, PW_SALT_LEN);
    mbedtls_sha256_update(&c, (const uint8_t *)plain, strlen(plain));
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
}

bool config_store_has_password(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    esp_err_t err = nvs_get_blob(h, NVS_KEY_PW_HASH, NULL, &len);
    nvs_close(h);
    return err == ESP_OK && len == PW_HASH_LEN;
}

esp_err_t config_store_set_password(const char *plain)
{
    uint8_t salt[PW_SALT_LEN];
    esp_fill_random(salt, sizeof(salt));
    uint8_t hash[PW_HASH_LEN];
    hash_password(salt, plain, hash);

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open failed");
    esp_err_t err = nvs_set_blob(h, NVS_KEY_PW_SALT, salt, sizeof(salt));
    if (err == ESP_OK) err = nvs_set_blob(h, NVS_KEY_PW_HASH, hash, sizeof(hash));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t config_store_clear_password(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open failed");
    esp_err_t e1 = nvs_erase_key(h, NVS_KEY_PW_SALT);
    esp_err_t e2 = nvs_erase_key(h, NVS_KEY_PW_HASH);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    // Already-absent keys count as cleared.
    if (e1 != ESP_OK && e1 != ESP_ERR_NVS_NOT_FOUND) return e1;
    if (e2 != ESP_OK && e2 != ESP_ERR_NVS_NOT_FOUND) return e2;
    return err;
}

esp_err_t config_store_check_password(const char *plain)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t salt[PW_SALT_LEN];
    uint8_t stored[PW_HASH_LEN];
    size_t sl = sizeof(salt), hl = sizeof(stored);
    esp_err_t e1 = nvs_get_blob(h, NVS_KEY_PW_SALT, salt, &sl);
    esp_err_t e2 = nvs_get_blob(h, NVS_KEY_PW_HASH, stored, &hl);
    nvs_close(h);
    if (e1 != ESP_OK || e2 != ESP_OK || sl != PW_SALT_LEN || hl != PW_HASH_LEN) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t calc[PW_HASH_LEN];
    hash_password(salt, plain, calc);
    // Constant-time compare to avoid leaking the hash via timing.
    uint8_t diff = 0;
    for (int i = 0; i < PW_HASH_LEN; i++) {
        diff |= calc[i] ^ stored[i];
    }
    return diff == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t config_store_get_dljob(config_dljob_t *job)
{
    if (!job) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;  // ESP_ERR_NVS_NOT_FOUND if never written
    size_t len = sizeof(*job);
    err = nvs_get_blob(h, NVS_KEY_DLJOB, job, &len);
    nvs_close(h);
    if (err != ESP_OK) return err;
    if (len != sizeof(*job)) return ESP_ERR_NVS_NOT_FOUND;  // layout changed: ignore
    return ESP_OK;
}

esp_err_t config_store_set_dljob(const config_dljob_t *job)
{
    if (!job) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open failed");
    esp_err_t err = nvs_set_blob(h, NVS_KEY_DLJOB, job, sizeof(*job));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

uint32_t config_store_get_highscore(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return 0;
    uint32_t score = 0;
    if (nvs_get_u32(h, NVS_KEY_HISCORE, &score) != ESP_OK) score = 0;
    nvs_close(h);
    return score;
}

esp_err_t config_store_set_highscore(uint32_t score)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open failed");
    esp_err_t err = nvs_set_u32(h, NVS_KEY_HISCORE, score);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

uint32_t config_store_get_maxstreak(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return 0;
    uint32_t streak = 0;
    if (nvs_get_u32(h, NVS_KEY_MAXSTRK, &streak) != ESP_OK) streak = 0;
    nvs_close(h);
    return streak;
}

esp_err_t config_store_set_maxstreak(uint32_t streak)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open failed");
    esp_err_t err = nvs_set_u32(h, NVS_KEY_MAXSTRK, streak);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t config_store_get_usage(uint32_t *date, uint32_t *seconds)
{
    if (!date || !seconds) return ESP_ERR_INVALID_ARG;
    *date = 0;
    *seconds = 0;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    if (nvs_get_u32(h, NVS_KEY_USE_DAY, date) != ESP_OK) *date = 0;
    if (nvs_get_u32(h, NVS_KEY_USE_SEC, seconds) != ESP_OK) *seconds = 0;
    nvs_close(h);
    return ESP_OK;
}

esp_err_t config_store_set_usage(uint32_t date, uint32_t seconds)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open failed");
    esp_err_t err = nvs_set_u32(h, NVS_KEY_USE_DAY, date);
    if (err == ESP_OK) err = nvs_set_u32(h, NVS_KEY_USE_SEC, seconds);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t config_store_get_resume(config_resume_t *r)
{
    if (!r) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;  // ESP_ERR_NVS_NOT_FOUND if never written
    size_t len = sizeof(*r);
    err = nvs_get_blob(h, NVS_KEY_RESUME, r, &len);
    nvs_close(h);
    if (err != ESP_OK) return err;
    if (len != sizeof(*r)) return ESP_ERR_NVS_NOT_FOUND;  // layout changed: ignore
    return ESP_OK;
}

esp_err_t config_store_set_resume(const config_resume_t *r)
{
    if (!r) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open failed");
    esp_err_t err = nvs_set_blob(h, NVS_KEY_RESUME, r, sizeof(*r));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
