from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
UI = (ROOT / "components/ui/ui.c").read_text()


def function(name):
    match = re.search(r"^.*\b" + name + r"\([^;]*?\)\n\{", UI, re.M)
    assert match, name
    opening = UI.index("{", match.end() - 1)
    depth, end = 1, opening + 1
    while depth:
        depth += (UI[end] == "{") - (UI[end] == "}")
        end += 1
    return UI[match.start():end]


prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lang.h"
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define CFG_MAX_FAVORITES 12
#define LV_STATE_CHECKED 1
typedef int esp_err_t;
typedef struct { int value; bool checked; } lv_obj_t;
typedef struct { intptr_t user; } lv_event_t;
typedef struct { int type, radio_id; char path[256], title[64]; } config_favorite_t;
typedef struct {
    int enabled, hour, minute, days, source, radio_id, volume;
    char sd_path[256], sd_title[64];
} config_alarm_t;
typedef struct { size_t favorite_count; config_alarm_t alarms[3]; } config_t;
static config_t config;
static bool s_save_failed_pending, s_alarm_rebuild_pending, s_landscape;
static int s_alarm_edit_idx, s_dark_applied, s_accent_applied;
static int s_as_src_mode, s_as_src_radio_id;
static char s_as_src_path[256], s_as_src_title[64];
static lv_obj_t sw, hour, minute, volume, days[7];
static lv_obj_t *s_as_switch = &sw, *s_as_hour_roller = &hour, *s_as_min_roller = &minute;
static lv_obj_t *s_as_vol_slider = &volume, *s_as_day_btn[7];
static esp_err_t save_result;
static int saves, shown, status_updates, fav_index = -1;
static const char *message;
static bool in_callback;
static const config_t *config_store_get(void) { return &config; }
static bool fav_current(config_favorite_t *f) { memset(f, 0, sizeof(*f)); return true; }
static int fav_find(const config_favorite_t *f) { (void)f; return fav_index; }
static esp_err_t config_store_favorite_add(const config_favorite_t *f) {
    (void)f; saves++; if (save_result == ESP_OK) config.favorite_count++; return save_result;
}
static esp_err_t config_store_favorite_remove(int index) {
    (void)index; saves++; if (save_result == ESP_OK) config.favorite_count--; return save_result;
}
static esp_err_t config_store_set_orientation(int orientation) { (void)orientation; saves++; return save_result; }
static esp_err_t config_store_set_theme(int dark, int accent) { (void)dark; (void)accent; saves++; return save_result; }
static esp_err_t config_store_set_alarm(int index, const config_alarm_t *a) {
    saves++; if (save_result == ESP_OK) config.alarms[index] = *a; return save_result;
}
static void *lv_event_get_user_data(lv_event_t *e) { return (void *)e->user; }
static bool lv_obj_has_state(lv_obj_t *o, int state) { (void)state; return o->checked; }
static uint32_t lv_roller_get_selected(lv_obj_t *o) { return o->value; }
static int lv_slider_get_value(lv_obj_t *o) { return o->value; }
static size_t strlcpy(char *d, const char *s, size_t n) {
    size_t len = strlen(s); if (n) { size_t copy = len < n - 1 ? len : n - 1; memcpy(d, s, copy); d[copy] = 0; } return len;
}
static void toast(const char *text) { assert(!in_callback); message = text; }
static void alarm_status_refresh(void) { status_updates++; }
static void build_alarm_edit(lv_obj_t *scr) { (void)scr; hour.value = config.alarms[s_alarm_edit_idx].hour; }
static void build_now_playing(lv_obj_t *scr) { (void)scr; }
static void build_home(lv_obj_t *scr) { (void)scr; }
static void (*s_active_builder)(lv_obj_t *) = build_alarm_edit;
static void show(void (*builder)(lv_obj_t *)) { assert(!in_callback); shown++; builder(NULL); }
'''
body = "\n".join(function(name) for name in (
    "config_saved", "tick_config_save", "on_fav_toggle", "on_toggle_orientation",
    "on_theme_mode", "on_theme_accent", "alarm_settings_save",
))
cases = r'''
static void reset(void) {
    save_result = ESP_FAIL; message = NULL; shown = saves = status_updates = 0;
    s_save_failed_pending = s_alarm_rebuild_pending = false;
    in_callback = false; config.favorite_count = 1; fav_index = -1;
    s_active_builder = build_alarm_edit;
}
int main(void) {
    lv_event_t event = {.user = 1};
    for (int lang = 0; lang < 2; lang++) {
        lang_set_code(lang ? "fr" : "en");
        assert(strcmp(T(STR_SAVE_FAILED), lang ? "Impossible d'enregistrer les modifications" : "Could not save changes") == 0);
        reset(); fav_index = 0; on_fav_toggle(&event); tick_config_save();
        assert(message == T(STR_SAVE_FAILED) && config.favorite_count == 1 && !shown);
        reset(); on_fav_toggle(&event); tick_config_save();
        assert(message == T(STR_SAVE_FAILED) && !shown);
        reset(); save_result = ESP_ERR_NO_MEM; on_fav_toggle(&event); tick_config_save();
        assert(message == T(STR_SAVE_FAILED) && saves == 1);
        reset(); config.favorite_count = CFG_MAX_FAVORITES; on_fav_toggle(&event); tick_config_save();
        assert(message == T(STR_FAV_LIST_FULL) && !saves && !shown);
        reset(); save_result = ESP_OK; on_fav_toggle(&event); tick_config_save();
        assert(message == T(STR_FAV_ADDED) && shown == 1);
        reset(); fav_index = 0; save_result = ESP_OK; on_fav_toggle(&event); tick_config_save();
        assert(message == T(STR_FAV_REMOVED) && shown == 1);
        void (*callbacks[])(lv_event_t *) = {on_toggle_orientation, on_theme_mode, on_theme_accent};
        for (unsigned i = 0; i < sizeof(callbacks) / sizeof(callbacks[0]); i++) {
            reset(); in_callback = true; callbacks[i](&event); in_callback = false; tick_config_save();
            assert(message == T(STR_SAVE_FAILED) && saves == 1 && !shown);
            reset(); save_result = ESP_OK; callbacks[i](&event); tick_config_save();
            assert(!message && saves == 1);
        }
        reset(); config.alarms[0].hour = 7; hour.value = 9;
        for (int i = 0; i < 7; i++) s_as_day_btn[i] = &days[i];
        in_callback = true; alarm_settings_save(); alarm_settings_save(); in_callback = false;
        assert(saves == 1 && !shown && !status_updates && config.alarms[0].hour == 7);
        tick_config_save();
        assert(shown == 1 && hour.value == 7 && message == T(STR_SAVE_FAILED));
        reset(); in_callback = true; alarm_settings_save(); in_callback = false;
        s_active_builder = build_home; tick_config_save(); assert(!shown && message == T(STR_SAVE_FAILED));
        reset(); save_result = ESP_OK; hour.value = 8; alarm_settings_save(); tick_config_save();
        assert(config.alarms[0].hour == 8 && status_updates == 1 && !shown && !message);
    }
    puts("UI config-save feedback: English/French failure, success and deferred restore passed");
}
'''
with tempfile.TemporaryDirectory(prefix="bugne-ui-save-") as directory:
    source = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    source.write_text(prelude + body + cases)
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "components/ui/include"),
                    str(source), str(ROOT / "components/ui/lang.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
