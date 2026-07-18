// ui: ILI9341V display, FT6336G touch, LVGL. Setup QR screen, source menus,
// now-playing, and screen sleep. Playback runs on a worker task because the
// source _play() calls block until the track ends.
#include "ui.h"
#include "lang.h"
#include "board_pins.h"
#include "board.h"
#include "net.h"
#include "config_store.h"
#include "source_sd.h"
#include "source_stream.h"
#include "source_sendspin.h"
#include "podcast.h"
#include "played.h"
#include "audio.h"
#include "audio_arbiter.h"
#include "decode.h"
#include "library.h"
#include "quiet.h"
#include "usage.h"
#include "alarm_next.h"
#include "stats.h"
#include "pitch.h"
#include "memo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"  // xTaskCreateWithCaps (tuner task, PSRAM stack)
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "lvgl_private.h"  // full lv_theme_t, for the child-theme extension pattern

static const char *TAG = "ui";

// Latin-1 text font (DejaVuSans, generated) so accented titles render. Its
// fallback is the built-in Montserrat, which carries the LV_SYMBOL_* icons.
LV_FONT_DECLARE(bugne_font_14);
LV_FONT_DECLARE(bugne_font_20);
// Big clock digits (digits, colon, space only) for the alarm-ringing screen.
LV_FONT_DECLARE(bugne_font_48);

#define LCD_HRES        240
#define LCD_VRES        320
#define LCD_SPI_HOST    SPI2_HOST
#define LCD_PCLK_HZ     (40 * 1000 * 1000)
#define LCD_CMD_BITS    8
#define LCD_PARAM_BITS  8

#define SD_LIST_MAX     32

static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;
static lv_indev_t *s_touch_indev;
static bool s_landscape;  // orientation currently applied
static bool s_asleep;

// Color theme = two independent choices: dark/light mode plus an accent
// color. The LVGL default theme only shows its primary color on slider/bar
// indicators and focus rings (backgrounds, buttons and text depend on the
// dark flag alone), so a thin style layer of our own makes the accent
// visible: accent-colored buttons always, plus a pastel screen wash in light
// mode (dark mode keeps the stock dark background). Attached by the child
// theme's apply_cb below.
typedef struct { str_id_t name; lv_palette_t primary; } accent_def_t;
static const accent_def_t ACCENTS[] = {
    { STR_THEME_BLUE,   LV_PALETTE_BLUE   },  // 0: default
    { STR_THEME_OCEAN,  LV_PALETTE_TEAL   },
    { STR_THEME_PINK,   LV_PALETTE_PINK   },
    { STR_THEME_FOREST, LV_PALETTE_GREEN  },
    { STR_THEME_ORANGE, LV_PALETTE_ORANGE },
};
#define ACCENT_COUNT ((int)(sizeof(ACCENTS) / sizeof(ACCENTS[0])))
static int s_dark_applied = -1;    // 0/1 currently applied, -1 = none yet
static int s_accent_applied = -1;
static char s_tz_applied[CFG_TZ_MAX];  // TZ string currently applied via apply_tz
static int s_block_applied = -1;   // parental block (quiet hours or daily limit) currently applied, -1 = none yet
static int s_limit_warn_date;      // yyyymmdd of the last "5 minutes left" warning (one per day)

// Design tokens ("playful tiles" redesign). The mini bar floats as a rounded
// card; every bottom clearance in the layout code derives from these three so
// a bar size change stays a one-line edit.
#define MINI_BAR_H    48
#define MINI_BAR_GAP  8   // gap between the bar and the screen edges
#define MINI_CLEAR    (MINI_BAR_H + MINI_BAR_GAP + 8)  // 64: content stays above the bar
#define RADIUS_TILE   18  // home tiles
#define RADIUS_BTN    12  // buttons, list rows, keypad keys
#define RADIUS_BAR    16  // mini bar
#define RADIUS_TOAST  14
#define PAD_SIDE      12  // screen side margin (list insets)

// Per-theme style layer. Every style here is init'ed once (ui_start) and
// reset + refilled by apply_theme, via the THEME_STYLES loop below, so a live
// theme switch can never leave one stale.
static lv_style_t s_th_scr;                                        // screen wash
static lv_style_t s_th_btn, s_th_btn_pr, s_th_btn_dis;             // plain buttons
static lv_style_t s_th_list, s_th_row, s_th_row_pr;                // lists + rounded rows
static lv_style_t s_th_table, s_th_table_items;                    // episodes table
static lv_style_t s_th_btnm, s_th_btnm_items, s_th_btnm_items_pr;  // game keypad
static lv_style_t s_th_muted;                                      // secondary text, see muted()
// Filled by apply_theme but attached by builders (home tiles, round transport
// buttons): the theme apply_cb cannot identify those objects by class.
static lv_style_t s_tile, s_tile_pr, s_tile_dis;
static lv_style_t s_round_accent, s_round_accent_pr;
static lv_style_t s_round_surface, s_round_surface_pr;
// Alarm settings day-chip checked look: accent bg + white text, layered over
// s_round_surface's SURFACE default at LV_STATE_CHECKED (see build_settings_alarm).
static lv_style_t s_chip_ck;
static lv_style_t *const THEME_STYLES[] = {
    &s_th_scr, &s_th_btn, &s_th_btn_pr, &s_th_btn_dis, &s_th_list, &s_th_row,
    &s_th_row_pr, &s_th_table, &s_th_table_items, &s_th_btnm, &s_th_btnm_items,
    &s_th_btnm_items_pr, &s_th_muted, &s_tile, &s_tile_pr, &s_tile_dis,
    &s_round_accent, &s_round_accent_pr, &s_round_surface, &s_round_surface_pr,
    &s_chip_ck,
};
#define THEME_STYLE_COUNT ((int)(sizeof(THEME_STYLES) / sizeof(THEME_STYLES[0])))

static lv_theme_t s_bugne_theme;       // child of the LVGL default theme
static uint32_t s_sleep_ms;
static net_state_t s_last_net = NET_STATE_BOOT;  // to rebuild home when it changes
// Sendspin now-playing widgets, refreshed live from the sleep_timer_cb while the
// screen is shown.
static lv_obj_t *s_ss_title_lbl;
static lv_obj_t *s_ss_artist_lbl;
static lv_obj_t *s_ss_bar;
static lv_obj_t *s_ss_time_lbl;
static lv_obj_t *s_ss_pause_lbl;
static bool s_ss_prev;  // previous Sendspin session state, for edge detection

// Current LVGL resolution, follows the display rotation: 240x320 portrait,
// 320x240 landscape. Layout code must use these, never LCD_HRES/LCD_VRES
// (those stay native-panel, for the SPI/DMA/touch configs only).
static inline int32_t scr_w(void) { return lv_display_get_horizontal_resolution(s_disp); }
static inline int32_t scr_h(void) { return lv_display_get_vertical_resolution(s_disp); }

// Remote screenshot (GET /api/screenshot on the web server). Rendering must stay
// on the LVGL task, so the web task sets s_shot_req and blocks on s_shot_done;
// sleep_timer_cb (inside the LVGL task) takes the snapshot and gives the
// semaphore. s_shot_lock serializes clients. The RGB565 pixel buffer is kept
// allocated (screen area is orientation-invariant, 240*320*2 either way).
static volatile bool s_shot_req;
static SemaphoreHandle_t s_shot_done;
static SemaphoreHandle_t s_shot_lock;
static uint8_t *s_shot_buf;   // w*h*2 RGB565, PSRAM, allocated on first use
static int s_shot_w, s_shot_h;
static bool s_shot_ok;

// Remote navigation (POST /api/debug/nav). ui_remote_nav validates the name
// synchronously (a safe string compare from the web task) and stores it here;
// sleep_timer_cb applies it via show(). Empty = no request pending.
static char s_nav_req[24];

// Listening-stats reset (POST /api/stats/reset). The web task sets the flag;
// sleep_timer_cb performs stats_reset() on the LVGL task, which owns both the
// stats RAM and the stats file (no cross-task races).
static volatile bool s_stats_reset_req;

// Persistent now-playing mini bar on the top layer (survives screen changes),
// shown while something plays and we are not on a full now-playing screen.
static lv_obj_t *s_mini;
static lv_obj_t *s_mini_title;
static lv_obj_t *s_mini_pause;  // the label inside the mini pause/play button
static lv_obj_t *s_np_icy_lbl;  // web radio ICY "now playing" line on the local now-playing screen
static lv_obj_t *s_np_prog;     // SD file progress slider (draggable to seek)
static lv_obj_t *s_np_prog_time;// elapsed/total time label under the progress slider
static bool      s_np_seeking;  // user is dragging the progress slider
static uint32_t  s_np_dur_override_ms;  // exact duration (podcasts from RSS); 0 = use decoder's
static lv_obj_t *s_np_name;     // main title label (SD: tag title, else file name)
static lv_obj_t *s_np_artist;   // SD: artist from the tag (empty if none)
static lv_obj_t *s_np_vol;      // volume slider on the current screen (NULL if none)
static lv_obj_t *s_sleep_lbl;   // sleep timer remaining-time label, shared like s_np_vol
static lv_obj_t *s_ep_msg;      // episodes-screen status line (refresh errors/hints)
static lv_obj_t *s_home_clock;  // home screen wall-clock label (NULL if not built, or net offline)
static char s_meta_title[64];   // tag title of the playing SD file (empty if none yet)

// build_alarm_ringing: big clock label, refreshed by the 1 Hz tick in sleep_timer_cb.
static lv_obj_t *s_alarm_time_lbl;
// build_alarm_ringing: muted source line. Kept so the engine can switch it to
// STR_ALARM_BEEP if the beep takes over mid-ring. NULL'd in show().
static lv_obj_t *s_alarm_src_lbl;

// build_alarm_edit widgets, read by alarm_settings_save() on every control
// change. NULL'd in show(); s_as_src_mode/s_as_src_radio_id are not widgets but
// track the pending source selection (set by the source list's click handlers).
// s_alarm_edit_idx selects which alarms[] the list screen opened the editor on.
static int       s_alarm_edit_idx;
static lv_obj_t *s_as_switch;
static lv_obj_t *s_as_hour_roller;
static lv_obj_t *s_as_min_roller;
static lv_obj_t *s_as_day_btn[7];
static lv_obj_t *s_as_vol_slider;
static lv_obj_t *s_as_status_lbl;
static int       s_as_src_mode;      // 0 = web radio, 1 = SD track
static int       s_as_src_radio_id;
static char s_meta_artist[64];  // tag artist of the playing SD file

// Playback worker: source_*_play() block, so run them off the UI task.
// The worker task handles blocking operations off the UI task: track playback
// and podcast RSS refresh (network fetch + parse).
typedef enum { REQ_PLAY, REQ_REFRESH, REQ_REFRESH_ALL, REQ_DOWNLOAD_JOB, REQ_BEEP,
               REQ_MEMO_RECORD, REQ_MEMO_PLAY, REQ_MEMO_PEERS, REQ_MEMO_SEND } req_kind_t;
typedef struct {
    req_kind_t kind;
    bool is_file;
    int id;                        // podcast id (REQ_REFRESH / REQ_DOWNLOAD), peer index (REQ_MEMO_SEND)
    int skip_ms;                   // intro to skip at playback (REQ_PLAY)
    bool force;                    // REQ_DOWNLOAD: re-download episodes already on SD
    char target[PODCAST_URL_MAX];  // play target, or RSS url for REQ_REFRESH
} play_req_t;
static QueueHandle_t s_play_q;

// ---- Voice memo worker state ----
// The engine and the screens live after the tuner section; declared this early
// because play_task dispatches the runners and ui_play/ui_stop/beep_start set
// s_memo_stop (same takeover discipline as the beep).
typedef enum { MEMO_UI_IDLE, MEMO_UI_RECORDING, MEMO_UI_PREVIEW,
               MEMO_UI_PICK_PEER, MEMO_UI_SENDING } memo_ui_state_t;
#define MEMO_PEERS_MAX 8
static volatile int  s_memo_state;       // memo_ui_state_t; worker advances, UI resets
static volatile bool s_memo_stop;        // ends the memo record AND playback loops
static volatile int  s_memo_rec_ms;      // live elapsed capture time, worker -> UI tick
static volatile int  s_memo_result;      // one-shot: 1 sent, -1 send failed, -2 record failed
static volatile bool s_memo_playing;     // worker is inside the memo playback loop
static volatile bool s_memo_rx_flag;     // set by ui_memo_received (httpd task)
static char s_memo_rx_from[MEMO_SENDER_MAX];   // sender behind s_memo_rx_flag
static net_peer_t s_memo_peers[MEMO_PEERS_MAX];
static volatile int s_memo_peer_count;   // -1 = mDNS browse running on the worker
static lv_obj_t *s_memo_time_lbl, *s_memo_prog_bar, *s_memo_play_btn;  // reset in show()
static void memo_record_run(void);
static void memo_play_run(const char *path);
static void memo_send_run(int peer);
static char s_now_title[64];
// Current play target as passed to ui_play, so the now-playing star button
// (B2 favorites) can resolve a stable identity: a stream URL matching a
// configured webradio, or a local /sdcard/ path. Cleared by beep_start (the
// alarm beep has no identity to star).
static char s_now_target[PODCAST_URL_MAX];
static bool s_now_is_file;

// Current local playback context, so the now-playing screen can skip to the
// next/previous item in the same list. Web radio has no list (PLAY_CTX_NONE).
typedef enum { PLAY_CTX_NONE, PLAY_CTX_SD, PLAY_CTX_PODCAST, PLAY_CTX_LIBRARY } play_ctx_t;
static play_ctx_t s_play_ctx;
static int s_play_index;  // index in s_sd_names / s_episodes of the playing item
static volatile bool s_advance;  // a track ended on its own: UI task moves to the next item
static volatile int s_remote_cmd = -1;  // pending web command (ui_remote_t), -1 = none
static volatile int s_remote_arg;
static char s_remote_path[LIB_PATH_MAX];   // pending UI_REMOTE_PLAY_PATH argument
static char s_remote_title[LIB_NAME_MAX];  // its display title ("" = use file name)

// Navigation data buffers (single screen shown at a time).
static int s_nav_podcast_id;
static char s_nav_podcast_rss[PODCAST_URL_MAX];
static volatile bool s_refreshing;    // a podcast refresh is running on the worker
static volatile bool s_refresh_done;  // refresh finished; the UI should reload episodes
static volatile bool s_refresh_ok;    // result of the last refresh, valid with s_refresh_done
// Playback ended abnormally (stream unreachable, connection lost, decode error).
// Set by the worker, shown on the now-playing screen by sleep_timer_cb, cleared
// on the next play request. s_play_failed_local distinguishes a local-file
// failure (bad/corrupt file) from a stream failure, for the right message.
static volatile bool s_play_failed;
static volatile bool s_play_failed_local;
static volatile bool s_play_retrying;  // stream reconnect in progress (play_task retry loop)
// A stop was requested (Stop button, web stop, or a new play taking over), so the
// current play call returning is expected, not a failure. s_user_stopped cannot
// serve here: it self-clears from the UI timer as soon as audio goes idle, which
// can happen before the worker gets to check it.
static volatile bool s_stop_requested;
// Background download engine. A single job (persisted in NVS so it survives a
// reboot) is run on the worker, gated by the scheduler in sleep_timer_cb: it runs
// only when audio has been idle >5 min and Wi-Fi is up, and pauses the moment the
// user plays something. s_downloading = worker running now; s_dl_cancel = stop the
// worker (PAUSE keeps the job, user CANCEL clears it).
static volatile bool s_downloading;
static volatile bool s_dl_cancel;
static volatile int  s_dl_done;       // episodes handled so far this job
static volatile int  s_dl_total;      // episodes in scope this job
static config_dljob_t s_dljob;        // the persisted job; s_dljob.active == pending
static volatile ui_dl_phase_t s_dl_phase;  // for the web status view
static int64_t s_audio_idle_since_us; // esp_timer time audio last went idle (0 = since boot)
static volatile bool s_played_since_maint;  // audio played since the last auto-maintenance
static volatile bool s_maint_done_since_boot; // one auto-maintenance has run since boot
#define DL_IDLE_RESUME_US (5LL * 60 * 1000000)   // resume only after 5 min idle
#define DL_MIN_FREE_BYTES (150ULL * 1024 * 1024) // stop downloading below this SD free space
// GitHub firmware update check, run as an extra auto-maintenance phase (A1).
// web_config already depends on ui, so ui cannot REQUIRE web_config back
// (circular); bugne_main wires this function pointer to web_config_gh_check
// once web_config_start() succeeds. NULL means web_config is not up, so the
// phase is skipped silently.
static ui_ghota_check_fn_t s_ghota_check_fn;
static bool s_ghota_checked_once;      // false until the first auto-check attempt
static int64_t s_ghota_last_check_us;  // esp_timer time of the last auto-check attempt
#define GHOTA_CHECK_INTERVAL_US (24LL * 60 * 60 * 1000000) // at most once per day
static int s_play_podcast_skip_s;     // skip_seconds of the podcast currently playing
static int s_play_podcast_id;         // id of the podcast currently playing (0 = none)
// Podcast playback resume point (last interrupted episode only), persisted in
// NVS so it survives a reboot (there is no shutdown hook: the device can lose
// power at any time). Written immediately when an episode starts, then updated
// on a throttle while it plays; cleared on explicit Stop or natural end.
static config_resume_t s_resume;
static uint32_t s_resume_last_pos_ms;
static int64_t  s_resume_last_write_us;
#define RESUME_WRITE_INTERVAL_US (12LL * 1000000)  // throttle NVS writes during playback
// Set when the user presses Stop. The audio layer takes a moment to actually tear
// down (a source can hold seconds of buffered PCM), so the mini bar would linger.
// This hides it at once; it self-clears once playback has really gone idle.
static volatile bool s_user_stopped;
// Alarm engine (all on the LVGL task, driven by the 1 Hz block in
// sleep_timer_cb). IDLE waits for the wall clock to hit any of the
// CFG_MAX_ALARMS configured time/days (lowest index wins on a same-minute
// tie); RINGING plays the source with a volume ramp and auto-stops; SNOOZED
// re-fires after +10 min. Only one alarm rings/snoozes at a time:
// s_alarm_active_idx records which, and RINGING/SNOOZED code reads that one
// alarm's config. Snooze is RAM-only (a reboot forgets it).
typedef enum { ALARM_IDLE, ALARM_RINGING, ALARM_SNOOZED } alarm_state_t;
static alarm_state_t s_alarm_state;
static alarm_state_t s_alarm_state_shown;  // last state the home screen reflects
static int s_alarm_active_idx;         // which alarms[] is ringing/snoozed
static int64_t s_alarm_fired_min[CFG_MAX_ALARMS]; // per-alarm epoch/60 fire latch
static time_t  s_alarm_snooze_until;   // absolute epoch of the snooze re-fire
static int64_t s_alarm_ring_start_us;  // esp_timer clock: ramp + auto-stop timing
static int     s_alarm_saved_vol;      // user volume, restored at alarm end
static bool    s_alarm_beeping;        // the beep fallback took over this ring
static bool    s_alarm_beep_confirmed; // the beep was observed actually sounding
                                       // (arbiter == BEEP): only then stop retrying
static volatile bool s_beep_stop;      // set by ui_play/ui_stop: end the beep loop
static time_t  s_last_wall;            // previous 1 Hz wall clock (SNTP jump guard)

// Sunrise light (C1): progressive backlight ramp during an alarm's sunrise
// minutes before it fires. Only entered from the asleep + nothing-playing
// state. While the ramp runs, s_asleep STAYS true on purpose: the sleep
// countdown in the 50 ms tick is neutralized for free (it only arms when
// !s_asleep), and a touch rides the normal wake path (exit_sleep: backlight
// 100%), which the 1 Hz sunrise block then detects as the cancel edge.
// The percent is recomputed from the wall clock every tick, never
// accumulated. s_sunrise_block_* latch one canceled occurrence (alarm index
// + fire epoch minute) so a touch-cancel does not re-enter seconds later.
static bool    s_sunrise_active;
static int     s_sunrise_idx;           // which alarms[] the ramp is for
static int     s_sunrise_pct;           // last percent handed to bl_set
static int     s_sunrise_log_tick;      // 30 s ramp log divider
static int     s_sunrise_block_idx = -1;
static int64_t s_sunrise_block_min;     // fire epoch minute of the canceled occurrence

// Sleep timer (A2, RAM only, never persisted). esp_timer based, like the alarm
// ramp, so it is immune to an SNTP wall-clock jump. s_sleep_choice mirrors the
// web API's "value" (0=off, -1=end-of-track, else minutes armed): it is what the
// device button cycles and what a custom test value from the web looks like, but
// the engine and the auto-advance block only ever read s_sleep_stop_at_us /
// s_sleep_end_of_track (see sleep_arm).
static int     s_sleep_choice;       // 0=off, -1=end-of-track, else minutes armed
static int64_t s_sleep_stop_at_us;   // esp_timer deadline, 0 = off or end-of-track mode
static bool    s_sleep_end_of_track; // stop when the current track ends instead of a deadline
static podcast_episode_t *s_episodes;  // browse buffer (last opened podcast), PSRAM
static size_t s_ep_count;
// Snapshot of the episode list of the podcast that is playing, so next/previous
// and auto-advance follow that podcast even after the user browses to a different
// one (which overwrites s_episodes above). PSRAM-backed: the array is ~23 KB.
static podcast_episode_t *s_play_eps;
static size_t s_play_ep_count;
// The episode buffers are sized to the actual episode count (PSRAM), grown as
// needed, so a feed with many episodes is held without a fixed 30-entry cap.
static size_t s_episodes_cap;
static size_t s_play_eps_cap;

// Ensure *ptr holds at least `need` episode slots in PSRAM. Returns false on OOM.
static bool ensure_eps(podcast_episode_t **ptr, size_t *cap, size_t need)
{
    if (need == 0) need = 1;
    if (*ptr && *cap >= need) return true;
    free(*ptr);
    *ptr = heap_caps_malloc(sizeof(podcast_episode_t) * need, MALLOC_CAP_SPIRAM);
    *cap = *ptr ? need : 0;
    return *ptr != NULL;
}
#define SD_DIR_MAX      256
#define SD_BROWSE_MAX   256
static char s_sd_dir[SD_DIR_MAX];   // directory being browsed on the SD screen ("" = root)
static source_sd_entry_t *s_sd_entries;  // PSRAM listing of s_sd_dir (folders + files)
static size_t s_sd_entry_count;
static char s_sd_names[SD_LIST_MAX][SOURCE_SD_NAME_MAX];  // playable files in s_sd_dir
static size_t s_sd_count;
// Snapshot of the folder that is playing, so next/previous and auto-advance stay
// in it even if the user browses elsewhere (mirrors the podcast snapshot above).
static char s_play_sd_dir[SD_DIR_MAX];
static char (*s_play_sd_names)[SOURCE_SD_NAME_MAX];  // PSRAM
static size_t s_play_sd_count;

// Library (tag-based) browse + playback. Names buffer holds artists or albums;
// the track title/path buffers (PSRAM) hold the album being browsed, snapshotted
// into the play buffers when a track starts so auto-advance stays on that album.
#define LIB_DISP_MAX 128
static char s_lib_names[LIB_DISP_MAX][LIB_NAME_MAX];
static size_t s_lib_name_count;
static char s_lib_artist[LIB_NAME_MAX];
static char s_lib_album[LIB_NAME_MAX];
static char (*s_lib_titles)[LIB_NAME_MAX];   // PSRAM: browsed album's track titles
static char (*s_lib_paths)[LIB_PATH_MAX];    // PSRAM: browsed album's track paths
static size_t s_lib_track_count;
static char (*s_play_lib_titles)[LIB_NAME_MAX];  // PSRAM: playing album snapshot
static char (*s_play_lib_paths)[LIB_PATH_MAX];   // PSRAM
static size_t s_play_lib_count;

// ---- Backlight (LEDC PWM) ----
// The backlight pin used to be a plain GPIO; it is driven by LEDC PWM so the
// sunrise light (C1) can ramp it. Static here on purpose: every caller lives
// in this file and there is no host-testable logic, so a separate component
// would be an abstraction for single-use code.

#define BL_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER    LEDC_TIMER_0
#define BL_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BL_LEDC_FREQ_HZ  25000  // above the audible range (no PWM whine)
#define BL_DUTY_MAX      1023   // 10-bit resolution

// Set the backlight brightness 0..100. Gamma: perceived brightness follows
// roughly the square root of the duty, so duty proportional to percent
// squared makes the sunrise ramp look linear to the eye.
static void bl_set(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint32_t duty = (uint32_t)BL_DUTY_MAX * percent * percent / 10000;
    if (percent > 0 && duty == 0) duty = 1;  // never fully dark when asked lit
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

// Bring up the LEDC channel dark. Order matters for the boot no-flash rule:
// first drive the pin low as a plain GPIO (same as the pre-C1 code), then
// configure the timer, then attach the channel with duty 0, so the pin never
// sees a high level between power-on and ui_start's first-frame bl_set(100).
static esp_err_t bl_init(void)
{
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << BOARD_LCD_BL_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl), TAG, "backlight gpio failed");
    gpio_set_level(BOARD_LCD_BL_GPIO, 0);

    ledc_timer_config_t tcfg = {
        .speed_mode = BL_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tcfg), TAG, "backlight ledc timer failed");
    ledc_channel_config_t ccfg = {
        .gpio_num = BOARD_LCD_BL_GPIO,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,     // attach dark: duty 0 keeps the pin at a constant low
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ccfg), TAG, "backlight ledc channel failed");
    return ESP_OK;
}

// ---- Display / touch bring-up ----

static esp_err_t init_display(esp_lcd_panel_io_handle_t *out_io, esp_lcd_panel_handle_t *out_panel)
{
    // Keep the backlight OFF through init: it turns on in ui_start only after
    // the first screen has rendered, so boot shows no white/garbage flash.
    ESP_RETURN_ON_ERROR(bl_init(), TAG, "backlight init failed");
    bl_set(0);

    spi_bus_config_t bus = {
        .sclk_io_num = BOARD_LCD_SCLK_GPIO,
        .mosi_io_num = BOARD_LCD_MOSI_GPIO,
        .miso_io_num = BOARD_LCD_MISO_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_HRES * 80 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi init failed");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_LCD_CS_GPIO,
        .dc_gpio_num = BOARD_LCD_DC_GPIO,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, out_io),
                        TAG, "panel io failed");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9341(*out_io, &panel_cfg, out_panel), TAG, "ili9341 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*out_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*out_panel), TAG, "panel init failed");
    // This LCDWIKI ILI9341V panel needs color inversion (kidpod's ESPHome
    // config uses invert_colors: true for the same module). Without it every
    // color renders complemented: white shows black, orange shows blue. The
    // pre-theme UI unknowingly ran that way (LVGL light theme looked dark).
    esp_lcd_panel_invert_color(*out_panel, true);
    // Mirror is applied via the esp_lvgl_port rotation config (see ui_start),
    // because the port re-applies orientation when the display is added.
    esp_lcd_panel_disp_on_off(*out_panel, true);
    return ESP_OK;
}

static esp_err_t init_touch(i2c_master_bus_handle_t i2c_bus, esp_lcd_touch_handle_t *out_touch)
{
    esp_lcd_panel_io_handle_t tio;
    esp_lcd_panel_io_i2c_config_t tio_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tio_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus, &tio_cfg, &tio), TAG, "touch io failed");

    esp_lcd_touch_config_t tcfg = {
        .x_max = LCD_HRES,
        .y_max = LCD_VRES,
        .rst_gpio_num = BOARD_TOUCH_RST_GPIO,
        .int_gpio_num = BOARD_TOUCH_INT_GPIO,
        // Touch X is already aligned to the (X-mirrored) panel image; no extra
        // touch mirror, otherwise off-center targets land on the wrong side.
    };
    return esp_lcd_touch_new_i2c_ft5x06(tio, &tcfg, out_touch);
}

// Apply an orientation. The display side is hardware (ILI9341 MADCTL): setting
// the LVGL rotation fires LV_EVENT_RESOLUTION_CHANGED and esp_lvgl_port combines
// it with the base config ({mirror_x=1}) into the panel swap/mirror. ROTATION_270
// is the right-side-up landscape for this enclosure (ROTATION_90 renders upside
// down, checked on the device).
// Touch: nothing to do. This LVGL version rotates pointer input itself
// (lv_display_rotate_point, called from lv_indev.c), so the esp_lcd_touch
// swap/mirror flags MUST stay off: setting them double-transforms the taps
// (verified on the device by logging raw vs LVGL points).
static void apply_orientation(bool landscape)
{
    lv_display_set_rotation(s_disp, landscape ? LV_DISPLAY_ROTATION_270
                                              : LV_DISPLAY_ROTATION_0);
    s_landscape = landscape;
}

// Apply the configured POSIX TZ string to the C library, so localtime_r
// converts SNTP's UTC epoch to local wall time (DST included).
static void apply_tz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
}

// True once the system clock holds a plausible time (SNTP has synced at least
// once). Gates the home clock display and, later, alarm firing.
static bool time_valid(void)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    return (tmv.tm_year + 1900) >= 2025;
}

// Parental quiet hours: true while a configured no-playback window is open.
// Fail-open without valid time (same gate philosophy as the alarm).
static bool quiet_active(void)
{
    if (!time_valid()) return false;
    const config_t *c = config_store_get();
    if (!c) return false;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    int now_min = tmv.tm_hour * 60 + tmv.tm_min;
    int today = (tmv.tm_wday + 6) % 7;  // 0=Monday, same mapping as the alarm
    for (int i = 0; i < CFG_QUIET_WINDOWS; i++) {
        const config_quiet_window_t *w = &c->quiet[i];
        if (quiet_window_hit(w->enabled, w->days,
                             w->start_hour * 60 + w->start_minute,
                             w->end_hour * 60 + w->end_minute, now_min, today))
            return true;
    }
    return false;
}

// Local calendar day as yyyymmdd, or 0 before the first SNTP sync (usage.c
// drops ticks and queries on 0, so nothing is counted with an invalid clock).
static int date_today(void)
{
    if (!time_valid()) return 0;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    return (tmv.tm_year + 1900) * 10000 + (tmv.tm_mon + 1) * 100 + tmv.tm_mday;
}

// Parental daily usage limit: true once today's counted usage (listening +
// game time, accumulated in the 1 Hz block) has consumed the configured
// quota. Fail-open without valid time, same gate philosophy as quiet hours.
static bool limit_hit(void)
{
    const config_t *c = config_store_get();
    if (!c || !c->daily_limit.enabled) return false;
    int today = date_today();
    if (today == 0) return false;
    return usage_today(today) >= c->daily_limit.minutes * 60;
}

// Any parental block active (quiet hours or exhausted daily limit). Used
// where a toast is not wanted (home tile greying, debug-nav refusal).
static bool parental_blocked(void)
{
    return quiet_active() || limit_hit();
}

// Token colors, derived from the applied dark flag and accent palette so all
// five accents keep working. Read s_dark_applied/s_accent_applied, which
// apply_theme sets first, so the mini bar and toast can reuse them.
static lv_palette_t th_palette(void)
{
    int a = (s_accent_applied >= 0 && s_accent_applied < ACCENT_COUNT) ? s_accent_applied : 0;
    return ACCENTS[a].primary;
}
static bool th_dark(void) { return s_dark_applied == 1; }
static lv_color_t col_accent(void)     { return lv_palette_main(th_palette()); }
static lv_color_t col_surface(void)    { return th_dark() ? lv_color_hex(0x23262C) : lv_color_white(); }
static lv_color_t col_surface_pr(void) { return th_dark() ? lv_color_hex(0x2E323A) : lv_palette_lighten(th_palette(), 4); }
static lv_color_t col_hairline(void)   { return th_dark() ? lv_color_hex(0x32363E) : lv_palette_lighten(LV_PALETTE_GREY, 4); }
static lv_color_t col_muted(void)      { return th_dark() ? lv_color_hex(0x9AA0A8) : lv_color_hex(0x6A6F76); }

// Child-theme callback: after the default theme styles an object, add our
// per-theme layer. Screens (no parent) get the wash; buttons, list rows,
// tables and buttonmatrices get the "playful tiles" look. Builder-set local
// styles (e.g. the theme picker swatches) still override this layer.
// lv_obj_check_type matches the exact class, so plain buttons and the derived
// lv_list_button_class rows get distinct styles (rows must stay readable, not
// white-on-accent).
static void bugne_theme_apply(lv_theme_t *th, lv_obj_t *obj)
{
    (void)th;
    if (lv_obj_get_parent(obj) == NULL) {
        lv_obj_add_style(obj, &s_th_scr, 0);
    } else if (lv_obj_check_type(obj, &lv_button_class)) {
        lv_obj_add_style(obj, &s_th_btn, 0);
        lv_obj_add_style(obj, &s_th_btn_pr, LV_STATE_PRESSED);
        lv_obj_add_style(obj, &s_th_btn_dis, LV_STATE_DISABLED);
    } else if (lv_obj_check_type(obj, &lv_list_button_class)) {
        lv_obj_add_style(obj, &s_th_row, 0);
        lv_obj_add_style(obj, &s_th_row_pr, LV_STATE_PRESSED);
    } else if (lv_obj_check_type(obj, &lv_list_class)) {
        lv_obj_add_style(obj, &s_th_list, 0);
    } else if (lv_obj_check_type(obj, &lv_table_class)) {
        lv_obj_add_style(obj, &s_th_table, 0);
        lv_obj_add_style(obj, &s_th_table_items, LV_PART_ITEMS);
    } else if (lv_obj_check_type(obj, &lv_buttonmatrix_class)) {
        lv_obj_add_style(obj, &s_th_btnm, 0);
        lv_obj_add_style(obj, &s_th_btnm_items, LV_PART_ITEMS);
        lv_obj_add_style(obj, &s_th_btnm_items_pr, LV_PART_ITEMS | LV_STATE_PRESSED);
    }
}

// Apply a theme (dark mode + accent color). lv_theme_default_init
// re-initializes its single static theme in place and
// lv_obj_report_style_change(NULL) restyles every existing widget live, so no
// screen rebuild is needed (the mini bar is the one exception: its accent bg
// is local, so sleep_timer_cb recreates it after calling this). Secondary
// stays RED and the font becomes bugne_font_14 (same 14 px metrics as the
// per-screen font sets, which stay as belt and suspenders).
static void apply_theme(bool dark, int accent)
{
    if (accent < 0 || accent >= ACCENT_COUNT) accent = 0;
    s_dark_applied = dark ? 1 : 0;  // first: the col_* helpers read these
    s_accent_applied = accent;
    lv_palette_t primary = ACCENTS[accent].primary;
    lv_theme_default_init(s_disp, lv_palette_main(primary),
                          lv_palette_main(LV_PALETTE_RED), dark, &bugne_font_14);

    const lv_color_t accent_c   = col_accent();
    const lv_color_t accent_pr  = lv_palette_darken(primary, dark ? 2 : 1);
    const lv_color_t surface    = col_surface();
    const lv_color_t surface_pr = col_surface_pr();
    // Disabled text: an explicit color, because the base styles set white text
    // and white at 40% opa would vanish on a light SURFACE.
    const lv_color_t dis_text   = dark ? lv_color_white() : lv_color_black();

    for (int i = 0; i < THEME_STYLE_COUNT; i++) lv_style_reset(THEME_STYLES[i]);

    // Screens: pastel wash of the accent in light mode; dark keeps stock bg.
    if (!dark) lv_style_set_bg_color(&s_th_scr, lv_palette_lighten(primary, 5));

    // Plain buttons: rounded accent, white text.
    lv_style_set_bg_color(&s_th_btn, accent_c);
    lv_style_set_text_color(&s_th_btn, lv_color_white());
    lv_style_set_radius(&s_th_btn, RADIUS_BTN);
    lv_style_set_border_width(&s_th_btn, 0);
    lv_style_set_bg_color(&s_th_btn_pr, accent_pr);
    lv_style_set_bg_color(&s_th_btn_dis, surface);
    lv_style_set_text_color(&s_th_btn_dis, dis_text);
    lv_style_set_text_opa(&s_th_btn_dis, LV_OPA_40);

    // Lists: invisible container, rows as rounded SURFACE cards with the
    // default (readable) text color.
    lv_style_set_bg_opa(&s_th_list, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_th_list, 0);
    lv_style_set_radius(&s_th_list, 0);
    lv_style_set_pad_hor(&s_th_list, PAD_SIDE);
    lv_style_set_pad_row(&s_th_list, 6);
    lv_style_set_bg_color(&s_th_row, surface);
    lv_style_set_bg_opa(&s_th_row, LV_OPA_COVER);
    lv_style_set_radius(&s_th_row, RADIUS_BTN);
    lv_style_set_border_width(&s_th_row, 0);
    // pad 14 + the 16 px text line = 44 px rows (the tap-target floor;
    // pad 12 measured only 40 px on a bench screenshot).
    lv_style_set_pad_ver(&s_th_row, 14);
    lv_style_set_pad_hor(&s_th_row, 14);
    lv_style_set_bg_color(&s_th_row_pr, surface_pr);

    // Episodes table: flat with hairline separators (an lv_table cannot do
    // per-row cards; see ep_table_draw_cb for the offline greying).
    lv_style_set_bg_opa(&s_th_table, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_th_table, 0);
    // pad 13 + 17 px line + 1 px hairline = 44 px rows (same tap-target
    // floor as the list rows above; pad 12 measured 42 px on the bench).
    lv_style_set_pad_ver(&s_th_table_items, 13);
    lv_style_set_pad_hor(&s_th_table_items, 14);
    lv_style_set_border_width(&s_th_table_items, 1);
    lv_style_set_border_color(&s_th_table_items, col_hairline());
    lv_style_set_border_side(&s_th_table_items, LV_BORDER_SIDE_BOTTOM);

    // Game keypad: rounded SURFACE keys on a bare container (the default card
    // behind white keys would hide them in light mode); pressed key = accent.
    lv_style_set_bg_opa(&s_th_btnm, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_th_btnm, 0);
    lv_style_set_bg_color(&s_th_btnm_items, surface);
    lv_style_set_radius(&s_th_btnm_items, RADIUS_BTN);
    lv_style_set_bg_color(&s_th_btnm_items_pr, accent_c);
    lv_style_set_text_color(&s_th_btnm_items_pr, lv_color_white());

    // Secondary text token, attached by muted().
    lv_style_set_text_color(&s_th_muted, col_muted());

    // Home tiles and round icon buttons: filled here so a live theme switch
    // restyles them; attached by their builders (see the declaration comment).
    lv_style_set_radius(&s_tile, RADIUS_TILE);
    lv_style_set_bg_color(&s_tile, accent_c);
    lv_style_set_text_color(&s_tile, lv_color_white());
    lv_style_set_border_width(&s_tile, 0);
    lv_style_set_bg_color(&s_tile_pr, accent_pr);
    lv_style_set_bg_color(&s_tile_dis, surface);
    lv_style_set_text_color(&s_tile_dis, dis_text);
    lv_style_set_text_opa(&s_tile_dis, LV_OPA_40);
    lv_style_set_radius(&s_round_accent, LV_RADIUS_CIRCLE);
    lv_style_set_bg_color(&s_round_accent, accent_c);
    lv_style_set_text_color(&s_round_accent, lv_color_white());
    lv_style_set_border_width(&s_round_accent, 0);
    lv_style_set_bg_color(&s_round_accent_pr, accent_pr);
    lv_style_set_radius(&s_round_surface, LV_RADIUS_CIRCLE);
    lv_style_set_bg_color(&s_round_surface, surface);
    // Every lv_button also gets s_th_btn's white text from bugne_theme_apply,
    // which would vanish on this style's light SURFACE bg in light mode:
    // override it with the same readable color used for disabled text.
    lv_style_set_text_color(&s_round_surface, dis_text);
    lv_style_set_border_width(&s_round_surface, 0);
    lv_style_set_bg_color(&s_round_surface_pr, surface_pr);

    // Alarm day-chip checked state: accent bg, white text (see build_settings_alarm).
    lv_style_set_bg_color(&s_chip_ck, accent_c);
    lv_style_set_text_color(&s_chip_ck, lv_color_white());

    lv_obj_report_style_change(NULL);
}

// Secondary text (artist, times, hints): the muted token color, via a theme
// style so a live dark/light switch restyles these labels in place.
static void muted(lv_obj_t *label) { lv_obj_add_style(label, &s_th_muted, 0); }

// ---- Playback worker ----

static void dljob_persist(void) { config_store_set_dljob(&s_dljob); }
static void resume_persist(void) { config_store_set_resume(&s_resume); }

// True if podcast `p` is within the current job's scope.
static bool job_in_scope(const config_podcast_t *p)
{
    return s_dljob.scope_all || p->id == s_dljob.pod_id;
}

// Refresh the in-scope feed(s) before downloading (new mode only). Returns false
// if interrupted by a pause/cancel before finishing (so it re-refreshes next run).
static bool job_refresh(void)
{
    s_dl_phase = UI_DL_REFRESHING;
    const config_t *c = config_store_get();
    for (size_t i = 0; c && i < c->podcast_count; i++) {
        if (s_dl_cancel) return false;
        const config_podcast_t *p = &c->podcasts[i];
        if (!job_in_scope(p) || p->rss_url[0] == '\0') continue;
        podcast_refresh(p->id, p->title, p->rss_url);
    }
    return !s_dl_cancel;
}

// Run the persisted background download job on the worker. Resumes from the saved
// cursor, refreshes first when in "new" mode, skips cached episodes, and persists
// the cursor as it goes. On a pause/cancel (s_dl_cancel) it saves the cursor and
// returns with the job still active; on completion it clears the job.
static void worker_run_job(void)
{
    const config_t *c = config_store_get();
    if (!c) { s_downloading = false; return; }

    // Phase 1+2: refresh feeds (new mode) then download episodes. Skipped on resume
    // once downloads_done, so an interrupted library scan does not redo downloads.
    if (!s_dljob.downloads_done) {
        if (!s_dljob.force && !s_dljob.refresh_done) {
            if (!job_refresh()) { s_downloading = false; return; }  // interrupted: resume later
            s_dljob.refresh_done = true;
            dljob_persist();
        }
        s_dl_phase = UI_DL_DOWNLOADING;

        // Resume target captured before we overwrite the cursor below.
        int resume_pod = s_dljob.cur_pod_id, resume_ep = s_dljob.cur_ep_idx;

        // Pass 1: scope total + how many episodes precede the resume point (for the bar).
        int total = 0, done = 0;
        bool before = (resume_pod != 0);
        for (size_t i = 0; i < c->podcast_count; i++) {
            const config_podcast_t *p = &c->podcasts[i];
            if (!job_in_scope(p)) continue;
            int cnt = (int)podcast_manifest_count(p->id);
            total += cnt;
            if (before) {
                if (p->id == resume_pod) { done += (resume_ep < cnt ? resume_ep : cnt); before = false; }
                else done += cnt;
            }
        }
        s_dl_total = total;
        s_dl_done = done;

        // Pass 2: download from the cursor.
        bool started = (resume_pod == 0);
        for (size_t i = 0; i < c->podcast_count; i++) {
            const config_podcast_t *p = &c->podcasts[i];
            if (!job_in_scope(p)) continue;
            if (!started) { if (p->id == resume_pod) started = true; else continue; }
            size_t start_ep = (p->id == resume_pod) ? (size_t)resume_ep : 0;

            size_t n = podcast_manifest_count(p->id);
            podcast_episode_t *eps = heap_caps_malloc(sizeof(*eps) * (n ? n : 1), MALLOC_CAP_SPIRAM);
            if (!eps) { s_downloading = false; return; }
            size_t got = 0;
            podcast_read_manifest(p->id, eps, n, &got);
            int skip_s = p->skip_seconds;
            s_dljob.cur_pod_id = p->id;
            s_dljob.cur_ep_idx = (int)start_ep;
            dljob_persist();

            for (size_t e = start_ep; e < got; e++) {
                if (s_dl_cancel) { s_dljob.cur_ep_idx = (int)e; dljob_persist(); free(eps); s_downloading = false; return; }
                if (!s_dljob.force && eps[e].cached) {
                    s_dl_done++;
                } else {
                    // Stop cleanly when the card is almost full rather than failing on
                    // every remaining episode. Clear the job so it does not auto-resume
                    // into a full card; the user frees space and downloads again.
                    uint64_t freeb = 0;
                    if (source_sd_usage(NULL, &freeb) && freeb < DL_MIN_FREE_BYTES) {
                        ESP_LOGW(TAG, "SD almost full (%llu MB free), stopping download job",
                                 (unsigned long long)(freeb / (1024 * 1024)));
                        s_dljob.cur_ep_idx = (int)e;
                        s_dljob.active = false;
                        dljob_persist();
                        free(eps);
                        s_dl_phase = UI_DL_SDFULL;
                        s_downloading = false;
                        return;
                    }
                    esp_err_t r = podcast_download_episode(&eps[e], skip_s, &s_dl_cancel);
                    if (r != ESP_OK && s_dl_cancel) { s_dljob.cur_ep_idx = (int)e; dljob_persist(); free(eps); s_downloading = false; return; }
                    if (r != ESP_OK) ESP_LOGW(TAG, "  episode %u of podcast %d failed (%d)", (unsigned)e, p->id, r);
                    s_dl_done++;
                }
                s_dljob.cur_ep_idx = (int)e + 1;
                if ((e & 0x7) == 0) dljob_persist();  // throttle flash writes
            }
            free(eps);
        }

        s_dljob.downloads_done = true;  // refresh + downloads finished for this job
        dljob_persist();
    }

    // Phase 3 (auto-maintenance only): rescan the SD music library, same pause rule.
    // If the user starts playing, s_dl_cancel aborts the scan (the on-disk index is
    // left intact) and it re-runs on the next idle window.
    if (s_dljob.maint) {
        s_dl_phase = UI_DL_SCANNING;
        ESP_LOGI(TAG, "auto-maintenance: scanning the SD music library");
        library_scan_cancelable(&s_dl_cancel);
        if (s_dl_cancel) { dljob_persist(); s_downloading = false; return; }  // paused: re-scan on resume
    }

    // Phase 4 (auto-maintenance only, A1): check GitHub for a firmware update,
    // at most once per 24 h. Install stays manual (web Firmware tab). Skipped
    // silently when web_config is not up (s_ghota_check_fn is NULL until
    // bugne_main wires it, right after a successful web_config_start()).
    if (s_dljob.maint && s_ghota_check_fn) {
        int64_t now = esp_timer_get_time();
        if (!s_ghota_checked_once || (now - s_ghota_last_check_us) >= GHOTA_CHECK_INTERVAL_US) {
            s_ghota_checked_once = true;
            s_ghota_last_check_us = now;
            char latest[32];
            bool update = false;
            if (s_ghota_check_fn(latest, sizeof(latest), &update) == ESP_OK) {
                ESP_LOGI(TAG, "ghota auto-check: current=%s latest=%s update=%s",
                         esp_app_get_description()->version, latest, update ? "true" : "false");
            } else {
                ESP_LOGW(TAG, "ghota auto-check: failed (no release reachable or check error)");
            }
        }
    }

    s_dljob.active = false;  // job fully complete
    dljob_persist();
    s_dl_phase = UI_DL_IDLE;
    s_downloading = false;
    ESP_LOGI(TAG, "download job complete: %d/%d episodes", s_dl_done, s_dl_total);
}

// Alarm beep fallback tone. 1600 ms cycle: 4 bursts of 150 ms tone + 100 ms gap,
// then a 600 ms rest, repeated. Returns true when the tone should sound at t_ms.
static bool beep_pattern_on(uint32_t t_ms)
{
    if (t_ms >= 1000) return false;    // final 600 ms rest of the 1600 ms cycle
    return (t_ms % 250) < 150;         // 150 ms on, 100 ms off, four times
}

#define BEEP_RATE  22050
#define BEEP_FRAME 512   // samples per write: ~23 ms, so stop latency < 50 ms

// Generate and play the beep until s_beep_stop. Runs on the worker task (no flash
// writes, internal stack is fine). audio_write blocks on the I2S DMA, which paces
// the loop like the decoders, so no explicit delay and no watchdog concern.
static void beep_run(void)
{
    if (audio_arbiter_acquire(AUDIO_SOURCE_BEEP) != ESP_OK) return;
    if (audio_open(BEEP_RATE, 16, 1) != ESP_OK) {
        audio_arbiter_release(AUDIO_SOURCE_BEEP);
        return;
    }
    static int16_t frame[BEEP_FRAME];
    uint32_t phase = 0;
    uint32_t t_ms = 0;
    bool prev_on = false;
    while (!s_beep_stop) {
        bool on = beep_pattern_on(t_ms);
        if (on && !prev_on) phase = 0;  // start each burst at a zero crossing (no click)
        prev_on = on;
        for (int i = 0; i < BEEP_FRAME; i++) {
            int16_t sample = 0;
            if (on) {
                sample = (int16_t)(20000.0f *
                    sinf(2.0f * (float)M_PI * 880.0f * (float)phase / BEEP_RATE));
            }
            frame[i] = sample;
            phase++;
        }
        if (audio_write(frame, sizeof(frame)) != ESP_OK) break;
        t_ms = (t_ms + BEEP_FRAME * 1000 / BEEP_RATE) % 1600;
    }
    audio_close();
    audio_arbiter_release(AUDIO_SOURCE_BEEP);
}

static void play_task(void *arg)
{
    (void)arg;
    play_req_t req;
    for (;;) {
        if (xQueueReceive(s_play_q, &req, portMAX_DELAY) == pdTRUE) {
            if (req.kind == REQ_REFRESH_ALL) {
                const config_t *c = config_store_get();
                size_t n = c ? c->podcast_count : 0;
                ESP_LOGI(TAG, "refreshing all %u podcast feed(s)", (unsigned)n);
                bool ok = true;
                for (size_t i = 0; i < n; i++) {
                    esp_err_t r = podcast_refresh(c->podcasts[i].id, c->podcasts[i].title, c->podcasts[i].rss_url);
                    if (r != ESP_OK) ok = false;
                    ESP_LOGI(TAG, "  %u/%u %s: %s", (unsigned)(i + 1), (unsigned)n,
                             c->podcasts[i].title, r == ESP_OK ? "ok" : "failed");
                }
                s_refresh_ok = ok;
                s_refreshing = false;
                s_refresh_done = true;
                continue;
            }
            if (req.kind == REQ_REFRESH) {
                ESP_LOGI(TAG, "podcast %d refresh: %s", req.id, req.target);
                const char *name = "";
                const config_t *c = config_store_get();
                for (size_t i = 0; c && i < c->podcast_count; i++)
                    if (c->podcasts[i].id == req.id) { name = c->podcasts[i].title; break; }
                esp_err_t r = podcast_refresh(req.id, name, req.target);
                ESP_LOGI(TAG, "podcast %d refresh %s", req.id, r == ESP_OK ? "ok" : "failed");
                s_refresh_ok = (r == ESP_OK);
                s_refreshing = false;
                s_refresh_done = true;
                continue;
            }
            if (req.kind == REQ_DOWNLOAD_JOB) {
                worker_run_job();
                continue;
            }
            if (req.kind == REQ_BEEP) {
                beep_run();
                continue;
            }
            if (req.kind == REQ_MEMO_RECORD) {
                memo_record_run();
                continue;
            }
            if (req.kind == REQ_MEMO_PLAY) {
                memo_play_run(req.target);
                continue;
            }
            if (req.kind == REQ_MEMO_PEERS) {
                s_memo_peer_count = net_memo_peers(s_memo_peers, MEMO_PEERS_MAX);
                continue;
            }
            if (req.kind == REQ_MEMO_SEND) {
                memo_send_run(req.id);
                continue;
            }
            ESP_LOGI(TAG, "play request: %s (%s)", req.target, req.is_file ? "file" : "stream");
            s_stop_requested = false;  // a fresh play; a later stop re-arms it
            // Clear here too (not only in ui_play): if the previous play failed
            // in the instant between ui_play's clear and its takeover-stop being
            // seen, the stale flag would mark this healthy play as failed.
            s_play_failed = false;
            esp_err_t perr = ESP_OK;
            decode_set_start_skip_ms((uint32_t)req.skip_ms);  // skip a podcast intro (0 otherwise)
            if (req.is_file) {
                perr = source_sd_play(req.target);
                // A local file (SD track or cached episode) that reached its end:
                // ask the UI task to play the next item in the list. LVGL is not
                // thread-safe, so we only flag it here and let sleep_timer_cb (UI
                // task) advance.
                if ((s_play_ctx == PLAY_CTX_SD || s_play_ctx == PLAY_CTX_PODCAST) &&
                    source_sd_completed()) {
                    s_advance = true;
                    // The episode finished on its own: no resume point to keep.
                    if (s_play_ctx == PLAY_CTX_PODCAST && s_resume.active) {
                        s_resume.active = false;
                        resume_persist();
                    }
                }
            } else {
                // Streams auto-reconnect: a Wi-Fi blip otherwise silences a live
                // radio until a manual replay. Up to 6 attempts over ~2 min; any
                // deliberate stop or takeover (s_stop_requested, a queued request)
                // ends the retries instantly. An attempt that played > 60 s before
                // dying opens a fresh retry window.
                static const uint16_t retry_delay_s[] = { 2, 5, 10, 20, 30, 45 };
                int attempt = 0;
                for (;;) {
                    // Single choke point for every stream play (device UI, web
                    // /api/playback, and the alarm): arm the decoy only for a
                    // configured webradio with skip_preroll set, and always set
                    // it (even false) so a stale armed flag can never leak into
                    // an unrelated stream, e.g. a podcast episode. Re-armed on
                    // every attempt: a reconnect gets a fresh server pre-roll.
                    const config_t *sc = config_store_get();
                    bool decoy = false;
                    for (size_t i = 0; sc && i < sc->webradio_count; i++) {
                        if (strcmp(sc->webradios[i].url, req.target) == 0) {
                            decoy = sc->webradios[i].skip_preroll != 0;
                            break;
                        }
                    }
                    source_stream_set_preroll_decoy(decoy);
                    int64_t t0 = esp_timer_get_time();
                    perr = source_stream_play(req.target);
                    // A streamed podcast episode that played to its end advances
                    // too; live web radio (PLAY_CTX_NONE) never completes, so it
                    // stays put.
                    if (s_play_ctx == PLAY_CTX_PODCAST && source_stream_completed()) {
                        s_advance = true;
                        if (s_resume.active) {
                            s_resume.active = false;
                            resume_persist();
                        }
                        break;
                    }
                    // Stop, takeover, or a Music Assistant session engaged
                    // meanwhile (a reconnect must not steal the arbiter back
                    // from Sendspin): not a failure, no retry.
                    if (s_stop_requested || uxQueueMessagesWaiting(s_play_q) > 0 ||
                        source_sendspin_session_active())
                        break;
                    // Abnormal end: dead connection, unreachable stream, or a
                    // truncated episode. Retry with backoff.
                    if (esp_timer_get_time() - t0 > 60 * 1000000LL) attempt = 0;
                    if (attempt >= (int)(sizeof(retry_delay_s) / sizeof(retry_delay_s[0])))
                        break;  // retries exhausted: fall through to the failure flag
                    // Resume a streamed episode near the cut (same skip mechanism
                    // as the interrupted-resume path); a radio keeps skip_ms 0.
                    uint32_t pos = 0, dur = 0;
                    decode_progress(&pos, &dur);
                    if (s_play_ctx == PLAY_CTX_PODCAST && pos > 0) {
                        req.skip_ms = (int)pos;
                        if (s_resume.active) {
                            s_resume.pos_ms = pos;  // a reboot mid-retry resumes here
                            resume_persist();
                        }
                    }
                    s_play_retrying = true;
                    ESP_LOGW(TAG, "stream died, reconnect attempt %d in %u s",
                             attempt + 1, (unsigned)retry_delay_s[attempt]);
                    int64_t until = esp_timer_get_time() +
                                    (int64_t)retry_delay_s[attempt] * 1000000LL;
                    while (esp_timer_get_time() < until && !s_stop_requested &&
                           uxQueueMessagesWaiting(s_play_q) == 0 &&
                           !source_sendspin_session_active()) {
                        vTaskDelay(pdMS_TO_TICKS(200));
                    }
                    if (s_stop_requested || uxQueueMessagesWaiting(s_play_q) > 0 ||
                        source_sendspin_session_active())
                        break;
                    attempt++;
                    decode_set_start_skip_ms((uint32_t)req.skip_ms);
                }
                s_play_retrying = false;
            }
            // Surface abnormal ends on the now-playing screen. A live radio never
            // ends by itself, so it returning without a requested stop is a failure
            // (unreachable stream or a dead connection) even when the decode path
            // reported a clean EOF.
            if (!s_stop_requested && uxQueueMessagesWaiting(s_play_q) == 0 &&
                (perr != ESP_OK || s_play_ctx == PLAY_CTX_NONE)) {
                s_play_failed_local = req.is_file;  // pick the right message
                s_play_failed = true;
            }
            ESP_LOGI(TAG, "play finished");
        }
    }
}

// ---- Instrument tuner state (chromatic, mic capture) ----
// The tuner is the first user of the mic path (audio_record_*). Its capture
// task holds the audio arbiter as AUDIO_SOURCE_TUNER so no source can open
// the output mid-capture; conversely every playback takeover calls
// tuner_stop_sync() first (ui_play, beep_start), so the alarm always sounds.
// The full engine (task, screen) lives after the game section; only what
// ui_play/show need this early is declared here.
#define TUNER_SAMPLE_RATE   16000
#define TUNER_IN_TUNE_CENTS 5.0f
static volatile bool s_tuner_run;        // capture task loops while true
static volatile bool s_tuner_active;     // task alive; cleared by the task itself
static volatile int s_tuner_midi = -1;   // last detected MIDI note
static volatile float s_tuner_cents;     // deviation from s_tuner_midi, in cents
static volatile float s_tuner_freq;      // smoothed frequency, Hz
static volatile int64_t s_tuner_hit_us;  // esp_timer time of the last detection
static lv_obj_t *s_tuner_note_lbl, *s_tuner_freq_lbl, *s_tuner_bar;
static void build_tuner(lv_obj_t *scr);

// Stop the tuner and wait for its task to release the arbiter (bounded by one
// blocking mic read, ~128 ms). No-op when the tuner is not running.
static void tuner_stop_sync(void)
{
    if (!s_tuner_active) return;
    s_tuner_run = false;
    for (int i = 0; i < 60 && s_tuner_active; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_tuner_active) ESP_LOGW(TAG, "tuner: stop timed out");
}

static void ui_play(bool is_file, const char *target, const char *title, int skip_ms)
{
    tuner_stop_sync();        // free the mic and the arbiter before a source starts
    s_beep_stop = true;       // end the alarm beep if it is sounding (source takes over)
    s_memo_stop = true;       // end a memo record/playback the same way
    s_stop_requested = true;  // the current play (if any) is being taken over
    s_play_failed = false;    // a fresh start clears a stale error
    s_play_retrying = false;  // and any in-progress reconnect display
    audio_set_paused(false);  // never carry a paused state into a new track
    source_sd_stop();      // stop whatever is currently playing
    source_stream_stop();
    // If Music Assistant is engaged, end its session too. Only one source plays
    // at a time, and an explicit local play takes over; otherwise the Sendspin
    // session lingers (it survives stream-end) and the UI keeps showing the
    // Sendspin screen instead of this track.
    if (source_sendspin_session_active()) {
        source_sendspin_command(SENDSPIN_CMD_STOP);
    }
    strlcpy(s_now_title, title, sizeof(s_now_title));
    strlcpy(s_now_target, target, sizeof(s_now_target));  // favorite identity (star button)
    s_now_is_file = is_file;
    // Drop the previous track's tags so they do not show before the new file is
    // parsed; decode_run repopulates them at decoder init.
    decode_clear_metadata();
    s_meta_title[0] = '\0';
    s_meta_artist[0] = '\0';
    // A background download runs on this same worker task, so it must yield before
    // the play request can be served. Ask it to stop (the job stays active and
    // resumes once idle again) and start the 5-min resume debounce now.
    if (s_downloading) s_dl_cancel = true;
    s_audio_idle_since_us = esp_timer_get_time();
    play_req_t req = { .kind = REQ_PLAY, .is_file = is_file, .skip_ms = skip_ms };
    strlcpy(req.target, target, sizeof(req.target));
    xQueueOverwrite(s_play_q, &req);
}

// Sleep timer (A2): clear the armed state. Called from ui_stop (any stop path),
// alarm_fire (the alarm always wins), and the idle safety net in sleep_timer_cb.
// Declared this early so ui_stop can call it; sleep_arm/sleep_label_refresh/
// on_sleep_toggle live near build_now_playing, where the button is.
static void sleep_clear(void)
{
    s_sleep_choice = 0;
    s_sleep_stop_at_us = 0;
    s_sleep_end_of_track = false;
}

static void ui_stop(void)
{
    s_beep_stop = true;       // end the alarm beep if it is sounding
    s_memo_stop = true;       // end a memo record/playback the same way
    s_stop_requested = true;  // an expected end: not a playback failure
    s_user_stopped = true;    // hide the mini bar at once, before the source tears down
    audio_output_off();       // mute now so playback stops instantly, not after buffers drain
    audio_set_paused(false);  // release a paused decode loop so it can unwind
    sleep_clear();            // a stop always disarms the sleep timer
    source_sd_stop();
    source_stream_stop();
    // Deliberate stop: forget the resume point (only an interruption resumes).
    if (s_play_ctx == PLAY_CTX_PODCAST && s_resume.active) {
        s_resume.active = false;
        resume_persist();
    }
}

// ---- Screen navigation ----

typedef void (*screen_builder_t)(lv_obj_t *scr);

static screen_builder_t s_active_builder;  // which builder made the live screen

static void show(screen_builder_t builder)
{
    s_active_builder = builder;
    s_np_vol = NULL;  // the previous screen's slider is about to be freed
    s_tuner_note_lbl = NULL;  // same for the tuner widgets (rebuilt by build_tuner)
    s_tuner_freq_lbl = NULL;
    s_tuner_bar = NULL;
    s_memo_time_lbl = NULL;  // same for the memo record/play widgets
    s_memo_prog_bar = NULL;
    s_memo_play_btn = NULL;
    s_sleep_lbl = NULL;  // same for the sleep timer label
    s_ep_msg = NULL;  // same for the episodes status line (rebuilt by its builder)
    s_home_clock = NULL;  // same for the home clock label (rebuilt by build_home)
    s_alarm_time_lbl = NULL;  // same for the alarm-ringing big clock label
    s_alarm_src_lbl = NULL;   // and its muted source line
    s_as_switch = NULL;       // same for the alarm settings widgets
    s_as_hour_roller = NULL;
    s_as_min_roller = NULL;
    for (int i = 0; i < 7; i++) s_as_day_btn[i] = NULL;
    s_as_vol_slider = NULL;
    s_as_status_lbl = NULL;
    lv_obj_t *old = lv_screen_active();
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_text_font(scr, &bugne_font_14, 0);  // accents via DejaVu, symbols via fallback
    builder(scr);
    lv_screen_load(scr);
    if (old) {
        lv_obj_delete_async(old);  // safe to defer when called from an event
    }
}

static void build_home(lv_obj_t *scr);
static void build_settings(lv_obj_t *scr);
static void build_now_playing(lv_obj_t *scr);
static void build_sendspin_playing(lv_obj_t *scr);
static void build_episodes(lv_obj_t *scr);
static void build_podcasts(lv_obj_t *scr);
static void build_sd(lv_obj_t *scr);
static void build_settings_theme(lv_obj_t *scr);
static void build_settings_alarm(lv_obj_t *scr);  // 3-row alarm list
static void build_alarm_edit(lv_obj_t *scr);       // single-alarm editor, s_alarm_edit_idx
static void build_game(lv_obj_t *scr);
static void build_game_setup(lv_obj_t *scr);  // table picker shown before build_game
static void build_memos(lv_obj_t *scr);        // memo list (tile target)
static void build_memo_record(lv_obj_t *scr);  // record/preview/send state machine
static void build_memo_play(lv_obj_t *scr);    // single-memo player
static lv_obj_t *alarm_row(lv_obj_t *parent, lv_flex_align_t main_align);  // card row, defined with build_alarm_edit
static void alarm_fire(int idx);     // alarm engine, defined after exit_sleep
static void alarm_finish(bool snooze);
static void beep_start(void);        // beep fallback (placeholder until A5)
static void toast(const char *text); // top-layer toast, defined after the screen builders

// Parental gate, checked at every user playback entry point (GATE PLACEMENT
// RULE: never inside ui_play or the alarm code, so the alarm stays exempt by
// construction). Quiet hours first, then the daily usage limit. Toasts the
// reason and returns true when the action must be refused.
static bool play_denied(void)
{
    if (quiet_active()) { toast(T(STR_QUIET_HOURS)); return true; }
    if (limit_hit())    { toast(T(STR_LIMIT_REACHED)); return true; }
    return false;
}
static lv_obj_t *add_menu_button(lv_obj_t *scr, const char *text, int x, int y, int h, lv_event_cb_t cb);
static lv_obj_t *add_menu_button_t(lv_obj_t *scr, const char *icon, str_id_t id, int x, int y, int h, lv_event_cb_t cb);
static lv_obj_t *make_tile(lv_obj_t *scr, const char *icon, str_id_t label_id,
                            int x, int y, int w, int h, bool horizontal, lv_event_cb_t cb);
static void on_prev(lv_event_t *e);  // defined with the local playback context
static void on_next(lv_event_t *e);

static lv_obj_t *add_title(lv_obj_t *scr, const char *text)
{
    lv_obj_t *t = lv_label_create(scr);
    lv_obj_set_style_text_font(t, &bugne_font_20, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    // Centered, wide enough for most titles, narrow enough to clear one corner
    // icon button on each side (back at left, gear/refresh at right). Screens
    // with two right icons adjust after the call (see build_settings). One
    // line tall: LONG_DOT only truncates once the height is bounded too,
    // otherwise a long title wraps under the widgets below.
    lv_obj_set_size(t, scr_w() - 100, lv_font_get_line_height(&bugne_font_20));
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(t, text);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 8);
    return t;
}

// Like add_title, for screens whose only top corner widget is the round back
// button (nothing on the right): add_title's default width assumes a second
// icon on the right, wasting that margin. Widen to a single 52 px clearance
// (back button + pad) and shift right by the reclaimed amount, same trick as
// build_home's left shift for its gear-only title ("Bibliotheque" truncated
// to "Bibliothe..." in portrait before this).
static lv_obj_t *add_title_wide(lv_obj_t *scr, const char *text)
{
    lv_obj_t *t = add_title(scr, text);
    lv_obj_set_width(t, scr_w() - 64);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 24, 8);
    return t;
}

static void on_back(lv_event_t *e)
{
    (void)e;
    show(build_home);
}

// Round icon button used across the redesigned screens (back, transport, gear).
// primary=true fills with the accent color, false with the surface color; both
// get their pressed variant. d is the diameter; a large button (d >= 60) gets
// the bigger montserrat_28 glyph, smaller ones keep the default symbol size.
// The icon label is child 0 (on_pause relies on lv_obj_get_child(btn, 0)).
static lv_obj_t *make_round_btn(lv_obj_t *parent, const char *symbol, int d,
                                bool primary, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, d, d);
    lv_obj_add_style(b, primary ? &s_round_accent : &s_round_surface, 0);
    lv_obj_add_style(b, primary ? &s_round_accent_pr : &s_round_surface_pr, LV_STATE_PRESSED);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, symbol);
    if (d >= 60) lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void add_back_button(lv_obj_t *scr)
{
    lv_obj_t *b = make_round_btn(scr, LV_SYMBOL_LEFT, 44, false, on_back);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 8, 8);
}

// Centered, wrapped message for a screen whose list has nothing to show, or a
// transient status (empty library, no radios/podcasts configured, refreshing).
// Muted (secondary) text: it is informational, not an error.
static void empty_state_label(lv_obj_t *scr, str_id_t id)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, scr_w() - 20);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, T(id));
    muted(l);
    lv_obj_center(l);
}

// Now-playing -------------------------------------------------------------

static void on_stop(lv_event_t *e)
{
    (void)e;
    ui_stop();
    show(build_home);
}

static void on_pause(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    bool paused = !audio_is_paused();
    audio_set_paused(paused);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl) {
        lv_label_set_text(lbl, paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    }
}

static void on_volume(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    audio_set_volume((int)lv_slider_get_value(slider));
}

// Seekable = the current playback is a local file in a format whose decode
// loop honors decode_seek(): FLAC (exact), MP3 (byte seek) and .m4a (MP4
// sample table). Ogg and raw ADTS stay read-only. Keyed on the real target
// path, not the display title (tagged tracks and podcast episodes show a
// title with no extension).
static bool now_playing_seekable(void)
{
    if (!s_now_is_file) return false;
    const char *dot = strrchr(s_now_target, '.');
    return dot && (strcasecmp(dot, ".flac") == 0 || strcasecmp(dot, ".mp3") == 0 ||
                   strcasecmp(dot, ".m4a") == 0);
}

// Progress slider drag (SD files): hold updates while dragging, seek on release.
static void on_seek_pressed(lv_event_t *e) { (void)e; s_np_seeking = true; }

static void on_seek_released(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    uint32_t pos = 0, dur = 0;
    decode_progress(&pos, &dur);
    if (dur > 0) {
        decode_seek((uint32_t)((uint64_t)lv_slider_get_value(s) * dur / 1000));
    }
    s_np_seeking = false;
}

// ---- Sleep timer (A2) ----

// True when the current playback has a real "track ended" hook: the s_advance
// producer in the worker's play loop only runs for list contexts (SD folder,
// podcast episodes, library). Radio, Sendspin and single-path playback do not
// end on their own, so end-of-track has nothing to latch onto there.
static bool sleep_has_track_end(void)
{
    return (s_play_ctx == PLAY_CTX_SD || s_play_ctx == PLAY_CTX_PODCAST ||
            s_play_ctx == PLAY_CTX_LIBRARY) &&
           !source_sendspin_session_active();
}

// Arm the timer for `choice` (0 = off, -1 = end-of-track, else minutes 1..180).
// A context without a track end (radio, Sendspin, single path) arms a
// 60-minute deadline instead, same as picking the 60 min step, per the plan.
static void sleep_arm(int choice)
{
    s_sleep_choice = choice;
    if (choice == 0) {
        s_sleep_stop_at_us = 0;
        s_sleep_end_of_track = false;
    } else if (choice < 0) {
        if (sleep_has_track_end()) {
            s_sleep_end_of_track = true;
            s_sleep_stop_at_us = 0;
        } else {
            s_sleep_end_of_track = false;
            s_sleep_stop_at_us = esp_timer_get_time() + 60LL * 60 * 1000000;
        }
    } else {
        s_sleep_end_of_track = false;
        s_sleep_stop_at_us = esp_timer_get_time() + (int64_t)choice * 60 * 1000000;
    }
}

// Remaining-time label under the sleep button, shared by build_now_playing and
// build_sendspin_playing (like s_np_vol). Hidden when the timer is off. Called
// right after arming (immediate feedback) and once a second while the screen
// showing it is active (sleep_timer_cb never touches it otherwise).
static void sleep_label_refresh(void)
{
    if (!s_sleep_lbl) return;
    if (s_sleep_choice == 0) {
        lv_obj_add_flag(s_sleep_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_sleep_lbl, LV_OBJ_FLAG_HIDDEN);
    char buf[24];
    if (s_sleep_end_of_track) {
        strlcpy(buf, T(STR_SLEEP_REMAIN_EOT), sizeof(buf));
    } else {
        int64_t remain_us = s_sleep_stop_at_us - esp_timer_get_time();
        int remain_min = remain_us > 0 ? (int)((remain_us + 59999999) / 60000000) : 0;
        snprintf(buf, sizeof(buf), T(STR_SLEEP_REMAIN_FMT), remain_min);
    }
    if (strcmp(lv_label_get_text(s_sleep_lbl), buf) != 0) {
        lv_label_set_text(s_sleep_lbl, buf);
    }
}

// Round button on the now-playing screens: tap cycles Off -> 15 -> 30 -> 45 ->
// 60 min -> end-of-track -> Off, and toasts the new setting.
static void on_sleep_toggle(lv_event_t *e)
{
    (void)e;
    int next;
    switch (s_sleep_choice) {
    case 0:  next = 15; break;
    case 15: next = 30; break;
    case 30: next = 45; break;
    case 45: next = 60; break;
    case 60: next = -1; break;
    default: next = 0;  break;  // end-of-track, or a custom web test value: back to off
    }
    sleep_arm(next);
    sleep_label_refresh();
    char msg[32];
    if (next == 0) {
        strlcpy(msg, T(STR_SLEEP_OFF), sizeof(msg));
    } else if (next < 0) {
        strlcpy(msg, T(STR_SLEEP_SET_EOT), sizeof(msg));
    } else {
        snprintf(msg, sizeof(msg), T(STR_SLEEP_SET_FMT), next);
    }
    toast(msg);
}

// Round sleep-timer button plus its remaining-time label, top-right, mirroring
// add_back_button's top-left placement. Shared by build_now_playing and
// build_sendspin_playing (s_sleep_lbl is reset to NULL by show(), like s_np_vol).
static void add_sleep_button(lv_obj_t *scr)
{
    lv_obj_t *b = make_round_btn(scr, LV_SYMBOL_EYE_CLOSE, 44, false, on_sleep_toggle);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -8, 8);

    // Narrow and centered under the button (same x-span, -8 inset): the title
    // above is only narrowed to clear this corner up to the button's own width
    // (see build_now_playing), so the label must not spill past it either.
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_font(l, &bugne_font_14, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_size(l, 56, lv_font_get_line_height(&bugne_font_14));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    muted(l);
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -8, 56);  // clears the button (bottom 52)
    s_sleep_lbl = l;
    sleep_label_refresh();  // reflect a timer already armed (e.g. set from the web)
}

// ---- Favorites star (B2) ----

// Resolve the currently playing content to a stable favorite identity: a
// stream URL that matches a configured webradio -> type 0 with its stable id;
// a local /sdcard/ file (SD track, library track, cached episode) -> type 1
// with its SD-relative path. Anything else (Sendspin, a stream URL not in the
// radio list, the alarm beep) has no stable identity: return false and
// build_now_playing creates no star button.
static bool fav_current(config_favorite_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_now_target[0]) return false;
    if (s_now_is_file) {
        static const char pfx[] = "/sdcard/";
        if (strncmp(s_now_target, pfx, sizeof(pfx) - 1) != 0) return false;
        const char *rel = s_now_target + sizeof(pfx) - 1;
        if (!rel[0] || strlen(rel) >= sizeof(out->path)) return false;
        out->type = 1;
        strlcpy(out->path, rel, sizeof(out->path));
        strlcpy(out->title,
                (s_play_ctx == PLAY_CTX_SD && s_meta_title[0]) ? s_meta_title : s_now_title,
                sizeof(out->title));
        return true;
    }
    const config_t *c = config_store_get();
    for (size_t i = 0; c && i < c->webradio_count; i++) {
        if (strcmp(c->webradios[i].url, s_now_target) == 0) {
            out->type = 0;
            out->radio_id = c->webradios[i].id;
            strlcpy(out->title, c->webradios[i].name, sizeof(out->title));
            return true;
        }
    }
    return false;
}

// Index of this identity in the favorites list, or -1.
static int fav_find(const config_favorite_t *f)
{
    const config_t *c = config_store_get();
    for (size_t i = 0; c && i < c->favorite_count; i++) {
        const config_favorite_t *g = &c->favorites[i];
        if (g->type != f->type) continue;
        if (f->type == 0 ? (g->radio_id == f->radio_id)
                         : (strcmp(g->path, f->path) == 0)) return (int)i;
    }
    return -1;
}

// Star button tap: toggle the playing content in/out of the favorites, then
// rebuild the screen so the button shows the new state (same rebuild-to-move-
// the-mark precedent as build_settings_alarm's source picker).
static void on_fav_toggle(lv_event_t *e)
{
    (void)e;
    config_favorite_t f;
    if (!fav_current(&f)) return;  // identity lost since the build: ignore
    int idx = fav_find(&f);
    if (idx >= 0) {
        config_store_favorite_remove(idx);
        toast(T(STR_FAV_REMOVED));
    } else if (config_store_favorite_add(&f) == ESP_OK) {
        toast(T(STR_FAV_ADDED));
    } else {
        toast(T(STR_FAV_LIST_FULL));
        return;  // nothing changed: no rebuild needed
    }
    show(build_now_playing);
}

// Round favorite toggle in the top-left column, under the back button (the
// top-right corner belongs to the sleep timer, A2). Created only when the
// playing content has a stable identity. Stock montserrat carries no
// star/heart glyph (checked lv_symbol_def.h), so the state is carried by a
// plus (not in favorites: tap adds) versus an accent-filled minus (in
// favorites: tap removes). The SD-tag artist line is narrowed to the title's
// scr_w()-112 span so this button never underlaps it (see build_now_playing).
static void add_fav_button(lv_obj_t *scr)
{
    config_favorite_t f;
    if (!fav_current(&f)) return;
    bool isfav = fav_find(&f) >= 0;
    lv_obj_t *b = make_round_btn(scr, isfav ? LV_SYMBOL_MINUS : LV_SYMBOL_PLUS, 44,
                                 isfav, on_fav_toggle);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 8, 60);  // below back (bottom edge 52)
}

static void build_now_playing(lv_obj_t *scr)
{
    // Landscape compresses the vertical stack and puts the whole transport on
    // one bottom row (240 px of height cannot fit the portrait two-row layout).
    // The mini bar is hidden here (mini_bar_update), so the bottom margin is ours.
    const bool ls = scr_w() > scr_h();
    add_back_button(scr);  // return to the menu while playback continues
    add_sleep_button(scr); // sleep timer, top-right (mirrors the back button)
    add_fav_button(scr);   // favorites star, top-left column under back (B2)
    s_np_icy_lbl = NULL;
    s_np_prog = NULL;
    s_np_prog_time = NULL;
    s_np_seeking = false;
    s_np_name = NULL;
    s_np_artist = NULL;

    // Sources with a progress row get a shorter title block, so the rows
    // below (artist, progress, time) keep fixed non-overlapping positions.
    // Any local file gets the row, including single tracks played with
    // PLAY_CTX_NONE (web remote, favorites, alarm SD track).
    const bool has_prog = (s_play_ctx != PLAY_CTX_NONE) || s_now_is_file;

    // Overline: small muted header, the big label below is the track name.
    lv_obj_t *over = lv_label_create(scr);
    muted(over);
    lv_label_set_text(over, T(STR_NOW_PLAYING));
    lv_obj_align(over, LV_ALIGN_TOP_MID, 0, ls ? 10 : 16);

    lv_obj_t *name = lv_label_create(scr);
    lv_obj_set_style_text_font(name, &bugne_font_20, 0);  // big bold title
    // Bounded height: LONG_DOT wraps inside the box and ellipsizes past the
    // last line (same trick as add_title), so a long title never runs into
    // the rows below. Radios have no progress row, so they get an extra line.
    const int tlines = ls ? (has_prog ? 1 : 2) : (has_prog ? 2 : 3);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    // Width narrowed to clear both corner buttons (back at left, sleep timer at
    // right, same "underlap" fix as build_settings' title): each button's inset
    // is 8 px with a 44 px diameter, so scr_w() - 112 stays centered in the free
    // gap without needing a horizontal shift (both buttons are symmetric here).
    lv_obj_set_size(name, scr_w() - 112,
                    tlines * lv_font_get_line_height(&bugne_font_20));
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    // SD files show the tag title when known (falls back to the file name); other
    // sources keep their own title (podcast episode, radio station).
    lv_label_set_text(name, (s_play_ctx == PLAY_CTX_SD && s_meta_title[0]) ? s_meta_title : s_now_title);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, ls ? 54 : 40);  // clears back button (bottom 52)
    s_np_name = name;

    if (s_play_ctx == PLAY_CTX_SD) {
        // Artist line under the title, filled from the tag once parsed.
        lv_obj_t *ar = lv_label_create(scr);
        lv_obj_set_style_text_font(ar, &bugne_font_14, 0);
        lv_label_set_long_mode(ar, LV_LABEL_LONG_DOT);
        // One line: LONG_DOT truncates only once the height is bounded too.
        // Same width as the title: the favorites button sits at the left edge
        // down to y 104, and this row (y 84/94) must clear it (B2).
        lv_obj_set_size(ar, scr_w() - 112, lv_font_get_line_height(&bugne_font_14));
        lv_obj_set_style_text_align(ar, LV_TEXT_ALIGN_CENTER, 0);
        muted(ar);
        lv_label_set_text(ar, s_meta_artist);
        lv_obj_align(ar, LV_ALIGN_TOP_MID, 0, ls ? 84 : 94);  // under the bounded title block
        s_np_artist = ar;
    }

    if (has_prog) {
        // Local files (SD, library, cached episodes): a progress slider plus a
        // time label. Draggable to seek when the format supports it; streamed
        // episodes keep a read-only bar.
        lv_obj_t *prog = lv_slider_create(scr);
        lv_obj_set_width(prog, scr_w() - 32);
        lv_slider_set_range(prog, 0, 1000);
        lv_slider_set_value(prog, 0, LV_ANIM_OFF);
        lv_obj_align(prog, LV_ALIGN_TOP_MID, 0, ls ? 106 : 116);
        if (now_playing_seekable()) {
            lv_obj_add_event_cb(prog, on_seek_pressed, LV_EVENT_PRESSED, NULL);
            lv_obj_add_event_cb(prog, on_seek_released, LV_EVENT_RELEASED, NULL);
        } else {
            lv_obj_remove_flag(prog, LV_OBJ_FLAG_CLICKABLE);  // read-only bar
            // Visual cue that this one cannot seek: hide the drag knob.
            lv_obj_set_style_bg_opa(prog, LV_OPA_TRANSP, LV_PART_KNOB);
        }
        s_np_prog = prog;

        lv_obj_t *t = lv_label_create(scr);
        muted(t);
        lv_label_set_text(t, "0:00 / 0:00");
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, ls ? 124 : 134);
        s_np_prog_time = t;
    } else {
        // Web radio "now playing" (ICY StreamTitle), updated live; empty otherwise.
        char icy[128];
        source_stream_title(icy, sizeof(icy));
        lv_obj_t *icyl = lv_label_create(scr);
        lv_obj_set_style_text_font(icyl, &bugne_font_14, 0);
        lv_label_set_long_mode(icyl, LV_LABEL_LONG_DOT);
        // One line: LONG_DOT truncates only once the height is bounded too.
        lv_obj_set_size(icyl, scr_w() - 2 * PAD_SIDE, lv_font_get_line_height(&bugne_font_14));
        lv_obj_set_style_text_align(icyl, LV_TEXT_ALIGN_CENTER, 0);
        muted(icyl);
        lv_label_set_text(icyl, icy);
        lv_obj_align(icyl, LV_ALIGN_TOP_MID, 0, ls ? 108 : 118);  // under the taller radio title
        s_np_icy_lbl = icyl;
    }

    // Volume: icon plus slider, above the transport cluster.
    const int vy = ls ? 144 : 154;
    lv_obj_t *vol = lv_slider_create(scr);
    lv_obj_set_width(vol, scr_w() - 60);
    lv_slider_set_range(vol, 0, audio_get_volume_limit());  // ceiling from config
    lv_slider_set_value(vol, audio_get_volume(), LV_ANIM_OFF);
    lv_obj_align(vol, LV_ALIGN_TOP_MID, 12, vy);
    lv_obj_add_event_cb(vol, on_volume, LV_EVENT_VALUE_CHANGED, NULL);
    s_np_vol = vol;  // so a web volume change can move this slider too
    lv_obj_t *vicon = lv_label_create(scr);
    lv_label_set_text(vicon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_align(vicon, LV_ALIGN_TOP_LEFT, PAD_SIDE, vy - 2);

    // Transport: round buttons. Next/previous move through the current list (SD
    // folder or podcast episodes); they are disabled for a single web radio
    // stream. Playback starts running, so show the pause icon.
    bool has_list = (s_play_ctx != PLAY_CTX_NONE);

    const char *pause_sym = audio_is_paused() ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE;
    lv_obj_t *prev, *pause, *next, *stop;
    if (ls) {
        // Landscape: one centered bottom row (prev, play, next, stop).
        prev  = make_round_btn(scr, LV_SYMBOL_PREV, 48, false, on_prev);
        pause = make_round_btn(scr, pause_sym,      60, true,  on_pause);
        next  = make_round_btn(scr, LV_SYMBOL_NEXT, 48, false, on_next);
        stop  = make_round_btn(scr, LV_SYMBOL_STOP, 48, false, on_stop);
        lv_obj_align(prev,  LV_ALIGN_BOTTOM_MID, -102, -16);
        lv_obj_align(pause, LV_ALIGN_BOTTOM_MID,  -32, -10);
        lv_obj_align(next,  LV_ALIGN_BOTTOM_MID,   38, -16);
        lv_obj_align(stop,  LV_ALIGN_BOTTOM_MID,  102, -16);
    } else {
        // Portrait: prev/play/next in a row, stop centered below.
        prev  = make_round_btn(scr, LV_SYMBOL_PREV, 52, false, on_prev);
        pause = make_round_btn(scr, pause_sym,      68, true,  on_pause);
        next  = make_round_btn(scr, LV_SYMBOL_NEXT, 52, false, on_next);
        stop  = make_round_btn(scr, LV_SYMBOL_STOP, 44, false, on_stop);
        lv_obj_align(prev,  LV_ALIGN_BOTTOM_MID, -76, -90);
        lv_obj_align(pause, LV_ALIGN_BOTTOM_MID,   0, -82);
        lv_obj_align(next,  LV_ALIGN_BOTTOM_MID,  76, -90);
        lv_obj_align(stop,  LV_ALIGN_BOTTOM_MID,   0, -16);
    }
    if (!has_list) {
        lv_obj_add_state(prev, LV_STATE_DISABLED);
        lv_obj_add_state(next, LV_STATE_DISABLED);
    }
}

// Now-playing for Sendspin (Music Assistant) ------------------------------
// Controls send transport commands to MA; the screen opens/closes automatically
// from the playback state (see sleep_timer_cb), not from local navigation.

static void ss_fill_title(char *buf, size_t size)
{
    if (source_sendspin_title(buf, size) == 0) {
        strlcpy(buf, "Music Assistant", size);
    }
}

// Format milliseconds as m:ss into buf.
static void ss_fmt_time(char *buf, size_t size, uint32_t ms)
{
    uint32_t s = ms / 1000;
    snprintf(buf, size, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

static void on_ss_pause(lv_event_t *e)
{
    (void)e;
    // Derive the intent from the actual stream state, so it stays correct even if
    // playback was paused/resumed from Music Assistant itself.
    source_sendspin_command(source_sendspin_active() ? SENDSPIN_CMD_PAUSE : SENDSPIN_CMD_PLAY);
}

static void on_ss_next(lv_event_t *e) { (void)e; source_sendspin_command(SENDSPIN_CMD_NEXT); }
static void on_ss_prev(lv_event_t *e) { (void)e; source_sendspin_command(SENDSPIN_CMD_PREVIOUS); }

static void on_ss_stop(lv_event_t *e)
{
    (void)e;
    s_user_stopped = true;  // hide the mini bar at once (session teardown lags)
    source_sendspin_command(SENDSPIN_CMD_STOP);  // ends the session; the screen then closes
}

static void build_sendspin_playing(lv_obj_t *scr)
{
    // Mirrors build_now_playing's has_prog layout (SD/podcast/library): same
    // overline + bounded title + muted artist + progress row + volume +
    // transport cluster. Only the progress widget differs (read-only bar, no
    // seek events, since Music Assistant is not scrubbable from here).
    const bool ls = scr_w() > scr_h();
    add_back_button(scr);  // browse other screens while MA keeps streaming
    add_sleep_button(scr); // sleep timer, top-right (mirrors the back button)

    // Overline: same text this screen always showed, now styled as a small
    // muted header instead of the big title (the track title takes that spot).
    lv_obj_t *over = lv_label_create(scr);
    muted(over);
    lv_label_set_text(over, LV_SYMBOL_AUDIO " Music Assistant");
    lv_obj_align(over, LV_ALIGN_TOP_MID, 0, ls ? 10 : 16);

    char t[96];
    ss_fill_title(t, sizeof(t));
    lv_obj_t *name = lv_label_create(scr);
    lv_obj_set_style_text_font(name, &bugne_font_20, 0);  // big bold title
    const int tlines = ls ? 1 : 2;
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    // Narrowed to clear both corner buttons, same reasoning as build_now_playing.
    lv_obj_set_size(name, scr_w() - 112,
                    tlines * lv_font_get_line_height(&bugne_font_20));
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(name, t);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, ls ? 54 : 40);  // clears back button
    s_ss_title_lbl = name;

    char a[96];
    source_sendspin_artist(a, sizeof(a));
    lv_obj_t *artist = lv_label_create(scr);
    lv_obj_set_style_text_font(artist, &bugne_font_14, 0);
    lv_label_set_long_mode(artist, LV_LABEL_LONG_DOT);
    lv_obj_set_size(artist, scr_w() - 2 * PAD_SIDE, lv_font_get_line_height(&bugne_font_14));
    lv_obj_set_style_text_align(artist, LV_TEXT_ALIGN_CENTER, 0);
    muted(artist);
    lv_label_set_text(artist, a);
    lv_obj_align(artist, LV_ALIGN_TOP_MID, 0, ls ? 84 : 94);  // under the bounded title block
    s_ss_artist_lbl = artist;

    // Progress bar (read-only, no seek: Music Assistant owns the queue) plus
    // an elapsed/total time label, same slots as build_now_playing's slider.
    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, scr_w() - 32, 8);
    lv_bar_set_range(bar, 0, 1000);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, ls ? 106 : 116);
    s_ss_bar = bar;

    lv_obj_t *tm = lv_label_create(scr);
    muted(tm);
    lv_label_set_text(tm, "0:00 / 0:00");
    lv_obj_align(tm, LV_ALIGN_TOP_MID, 0, ls ? 124 : 134);
    s_ss_time_lbl = tm;

    // Volume: icon plus slider, above the transport cluster (same slot as
    // build_now_playing).
    const int vy = ls ? 144 : 154;
    lv_obj_t *vol = lv_slider_create(scr);
    lv_obj_set_width(vol, scr_w() - 60);
    lv_slider_set_range(vol, 0, audio_get_volume_limit());  // ceiling from config
    lv_slider_set_value(vol, audio_get_volume(), LV_ANIM_OFF);
    lv_obj_align(vol, LV_ALIGN_TOP_MID, 12, vy);
    lv_obj_add_event_cb(vol, on_volume, LV_EVENT_VALUE_CHANGED, NULL);
    s_np_vol = vol;  // so a web volume change can move this slider too
    lv_obj_t *vicon = lv_label_create(scr);
    lv_label_set_text(vicon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_align(vicon, LV_ALIGN_TOP_LEFT, PAD_SIDE, vy - 2);

    // Transport: round buttons, same layout and offsets as build_now_playing.
    // Next/previous skip the Music Assistant queue; always enabled (a session
    // always implies a queue).
    const char *pause_sym = source_sendspin_active() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
    lv_obj_t *prev, *pause, *next, *stop;
    if (ls) {
        prev  = make_round_btn(scr, LV_SYMBOL_PREV, 48, false, on_ss_prev);
        pause = make_round_btn(scr, pause_sym,      60, true,  on_ss_pause);
        next  = make_round_btn(scr, LV_SYMBOL_NEXT, 48, false, on_ss_next);
        stop  = make_round_btn(scr, LV_SYMBOL_STOP, 48, false, on_ss_stop);
        lv_obj_align(prev,  LV_ALIGN_BOTTOM_MID, -102, -16);
        lv_obj_align(pause, LV_ALIGN_BOTTOM_MID,  -32, -10);
        lv_obj_align(next,  LV_ALIGN_BOTTOM_MID,   38, -16);
        lv_obj_align(stop,  LV_ALIGN_BOTTOM_MID,  102, -16);
    } else {
        prev  = make_round_btn(scr, LV_SYMBOL_PREV, 52, false, on_ss_prev);
        pause = make_round_btn(scr, pause_sym,      68, true,  on_ss_pause);
        next  = make_round_btn(scr, LV_SYMBOL_NEXT, 52, false, on_ss_next);
        stop  = make_round_btn(scr, LV_SYMBOL_STOP, 44, false, on_ss_stop);
        lv_obj_align(prev,  LV_ALIGN_BOTTOM_MID, -76, -90);
        lv_obj_align(pause, LV_ALIGN_BOTTOM_MID,   0, -82);
        lv_obj_align(next,  LV_ALIGN_BOTTOM_MID,  76, -90);
        lv_obj_align(stop,  LV_ALIGN_BOTTOM_MID,   0, -16);
    }
    // The icon label is child 0 of the button (make_round_btn), same
    // convention on_pause relies on for the local now-playing screen; ss_refresh
    // updates it by the stored label pointer.
    s_ss_pause_lbl = lv_obj_get_child(pause, 0);
}

// Refresh the live Sendspin widgets (called from the timer while the screen shows).
static void ss_refresh(void)
{
    char t[96];
    ss_fill_title(t, sizeof(t));
    if (s_ss_title_lbl && strcmp(lv_label_get_text(s_ss_title_lbl), t) != 0) {
        lv_label_set_text(s_ss_title_lbl, t);
    }
    char a[96];
    source_sendspin_artist(a, sizeof(a));
    if (s_ss_artist_lbl && strcmp(lv_label_get_text(s_ss_artist_lbl), a) != 0) {
        lv_label_set_text(s_ss_artist_lbl, a);
    }
    uint32_t pos = 0, dur = 0;
    source_sendspin_progress(&pos, &dur);
    if (s_ss_bar) {
        lv_bar_set_value(s_ss_bar, dur ? (int)((uint64_t)pos * 1000 / dur) : 0, LV_ANIM_OFF);
    }
    if (s_ss_time_lbl) {
        char tm[32], cur[12], tot[12];
        ss_fmt_time(cur, sizeof(cur), pos);
        if (dur) {
            ss_fmt_time(tot, sizeof(tot), dur);
            snprintf(tm, sizeof(tm), "%s / %s", cur, tot);
        } else {
            strlcpy(tm, cur, sizeof(tm));
        }
        if (strcmp(lv_label_get_text(s_ss_time_lbl), tm) != 0) {
            lv_label_set_text(s_ss_time_lbl, tm);
        }
    }
    if (s_ss_pause_lbl) {
        const char *want = source_sendspin_active() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
        if (strcmp(lv_label_get_text(s_ss_pause_lbl), want) != 0) {
            lv_label_set_text(s_ss_pause_lbl, want);
        }
    }
}

// Web radios --------------------------------------------------------------

static void on_webradio(lv_event_t *e)
{
    if (play_denied()) return;
    lv_obj_t *btn = lv_event_get_target(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(btn);
    const config_t *c = config_store_get();
    if (c && i >= 0 && (size_t)i < c->webradio_count) {
        s_play_ctx = PLAY_CTX_NONE;  // a single stream, no next/previous
        ui_play(false, c->webradios[i].url, c->webradios[i].name, 0);
        show(build_now_playing);
    }
}

// Row titles in lists are statically truncated with an ellipsis. By default
// LVGL gives list-button labels a continuous circular scroll animation; with
// many long rows that keeps the render task busy and looks visually busy, which
// is what made long lists feel sluggish. We turn it off so every row shows a
// fixed, truncated title: consistent whether the list is idle or scrolling.
static void list_titles_static(lv_obj_t *list)
{
    uint32_t n = lv_obj_get_child_count(list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *lbl = lv_obj_get_child(lv_obj_get_child(list, i), -1);  // label is last child
        if (lbl) lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    }
}

static void build_webradios(lv_obj_t *scr)
{
    add_back_button(scr);
    add_title_wide(scr, T(STR_WEBRADIOS));
    const config_t *c = config_store_get();
    if (!c || c->webradio_count == 0) {
        empty_state_label(scr, STR_NO_WEBRADIOS);
        return;
    }
    lv_obj_t *list = lv_list_create(scr);
    lv_obj_set_size(list, scr_w(), scr_h() - 56);  // below the 44 px round back button (8+44)
    lv_obj_set_style_pad_bottom(list, MINI_CLEAR, 0);  // last row scrolls clear of the floating mini bar
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    for (size_t i = 0; c && i < c->webradio_count; i++) {
        lv_obj_t *btn = lv_list_add_button(list, LV_SYMBOL_AUDIO, c->webradios[i].name);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, on_webradio, LV_EVENT_CLICKED, NULL);
    }
    list_titles_static(list);
}

// SD files ----------------------------------------------------------------

// Play item `i` of the active local context (SD list or episode list). When
// show_np is true also open the now-playing screen; auto-advance passes false
// to leave the user on whatever screen they browsed to. Out-of-range indices
// are ignored (list edges).
static void play_ctx_at_ex(int i, bool show_np)
{
    if (play_denied()) return;
    if (s_play_ctx == PLAY_CTX_SD) {
        // Navigate the playing folder's own snapshot, not the live browse list.
        if (!s_play_sd_names || i < 0 || (size_t)i >= s_play_sd_count) return;
        char path[16 + SD_DIR_MAX + SOURCE_SD_NAME_MAX];
        if (s_play_sd_dir[0]) {
            snprintf(path, sizeof(path), "/sdcard/%s/%s", s_play_sd_dir, s_play_sd_names[i]);
        } else {
            snprintf(path, sizeof(path), "/sdcard/%s", s_play_sd_names[i]);
        }
        ui_play(true, path, s_play_sd_names[i], 0);
        s_np_dur_override_ms = 0;  // SD: use the decoder's duration
    } else if (s_play_ctx == PLAY_CTX_PODCAST) {
        // Navigate the playing podcast's own snapshot, not the browse buffer.
        if (!s_play_eps || i < 0 || (size_t)i >= s_play_ep_count) return;
        const podcast_episode_t *ep = &s_play_eps[i];
        // Only a cached MP3 was physically trimmed at download, so it plays from
        // the start. Everything else (streamed, or cached FLAC/AAC which are not
        // trimmed) skips the intro at playback.
        size_t cpl = strlen(ep->cache_path);
        bool cached_mp3 = ep->cached && cpl >= 4 && strcasecmp(ep->cache_path + cpl - 4, ".mp3") == 0;
        int skip_ms = cached_mp3 ? 0 : s_play_podcast_skip_s * 1000;
        // Resume: if this is the episode interrupted (by a power loss) last
        // session, start from the saved position instead of the intro skip.
        // pos_ms is on the intro-inclusive timeline; a trimmed cached MP3's own
        // timeline already starts skip_seconds in, so subtract that back out.
        if (s_resume.active && s_resume.podcast_id == s_play_podcast_id &&
            strcmp(s_resume.episode_url, ep->episode_url) == 0) {
            int32_t resume_skip = (int32_t)s_resume.pos_ms -
                (cached_mp3 ? s_play_podcast_skip_s * 1000 : 0);
            skip_ms = resume_skip > 0 ? resume_skip : 0;
        }
        if (ep->cached) ui_play(true, ep->cache_path, ep->title, skip_ms);
        else            ui_play(false, ep->episode_url, ep->title, skip_ms);
        // Use the exact duration from the RSS feed (the byte-rate estimate drifts
        // on VBR/tagged podcast MP3s). 0 falls back to the estimate.
        s_np_dur_override_ms = (uint32_t)ep->duration_seconds * 1000;
        // Record this as the resume point right away (before the first periodic
        // save), so a power loss just after selecting an episode still resumes
        // here rather than leaving a stale record from whatever played before.
        s_resume.active = true;
        s_resume.podcast_id = s_play_podcast_id;
        strlcpy(s_resume.episode_url, ep->episode_url, sizeof(s_resume.episode_url));
        s_resume.cached_trimmed_mp3 = cached_mp3;
        s_resume.pos_ms = cached_mp3 ? (uint32_t)skip_ms + (uint32_t)s_play_podcast_skip_s * 1000
                                      : (uint32_t)skip_ms;
        resume_persist();
        s_resume_last_pos_ms = s_resume.pos_ms;
        s_resume_last_write_us = esp_timer_get_time();
    } else if (s_play_ctx == PLAY_CTX_LIBRARY) {
        if (!s_play_lib_paths || i < 0 || (size_t)i >= s_play_lib_count) return;
        char path[16 + LIB_PATH_MAX];
        snprintf(path, sizeof(path), "/sdcard/%s", s_play_lib_paths[i]);
        ui_play(true, path, s_play_lib_titles[i], 0);
        s_np_dur_override_ms = 0;
    } else {
        return;
    }
    s_play_index = i;
    if (show_np) show(build_now_playing);
}

static void play_ctx_at(int i) { play_ctx_at_ex(i, true); }

static void on_next(lv_event_t *e) { (void)e; play_ctx_at(s_play_index + 1); }
static void on_prev(lv_event_t *e) { (void)e; play_ctx_at(s_play_index - 1); }

// ---- Remote control from the web server ----
// ui_remote() just records the command; this runs it on the UI task (called from
// sleep_timer_cb) so it reuses the same playback paths as the on-screen buttons.
static void ui_remote_apply(ui_remote_t cmd, int arg)
{
    bool ss = source_sendspin_session_active();
    switch (cmd) {
    case UI_REMOTE_TOGGLE:
        // Pausing must always work; refuse only the resume direction while
        // parentally blocked (quiet hours or exhausted daily limit).
        if (ss) {
            if (source_sendspin_active()) source_sendspin_command(SENDSPIN_CMD_PAUSE);
            else if (play_denied()) break;
            else source_sendspin_command(SENDSPIN_CMD_PLAY);
        } else {
            if (audio_is_paused()) {
                if (play_denied()) break;
                audio_set_paused(false);
            } else {
                audio_set_paused(true);
            }
        }
        break;
    case UI_REMOTE_STOP:
        s_user_stopped = true;  // hide the mini bar at once on a web Stop too
        if (ss) source_sendspin_command(SENDSPIN_CMD_STOP);
        else    ui_stop();
        if (s_active_builder == build_now_playing || s_active_builder == build_sendspin_playing) {
            show(build_home);
        }
        break;
    case UI_REMOTE_NEXT:
        if (play_denied()) break;
        if (ss) source_sendspin_command(SENDSPIN_CMD_NEXT);
        else    play_ctx_at(s_play_index + 1);
        break;
    case UI_REMOTE_PREV:
        if (play_denied()) break;
        if (ss) source_sendspin_command(SENDSPIN_CMD_PREVIOUS);
        else    play_ctx_at(s_play_index - 1);
        break;
    case UI_REMOTE_VOLUME:
        if (arg < 0) arg = 0;
        if (arg > 100) arg = 100;
        audio_set_volume(arg);
        if (s_np_vol) lv_slider_set_value(s_np_vol, arg, LV_ANIM_OFF);  // reflect on screen
        break;
    case UI_REMOTE_PLAY_RADIO: {
        if (play_denied()) break;
        const config_t *c = config_store_get();
        if (c && arg >= 0 && (size_t)arg < c->webradio_count) {
            s_play_ctx = PLAY_CTX_NONE;  // a single stream, no next/previous
            ui_play(false, c->webradios[arg].url, c->webradios[arg].name, 0);
            show(build_now_playing);
        }
        break;
    }
    case UI_REMOTE_REFRESH_PODCASTS:
        // Same constraint as the on-device refresh: it runs on the shared worker,
        // so only start it when nothing is playing (the web endpoint already tells
        // the user to stop playback first).
        if (!audio_is_active() && !s_refreshing && !s_downloading) {
            s_refreshing = true;
            play_req_t req = { .kind = REQ_REFRESH_ALL };
            xQueueOverwrite(s_play_q, &req);
        }
        break;
    case UI_REMOTE_DOWNLOAD_PODCAST:    // arg = podcast id, fetch missing episodes
    case UI_REMOTE_REDOWNLOAD_PODCAST:  // arg = podcast id, re-fetch every episode
    case UI_REMOTE_DOWNLOAD_ALL:        // every podcast, fetch missing (refresh first)
    case UI_REMOTE_REDOWNLOAD_ALL: {    // every podcast, re-fetch every episode
        // Define a new persisted background job (overwrites any current one). The
        // scheduler in sleep_timer_cb starts it once the device is idle 5 min with
        // Wi-Fi up, and pauses it as soon as audio plays. If a worker is running,
        // ask it to stop so the new job takes over.
        bool all = (cmd == UI_REMOTE_DOWNLOAD_ALL || cmd == UI_REMOTE_REDOWNLOAD_ALL);
        bool force = (cmd == UI_REMOTE_REDOWNLOAD_PODCAST || cmd == UI_REMOTE_REDOWNLOAD_ALL);
        if (s_downloading) s_dl_cancel = true;
        memset(&s_dljob, 0, sizeof(s_dljob));
        s_dljob.active = true;
        s_dljob.scope_all = all;
        s_dljob.force = force;
        s_dljob.pod_id = all ? 0 : arg;
        dljob_persist();
        s_dl_done = 0;
        s_dl_total = 0;
        s_dl_phase = UI_DL_SCHEDULED;
        break;
    }
    case UI_REMOTE_CANCEL_DOWNLOAD:
        s_dljob.active = false;  // user cancel: do not auto-resume
        dljob_persist();
        s_dl_cancel = true;
        s_dl_phase = UI_DL_IDLE;
        break;
    case UI_REMOTE_PLAY_PATH: {
        if (play_denied()) break;
        if (!s_remote_path[0]) break;
        char path[16 + LIB_PATH_MAX];
        snprintf(path, sizeof(path), "/sdcard/%s", s_remote_path);
        const char *slash = strrchr(s_remote_path, '/');
        const char *title = s_remote_title[0] ? s_remote_title
                          : (slash ? slash + 1 : s_remote_path);
        s_play_ctx = PLAY_CTX_NONE;  // a single track, no next/previous
        s_np_dur_override_ms = 0;
        ui_play(true, path, title, 0);
        show(build_now_playing);
        break;
    }
    case UI_REMOTE_SLEEP: {
        // Accept arbitrary 1..180 minute values (the web select only offers the
        // fixed steps, but this lets a short bench test use e.g. 1 minute).
        int choice = arg;
        if (choice > 180) choice = 180;
        if (choice < 0) choice = -1;
        sleep_arm(choice);
        sleep_label_refresh();  // reflect on an open now-playing screen at once
        break;
    }
    case UI_REMOTE_SEEK:
        // Ungated like VOLUME (it does not start playback). Clamp against the
        // decoder's duration, not the RSS override: a client working from an
        // overridden dur_ms must not seek past the file (same choice as the
        // on-device on_seek_released). An open now-playing screen follows via
        // its periodic decode_progress refresh; nothing to update here.
        if (now_playing_seekable()) {
            uint32_t pos = 0, dur = 0;
            decode_progress(&pos, &dur);
            if (dur > 0) {
                uint32_t t = arg < 0 ? 0 : (uint32_t)arg;
                decode_seek(t > dur ? dur : t);
            }
        }
        break;
    }
}

void ui_remote(ui_remote_t cmd, int arg)
{
    s_remote_arg = arg;
    s_remote_cmd = (int)cmd;  // picked up by sleep_timer_cb on the UI task
}

void ui_remote_play_path(const char *rel_path, const char *title)
{
    strlcpy(s_remote_path, rel_path, sizeof(s_remote_path));
    strlcpy(s_remote_title, title ? title : "", sizeof(s_remote_title));
    s_remote_arg = 0;
    s_remote_cmd = (int)UI_REMOTE_PLAY_PATH;
}

bool ui_podcast_refreshing(void)
{
    return s_refreshing;
}

// Request a listening-stats reset from the web task. Deferred to sleep_timer_cb
// so the clear runs on the LVGL task, the single owner of the stats RAM+file.
void ui_stats_reset(void)
{
    s_stats_reset_req = true;
}

void ui_set_ghota_check_fn(ui_ghota_check_fn_t fn)
{
    s_ghota_check_fn = fn;
}

void ui_download_status(ui_dl_status_t *out)
{
    if (!out) return;
    out->pending = s_dljob.active;
    out->active  = s_downloading;
    out->done    = s_dl_done;
    out->total   = s_dl_total;
    if (s_downloading) {
        out->phase = s_dl_phase;  // REFRESHING or DOWNLOADING
    } else if (s_dljob.active) {
        out->phase = audio_is_active() ? UI_DL_PAUSED : UI_DL_SCHEDULED;
    } else {
        // Keep reporting SDFULL after the job stopped on a full card, until a new job.
        out->phase = (s_dl_phase == UI_DL_SDFULL) ? UI_DL_SDFULL : UI_DL_IDLE;
    }
}

void ui_status(ui_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->volume = audio_get_volume();
    // Sleep timer (A2): 0 = off, -1 = end-of-track, else minutes left. Computed
    // once here so every return path below (sendspin, idle, or a local source)
    // reports it the same way.
    if (s_sleep_choice == 0) {
        out->sleep_min = 0;
    } else if (s_sleep_end_of_track) {
        out->sleep_min = -1;
    } else {
        int64_t remain_us = s_sleep_stop_at_us - esp_timer_get_time();
        out->sleep_min = remain_us > 0 ? (int)((remain_us + 59999999) / 60000000) : 0;
    }
    if (source_sendspin_session_active()) {
        out->active = true;
        out->paused = !source_sendspin_active();
        strlcpy(out->source, "sendspin", sizeof(out->source));
        source_sendspin_title(out->title, sizeof(out->title));
        source_sendspin_artist(out->artist, sizeof(out->artist));
        return;
    }
    if (!audio_is_active()) {
        strlcpy(out->source, "none", sizeof(out->source));
        return;
    }
    out->active = true;
    out->paused = audio_is_paused();
    if (s_play_ctx == PLAY_CTX_SD) {
        strlcpy(out->source, "sd", sizeof(out->source));
        strlcpy(out->title, s_meta_title[0] ? s_meta_title : s_now_title, sizeof(out->title));
        strlcpy(out->artist, s_meta_artist, sizeof(out->artist));
    } else if (s_play_ctx == PLAY_CTX_PODCAST) {
        strlcpy(out->source, "podcast", sizeof(out->source));
        strlcpy(out->title, s_now_title, sizeof(out->title));
    } else {
        strlcpy(out->source, "radio", sizeof(out->source));
        strlcpy(out->title, s_now_title, sizeof(out->title));
        source_stream_title(out->artist, sizeof(out->artist));  // ICY "now playing"
    }
    uint32_t pos = 0, dur = 0;
    decode_progress(&pos, &dur);
    if (s_np_dur_override_ms > 0) dur = s_np_dur_override_ms;
    out->pos_ms = pos;
    out->dur_ms = dur;
    out->seekable = now_playing_seekable();  // sendspin/idle returned above: false there
}

// Classify the currently playing source for listening stats (C3), and its
// display title. Returns false when nothing countable is playing (idle, paused,
// or the alarm beep). It mirrors ui_status() with one deliberate deviation: a
// web-remote path play (POST /api/playback path) and a favorite SD track run
// with PLAY_CTX_NONE, so we ask the audio arbiter to tell an SD file apart from
// a radio stream. Without it those tracks would count as radio (see the C3
// report). PLAY_CTX_PODCAST is checked before the arbiter because a cached
// podcast plays from SD (arbiter == AUDIO_SOURCE_SD) yet is still a podcast.
static bool stats_classify(stats_source_t *src, const char **title)
{
    if (audio_arbiter_active() == AUDIO_SOURCE_BEEP) return false;  // alarm beep: not listening
    // A voice memo is a seconds-long message, not listening; counting it would
    // also misclassify (s_play_ctx still holds the previous session's context).
    if (audio_arbiter_active() == AUDIO_SOURCE_MEMO) return false;
    if (source_sendspin_session_active()) {
        if (!source_sendspin_active()) return false;  // paused
        static char t[64];
        source_sendspin_title(t, sizeof(t));
        *src = STATS_SRC_SENDSPIN;
        *title = t;
        return true;
    }
    if (!audio_is_active() || audio_is_paused()) return false;
    if (s_play_ctx == PLAY_CTX_PODCAST) {
        *src = STATS_SRC_PODCAST;
        *title = s_now_title;
    } else if (s_play_ctx == PLAY_CTX_SD || s_play_ctx == PLAY_CTX_LIBRARY) {
        *src = STATS_SRC_SD;
        *title = s_meta_title[0] ? s_meta_title : s_now_title;
    } else {  // PLAY_CTX_NONE: web-path / favorite SD track, or a radio stream
        if (audio_arbiter_active() == AUDIO_SOURCE_SD) {
            *src = STATS_SRC_SD;
            *title = s_meta_title[0] ? s_meta_title : s_now_title;
        } else {
            *src = STATS_SRC_RADIO;
            *title = s_now_title;
        }
    }
    return true;
}

// Keep in sync with format_from_path() in source_sd.c (the actual decode gate).
static bool sd_playable(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".flac") ||
                   !strcasecmp(dot, ".m4a") || !strcasecmp(dot, ".aac") ||
                   !strcasecmp(dot, ".mp4"));
}

static void sd_dir_push(const char *name)
{
    char tmp[SD_DIR_MAX + SOURCE_SD_NAME_MAX + 2];
    if (s_sd_dir[0]) snprintf(tmp, sizeof(tmp), "%s/%s", s_sd_dir, name);
    else            snprintf(tmp, sizeof(tmp), "%s", name);
    strlcpy(s_sd_dir, tmp, sizeof(s_sd_dir));  // capped at SD_DIR_MAX (deep paths truncate)
}

static void sd_dir_pop(void)
{
    char *slash = strrchr(s_sd_dir, '/');
    if (slash) *slash = '\0';
    else       s_sd_dir[0] = '\0';
}

static void on_sd_file(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(btn);
    s_play_ctx = PLAY_CTX_SD;
    // Snapshot the current folder + its playable files so navigation later stays
    // on this folder even if the user browses elsewhere.
    if (!s_play_sd_names) {
        s_play_sd_names = heap_caps_malloc(sizeof(*s_play_sd_names) * SD_LIST_MAX, MALLOC_CAP_SPIRAM);
    }
    if (s_play_sd_names) {
        memcpy(s_play_sd_names, s_sd_names, sizeof(*s_play_sd_names) * s_sd_count);
        s_play_sd_count = s_sd_count;
        strlcpy(s_play_sd_dir, s_sd_dir, sizeof(s_play_sd_dir));
    }
    play_ctx_at(i);
}

static void on_sd_dir(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (s_sd_entries && idx >= 0 && (size_t)idx < s_sd_entry_count) {
        sd_dir_push(s_sd_entries[idx].name);
        show(build_sd);
    }
}

static void on_sd_up(lv_event_t *e) { (void)e; sd_dir_pop(); show(build_sd); }

static void build_sd(lv_obj_t *scr)
{
    add_back_button(scr);
    add_title_wide(scr, s_sd_dir[0] ? s_sd_dir : T(STR_SDCARD));
    if (!source_sd_present()) {
        lv_obj_t *l = lv_label_create(scr);
        lv_label_set_text(l, T(STR_NO_SD));
        lv_obj_center(l);
        return;
    }
    if (!s_sd_entries) {
        s_sd_entries = heap_caps_malloc(sizeof(*s_sd_entries) * SD_BROWSE_MAX, MALLOC_CAP_SPIRAM);
    }
    s_sd_entry_count = 0;
    if (s_sd_entries) {
        // No sizes: this runs on the LVGL task, and the per-file stat() would
        // freeze rendering and touch for the whole walk of a large folder.
        source_sd_browse(s_sd_dir, s_sd_entries, SD_BROWSE_MAX, &s_sd_entry_count, false);
    }

    lv_obj_t *list = lv_list_create(scr);
    lv_obj_set_size(list, scr_w(), scr_h() - 56);  // below the 44 px round back button (8+44)
    lv_obj_set_style_pad_bottom(list, MINI_CLEAR, 0);  // last row scrolls clear of the floating mini bar
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);

    if (s_sd_dir[0]) {  // a way back to the parent folder
        lv_obj_t *up = lv_list_add_button(list, LV_SYMBOL_LEFT, "..");
        lv_obj_add_event_cb(up, on_sd_up, LV_EVENT_CLICKED, NULL);
    }
    // Folders first (tap to enter).
    for (size_t i = 0; i < s_sd_entry_count; i++) {
        if (!s_sd_entries[i].is_dir) continue;
        lv_obj_t *btn = lv_list_add_button(list, LV_SYMBOL_DIRECTORY, s_sd_entries[i].name);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, on_sd_dir, LV_EVENT_CLICKED, NULL);
    }
    // Then playable files, collected into s_sd_names for playback/auto-advance.
    s_sd_count = 0;
    size_t shown = 0;
    for (size_t i = 0; i < s_sd_entry_count && s_sd_count < SD_LIST_MAX; i++) {
        if (s_sd_entries[i].is_dir) { shown++; continue; }
        if (!sd_playable(s_sd_entries[i].name)) continue;
        strlcpy(s_sd_names[s_sd_count], s_sd_entries[i].name, SOURCE_SD_NAME_MAX);
        lv_obj_t *btn = lv_list_add_button(list, LV_SYMBOL_FILE, s_sd_names[s_sd_count]);
        lv_obj_set_user_data(btn, (void *)(intptr_t)s_sd_count);
        lv_obj_add_event_cb(btn, on_sd_file, LV_EVENT_CLICKED, NULL);
        s_sd_count++;
        shown++;
    }
    if (shown == 0) {
        // Nothing visible (no subfolders, no playable files): say so instead of
        // leaving a blank frame.
        lv_list_add_text(list, T(STR_EMPTY_FOLDER));
    }
    list_titles_static(list);
}

// Library (browse by artist/album) ----------------------------------------

static void build_library(lv_obj_t *scr);          // chooser: By artist / By album
static void build_library_artists(lv_obj_t *scr);
static void build_library_albums(lv_obj_t *scr);     // albums of s_lib_artist
static void build_library_albums_all(lv_obj_t *scr); // every album
static void build_library_tracks(lv_obj_t *scr);

static char s_lib_albumartists[LIB_DISP_MAX][LIB_NAME_MAX];  // artist of each album in the "By album" list
static bool s_lib_from_album;  // tracks screen reached via "By album" (back goes there, not to an artist)

// A back button wired to a specific screen (drill-down levels go up, not home).
// Same round style and position as add_back_button, just a different target.
static void add_back_cb(lv_obj_t *scr, lv_event_cb_t cb)
{
    lv_obj_t *b = make_round_btn(scr, LV_SYMBOL_LEFT, 44, false, cb);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 8, 8);
}

static bool lib_alloc(void)
{
    if (!s_lib_titles)      s_lib_titles      = heap_caps_malloc(LIB_DISP_MAX * LIB_NAME_MAX, MALLOC_CAP_SPIRAM);
    if (!s_lib_paths)       s_lib_paths       = heap_caps_malloc(LIB_DISP_MAX * LIB_PATH_MAX, MALLOC_CAP_SPIRAM);
    if (!s_play_lib_titles) s_play_lib_titles = heap_caps_malloc(LIB_DISP_MAX * LIB_NAME_MAX, MALLOC_CAP_SPIRAM);
    if (!s_play_lib_paths)  s_play_lib_paths  = heap_caps_malloc(LIB_DISP_MAX * LIB_PATH_MAX, MALLOC_CAP_SPIRAM);
    return s_lib_titles && s_lib_paths && s_play_lib_titles && s_play_lib_paths;
}

static void on_lib_back_chooser(lv_event_t *e) { (void)e; show(build_library); }
static void on_lib_back_artists(lv_event_t *e) { (void)e; show(build_library_artists); }
// Tracks back: return to whichever album list we came from.
static void on_lib_back_albums(lv_event_t *e)
{
    (void)e;
    show(s_lib_from_album ? build_library_albums_all : build_library_albums);
}

static void on_lib_by_artist(lv_event_t *e) { (void)e; show(build_library_artists); }
static void on_lib_by_album(lv_event_t *e)  { (void)e; show(build_library_albums_all); }

static void on_lib_artist(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (i >= 0 && (size_t)i < s_lib_name_count) {
        strlcpy(s_lib_artist, s_lib_names[i], sizeof(s_lib_artist));
        show(build_library_albums);
    }
}

// From an artist's album list.
static void on_lib_album(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (i >= 0 && (size_t)i < s_lib_name_count) {
        strlcpy(s_lib_album, s_lib_names[i], sizeof(s_lib_album));
        s_lib_from_album = false;
        show(build_library_tracks);
    }
}

// From the all-albums list (carries the album's own artist).
static void on_lib_album_all(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (i >= 0 && (size_t)i < s_lib_name_count) {
        strlcpy(s_lib_album, s_lib_names[i], sizeof(s_lib_album));
        strlcpy(s_lib_artist, s_lib_albumartists[i], sizeof(s_lib_artist));
        s_lib_from_album = true;
        show(build_library_tracks);
    }
}

static void on_lib_track(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (!lib_alloc() || i < 0 || (size_t)i >= s_lib_track_count) return;
    // Snapshot the displayed album as the playing list (isolated from browsing).
    memcpy(s_play_lib_titles, s_lib_titles, (size_t)LIB_NAME_MAX * s_lib_track_count);
    memcpy(s_play_lib_paths,  s_lib_paths,  (size_t)LIB_PATH_MAX * s_lib_track_count);
    s_play_lib_count = s_lib_track_count;
    s_play_ctx = PLAY_CTX_LIBRARY;
    play_ctx_at(i);
}

// Render a list of s_lib_names (artists or albums) as tappable buttons.
static void lib_list_names(lv_obj_t *scr, lv_event_cb_t cb)
{
    lv_obj_t *list = lv_list_create(scr);
    lv_obj_set_size(list, scr_w(), scr_h() - 56);  // below the 44 px round back button (8+44)
    lv_obj_set_style_pad_bottom(list, MINI_CLEAR, 0);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    for (size_t i = 0; i < s_lib_name_count; i++) {
        lv_obj_t *btn = lv_list_add_button(list, LV_SYMBOL_AUDIO, s_lib_names[i]);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }
    list_titles_static(list);
}

// Top of the Library: choose how to browse.
static void build_library(lv_obj_t *scr)
{
    add_back_button(scr);  // back to home
    add_title_wide(scr, T(STR_LIBRARY));
    if (!source_sd_present()) {
        lv_obj_t *l = lv_label_create(scr);
        lv_label_set_text(l, T(STR_NO_SD));
        lv_obj_center(l);
        return;
    }
    if (library_track_count() == 0) library_load();  // lazy-load the index
    if (library_track_count() == 0) {
        empty_state_label(scr, STR_EMPTY_LIBRARY);
        return;
    }
    // Two choices as tiles, matching the home screen's look. Reuses the home
    // grid's own margin/gutter/top-offset constants (see build_home). Landscape:
    // two full-width horizontal chips stacked, filling the content band.
    // Portrait: a single row of two vertical tiles, same height as a home 2x2
    // grid row; centered in the band (top-aligned left a large empty strip below).
    const bool ls = scr_w() > scr_h();
    const int M = PAD_SIDE, G = 10, Y0 = 54;
    const int limit = scr_h() - MINI_CLEAR;
    if (ls) {
        const int w = scr_w() - 2 * M;
        const int h = (limit - Y0 - G) / 2;
        make_tile(scr, LV_SYMBOL_AUDIO, STR_BY_ARTIST, M, Y0, w, h, true, on_lib_by_artist);
        make_tile(scr, LV_SYMBOL_LIST, STR_BY_ALBUM, M, Y0 + h + G, w, h, true, on_lib_by_album);
    } else {
        const int w2 = (scr_w() - 2 * M - G) / 2;
        const int h2 = (limit - Y0 - G) / 2;
        const int y = Y0 + (limit - Y0 - h2) / 2;
        make_tile(scr, LV_SYMBOL_AUDIO, STR_BY_ARTIST, M, y, w2, h2, false, on_lib_by_artist);
        make_tile(scr, LV_SYMBOL_LIST, STR_BY_ALBUM, M + w2 + G, y, w2, h2, false, on_lib_by_album);
    }
}

static void build_library_artists(lv_obj_t *scr)
{
    add_back_cb(scr, on_lib_back_chooser);
    add_title_wide(scr, T(STR_ARTISTS));
    s_lib_name_count = library_artists(s_lib_names, LIB_DISP_MAX);
    lib_list_names(scr, on_lib_artist);
}

static void build_library_albums(lv_obj_t *scr)
{
    add_back_cb(scr, on_lib_back_artists);
    add_title_wide(scr, s_lib_artist);
    s_lib_name_count = library_albums(s_lib_artist, s_lib_names, LIB_DISP_MAX);
    lib_list_names(scr, on_lib_album);
}

static void build_library_albums_all(lv_obj_t *scr)
{
    add_back_cb(scr, on_lib_back_chooser);
    add_title_wide(scr, T(STR_ALBUMS));
    s_lib_name_count = library_all_albums(s_lib_names, s_lib_albumartists, LIB_DISP_MAX);
    lib_list_names(scr, on_lib_album_all);
}

static void build_library_tracks(lv_obj_t *scr)
{
    add_back_cb(scr, on_lib_back_albums);
    add_title_wide(scr, s_lib_album);
    if (!lib_alloc()) return;
    s_lib_track_count = library_album_tracks(s_lib_artist, s_lib_album,
                                             s_lib_titles, s_lib_paths, LIB_DISP_MAX);
    lv_obj_t *list = lv_list_create(scr);
    lv_obj_set_size(list, scr_w(), scr_h() - 56);  // below the 44 px round back button (8+44)
    lv_obj_set_style_pad_bottom(list, MINI_CLEAR, 0);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    for (size_t i = 0; i < s_lib_track_count; i++) {
        lv_obj_t *btn = lv_list_add_button(list, LV_SYMBOL_FILE, s_lib_titles[i]);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, on_lib_track, LV_EVENT_CLICKED, NULL);
    }
    list_titles_static(list);
}

static void on_open_library(lv_event_t *e) { (void)e; show(build_library); }

// Podcast episodes --------------------------------------------------------

static void on_episode(lv_event_t *e)
{
    // The episode list is an lv_table (virtualized, so it stays smooth with
    // hundreds of episodes). The tapped row is the selected cell.
    lv_obj_t *tbl = lv_event_get_target(e);
    uint32_t row = LV_TABLE_CELL_NONE, col = 0;
    lv_table_get_selected_cell(tbl, &row, &col);
    if (row == LV_TABLE_CELL_NONE || (size_t)row >= s_ep_count) return;
    int i = (int)row;
    // A non-downloaded episode can only stream: refuse it while offline (the
    // row is drawn greyed out, see ep_table_draw_cb) and say why.
    if (!s_episodes[i].cached && net_state() != NET_STATE_CONNECTED) {
        if (s_ep_msg) lv_label_set_text(s_ep_msg, T(STR_WAITING_WIFI));
        return;
    }
    s_play_ctx = PLAY_CTX_PODCAST;
    // Snapshot the episode list shown here (the podcast being opened) so later
    // navigation stays on this podcast even if the user browses elsewhere.
    if (ensure_eps(&s_play_eps, &s_play_eps_cap, s_ep_count)) {
        memcpy(s_play_eps, s_episodes, sizeof(*s_play_eps) * s_ep_count);
        s_play_ep_count = s_ep_count;
    }
    // Remember this podcast's intro skip so streamed episodes skip it at playback.
    s_play_podcast_skip_s = 0;
    const config_t *c = config_store_get();
    for (size_t k = 0; c && k < c->podcast_count; k++) {
        if (c->podcasts[k].id == s_nav_podcast_id) { s_play_podcast_skip_s = c->podcasts[k].skip_seconds; break; }
    }
    s_play_podcast_id = s_nav_podcast_id;  // for the resume record and its lookup
    play_ctx_at(i);
}

// Fetch + parse the RSS feed on the worker task. The episodes screen reloads
// itself when the worker finishes (see sleep_timer_cb).
static void on_refresh(lv_event_t *e)
{
    (void)e;
    // The refresh runs on the same worker as playback, which blocks for the whole
    // track (a live web radio never ends), so a refresh requested mid-playback
    // would queue behind it and hang on T(STR_REFRESHING). Refuse it while audio
    // plays and say why, instead of an inert button (feeds also refresh
    // automatically when idle and from the web page).
    if (audio_is_active()) {
        if (s_ep_msg) lv_label_set_text(s_ep_msg, T(STR_STOP_PLAYBACK_FIRST));
        return;
    }
    if (net_state() != NET_STATE_CONNECTED) {  // an RSS fetch needs the network
        if (s_ep_msg) lv_label_set_text(s_ep_msg, T(STR_WAITING_WIFI));
        return;
    }
    if (s_refreshing || s_downloading || s_nav_podcast_rss[0] == '\0') {
        // s_downloading: the worker is busy for the whole download, so a queued
        // REQ_REFRESH would sit behind it and could be clobbered by a later
        // REQ_PLAY, leaving s_refreshing stuck true forever (matches the web path).
        return;
    }
    s_refreshing = true;
    play_req_t req = { .kind = REQ_REFRESH, .id = s_nav_podcast_id };
    strlcpy(req.target, s_nav_podcast_rss, sizeof(req.target));
    xQueueOverwrite(s_play_q, &req);
    show(build_episodes);  // re-render to show the T(STR_REFRESHING) state
}

static void on_episodes_back(lv_event_t *e) { (void)e; show(build_podcasts); }

// Per-row styling: grey out episodes that cannot play right now (not
// downloaded and no Wi-Fi), muted text for already-played episodes (A3).
// Offline grey wins over played styling for a non-cached row (it still
// cannot play). Per-row table styling goes through the draw-task event: id1
// is the row index (set by lv_table's draw code).
static void ep_table_draw_cb(lv_event_t *e)
{
    lv_draw_task_t *t = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(t);
    if (!base || base->part != LV_PART_ITEMS) return;
    uint32_t row = base->id1;
    if (row >= s_ep_count) return;
    bool offline_grey = net_state() != NET_STATE_CONNECTED && !s_episodes[row].cached;
    bool played = !offline_grey && played_contains(s_episodes[row].episode_url);
    if (!offline_grey && !played) return;
    lv_draw_label_dsc_t *label = lv_draw_task_get_label_dsc(t);
    if (!label) return;
    label->color = offline_grey ? lv_palette_main(LV_PALETTE_GREY) : col_muted();
}

static void build_episodes(lv_obj_t *scr)
{
    add_back_cb(scr, on_episodes_back);  // back to the podcast list, not home
    add_title(scr, T(STR_EPISODES));

    // Refresh icon, top-right, same round style/size as the back button.
    lv_obj_t *rb = make_round_btn(scr, LV_SYMBOL_REFRESH, 44, false, on_refresh);
    lv_obj_align(rb, LV_ALIGN_TOP_RIGHT, -8, 8);

    // Status line for refresh outcomes ("stop playback first", "refresh failed"),
    // over the bottom of the list, empty until something needs saying.
    lv_obj_t *msg = lv_label_create(scr);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, scr_w() - 12);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(msg, lv_palette_main(LV_PALETTE_RED), 0);
    lv_label_set_text(msg, "");
    lv_obj_align(msg, LV_ALIGN_BOTTOM_MID, 0, -MINI_CLEAR);  // clears the mini bar
    s_ep_msg = msg;
    if (s_refreshing) {
        lv_obj_add_state(rb, LV_STATE_DISABLED);
        empty_state_label(scr, STR_REFRESHING_FEED);
        return;
    }

    s_ep_count = 0;
    size_t total = podcast_manifest_count(s_nav_podcast_id);
    if (total && ensure_eps(&s_episodes, &s_episodes_cap, total)) {
        podcast_read_manifest(s_nav_podcast_id, s_episodes, s_episodes_cap, &s_ep_count);
    }
    if (s_ep_count == 0) {
        empty_state_label(scr, STR_NO_EPISODES);
        return;
    }
    // An lv_table, not an lv_list: a list creates one object per row, which makes
    // opening slow and scrolling laggy once a feed has 100+ episodes. A table only
    // draws the visible rows, so it stays fast at any episode count.
    lv_obj_t *list = lv_table_create(scr);
    lv_obj_set_size(list, scr_w(), scr_h() - 56);  // below the 44 px round back button (8+44)
    lv_obj_set_style_pad_bottom(list, MINI_CLEAR, 0);  // last row scrolls clear of the floating mini bar
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_table_set_column_count(list, 1);
    lv_table_set_column_width(list, 0, scr_w());
    lv_table_set_row_count(list, s_ep_count);
    for (size_t i = 0; i < s_ep_count; i++) {
        // A3: an already-played episode shows OK instead of its cached/download icon.
        const char *icon = played_contains(s_episodes[i].episode_url) ? LV_SYMBOL_OK
                          : s_episodes[i].cached ? LV_SYMBOL_SD_CARD : LV_SYMBOL_DOWNLOAD;
        char line[PODCAST_TITLE_MAX + 8];
        snprintf(line, sizeof(line), "%s %s", icon, s_episodes[i].title);
        lv_table_set_cell_value(list, i, 0, line);
        // No TEXT_CROP: long titles wrap onto multiple lines. The table caches each
        // row's height once, so the wrap does not bring back the scroll lag.
    }
    // VALUE_CHANGED (not CLICKED): lv_table sends it on a tap-release that was not a
    // scroll, while the selected cell is still set (CLICKED fires after the table has
    // already cleared the selection, so the row would read as NONE).
    lv_obj_add_event_cb(list, on_episode, LV_EVENT_VALUE_CHANGED, NULL);
    // Offline greying of non-downloaded rows (no-op while connected).
    lv_obj_add_event_cb(list, ep_table_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_move_foreground(msg);  // the status line draws over the table
}

// Podcasts ----------------------------------------------------------------

static void on_podcast(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(btn);
    const config_t *c = config_store_get();
    if (c && i >= 0 && (size_t)i < c->podcast_count) {
        s_nav_podcast_id = c->podcasts[i].id;
        strlcpy(s_nav_podcast_rss, c->podcasts[i].rss_url, sizeof(s_nav_podcast_rss));
        show(build_episodes);
    }
}

static void build_podcasts(lv_obj_t *scr)
{
    add_back_button(scr);
    add_title_wide(scr, T(STR_PODCASTS));
    const config_t *c = config_store_get();
    if (!c || c->podcast_count == 0) {
        empty_state_label(scr, STR_NO_PODCASTS);
        return;
    }
    lv_obj_t *list = lv_list_create(scr);
    lv_obj_set_size(list, scr_w(), scr_h() - 56);  // below the 44 px round back button (8+44)
    lv_obj_set_style_pad_bottom(list, MINI_CLEAR, 0);  // last row scrolls clear of the floating mini bar
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    for (size_t i = 0; c && i < c->podcast_count; i++) {
        lv_obj_t *btn = lv_list_add_button(list, LV_SYMBOL_LIST, c->podcasts[i].title);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, on_podcast, LV_EVENT_CLICKED, NULL);
    }
    list_titles_static(list);
}

// Home --------------------------------------------------------------------

// Web radios and podcasts need the network; ignore taps until connected.
static void on_open_webradios(lv_event_t *e) { (void)e; if (net_state() == NET_STATE_CONNECTED) show(build_webradios); }
// Multiplication tables game ----------------------------------------------
// build_game_setup lets the child pick which tables (1..10) this session
// drills (s_game_tables, a bitmask, RAM only: resets to "none" every time the
// tile is opened, never persisted; the child checks tables individually or
// taps "All"). One operand is drawn from that set, the
// other stays 1..10, and the child types the product on a keypad. +10 points
// on the first try, +5 on the second, otherwise the answer is shown and the
// game moves on after a pause. Endless; the back button saves the score to
// NVS when it beats the stored high score.

static int      s_game_a, s_game_b;   // current operands, 0 = no question yet
static int      s_game_attempt;       // 0 = first try, 1 = second try
static bool     s_game_reveal;        // feedback shown, keypad input ignored
static int      s_game_error_count;   // how many times the tool gave the answer
static int      s_game_streak;        // consecutive correct answers on first attempt
static int      s_game_max_streak;    // highest streak ever achieved
static uint32_t s_game_score;
static uint32_t s_game_best;          // loaded on entry, follows a beaten score
static char     s_game_input[4];      // typed digits
static lv_obj_t *s_game_q_lbl, *s_game_fb_lbl, *s_game_score_lbl;
static lv_timer_t *s_game_timer;      // one-shot advance to the next question
static uint16_t s_game_tables;        // bit i set = table (i+1) enabled; RAM only, not persisted
static lv_obj_t *s_game_tbl_btn[10];  // table-picker chips, build_game_setup only
#define GAME_TABLES_ALL 0x03FF        // all 10 tables enabled

static void game_refresh(void)
{
    if (!s_game_q_lbl || !s_game_score_lbl) return;
    char q[40];
    snprintf(q, sizeof(q), "%d × %d = %s", s_game_a, s_game_b,
             s_game_input[0] ? s_game_input : "?");
    lv_label_set_text(s_game_q_lbl, q);
    char sc[32], best[32], streak[32], max_streak[32], line[160];
    snprintf(sc, sizeof(sc), T(STR_SCORE_FMT), (unsigned)s_game_score);
    snprintf(best, sizeof(best), T(STR_BEST_FMT), (unsigned)s_game_best);
    snprintf(streak, sizeof(streak), T(STR_STREAK_FMT), s_game_streak);
    snprintf(max_streak, sizeof(max_streak), T(STR_MAX_STREAK_FMT), s_game_max_streak);
    if (scr_w() > scr_h()) {
        // Landscape stacks the header on multiple lines in the narrow left column.
        snprintf(line, sizeof(line), "%s\n%s\n%s\n%s", sc, best, streak, max_streak);
    } else {
        // Portrait fits two elements per line.
        snprintf(line, sizeof(line), "%s   %s\n%s   %s", sc, best, streak, max_streak);
    }
    lv_label_set_text(s_game_score_lbl, line);
}

static void game_new_question(void)
{
    // The "table" operand comes from the chosen set (bit i = table i+1); the
    // other operand stays 1..10, as in a classic times-table drill.
    int tbl[10], n = 0;
    for (int i = 0; i < 10; i++) if (s_game_tables & (1 << i)) tbl[n++] = i + 1;
    if (n == 0) { for (int i = 0; i < 10; i++) tbl[n++] = i + 1; }  // guard: nothing chosen
    int a = tbl[esp_random() % n];
    int b = 1 + (int)(esp_random() % 10);
    if (a == s_game_a && b == s_game_b) a = tbl[esp_random() % n];  // reroll once
    s_game_a = a;
    s_game_b = b;
    s_game_attempt = 0;
    s_game_reveal = false;
    s_game_input[0] = '\0';
    if (s_game_fb_lbl) lv_label_set_text(s_game_fb_lbl, "");
    game_refresh();
}

static void game_advance_cb(lv_timer_t *t)
{
    (void)t;
    s_game_timer = NULL;  // one-shot: LVGL deletes it after this run
    if (s_active_builder == build_game) game_new_question();
}

static void game_arm_advance(uint32_t ms)
{
    if (s_game_timer) lv_timer_delete(s_game_timer);
    s_game_timer = lv_timer_create(game_advance_cb, ms, NULL);
    lv_timer_set_repeat_count(s_game_timer, 1);
}

static void on_game_key(lv_event_t *e)
{
    if (s_game_reveal) return;  // between questions: ignore input
    lv_obj_t *km = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(km);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *txt = lv_buttonmatrix_get_button_text(km, id);
    if (!txt) return;

    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        size_t n = strlen(s_game_input);
        if (n) s_game_input[n - 1] = '\0';
        game_refresh();
        return;
    }
    if (strcmp(txt, LV_SYMBOL_OK) == 0) {
        if (!s_game_input[0]) return;
        char fb[80];
        if (atoi(s_game_input) == s_game_a * s_game_b) {
            int pts = (s_game_attempt == 0) ? 10 : 5;
            s_game_score += pts;
            if (s_game_attempt == 0) {
                s_game_streak++;
                if (s_game_streak > s_game_max_streak) {
                    s_game_max_streak = s_game_streak;
                }
            } else {
                s_game_streak = 0;
            }
            snprintf(fb, sizeof(fb), T(STR_CORRECT_FMT), pts);
            if (s_game_score > s_game_best) {
                s_game_best = s_game_score;  // header follows live; NVS on back
                snprintf(fb + strlen(fb), sizeof(fb) - strlen(fb), "  %s", T(STR_NEW_BEST));
            }
            lv_obj_set_style_text_color(s_game_fb_lbl, lv_palette_main(LV_PALETTE_GREEN), 0);
            lv_label_set_text(s_game_fb_lbl, fb);
            s_game_reveal = true;
            game_refresh();
            game_arm_advance(900);
        } else if (s_game_attempt == 0) {
            s_game_attempt = 1;
            s_game_streak = 0;
            s_game_input[0] = '\0';
            lv_obj_set_style_text_color(s_game_fb_lbl, lv_palette_main(LV_PALETTE_ORANGE), 0);
            lv_label_set_text(s_game_fb_lbl, T(STR_TRY_AGAIN));
            game_refresh();
        } else {
            s_game_reveal = true;
            s_game_streak = 0;
            snprintf(fb, sizeof(fb), "%d × %d = %d", s_game_a, s_game_b, s_game_a * s_game_b);
            lv_obj_set_style_text_color(s_game_fb_lbl, lv_palette_main(LV_PALETTE_RED), 0);
            lv_label_set_text(s_game_fb_lbl, fb);
            
            s_game_error_count++;
            if (s_game_error_count > 3) {
                if (s_game_best > config_store_get_highscore()) {
                    config_store_set_highscore(s_game_best);
                }
                if (s_game_max_streak > config_store_get_maxstreak()) {
                    config_store_set_maxstreak(s_game_max_streak);
                }
                s_game_score = 0;
                s_game_error_count = 0;
                snprintf(fb + strlen(fb), sizeof(fb) - strlen(fb), "  %s", T(STR_RESET_MSG));
                lv_label_set_text(s_game_fb_lbl, fb);
                game_refresh();
            }
            game_arm_advance(3000);
        }
        return;
    }
    if (strlen(s_game_input) < 3 && txt[0] >= '0' && txt[0] <= '9') {
        size_t n = strlen(s_game_input);
        s_game_input[n] = txt[0];
        s_game_input[n + 1] = '\0';
        game_refresh();
    }
}

static void on_game_back(lv_event_t *e)
{
    (void)e;
    if (s_game_timer) { lv_timer_delete(s_game_timer); s_game_timer = NULL; }
    // Persist a beaten high score. NVS write from the LVGL task is safe
    // (internal stack).
    if (s_game_best > config_store_get_highscore()) {
        config_store_set_highscore(s_game_best);
    }
    if (s_game_max_streak > config_store_get_maxstreak()) {
        config_store_set_maxstreak(s_game_max_streak);
    }
    show(build_home);
}

// Reset the session here, NOT in build_game: rebuilds of the open screen
// (orientation or language change) must keep the running game intact.
static void on_open_game(lv_event_t *e)
{
    (void)e;
    if (play_denied()) return;
    if (s_game_timer) { lv_timer_delete(s_game_timer); s_game_timer = NULL; }
    s_game_score = 0;
    s_game_error_count = 0;
    s_game_streak = 0;
    s_game_best = config_store_get_highscore();
    s_game_max_streak = config_store_get_maxstreak();
    s_game_tables = 0;  // every session starts with nothing checked; the child picks
    s_game_a = 0;  // force a fresh question in build_game
    show(build_game_setup);
}

// Table-picker chip: reused for the "toggle one" tap handler and for the
// "All"/"None" bulk handlers, which just re-derive every chip's checked state.
static void game_set_chip(int i, bool checked)
{
    if (!s_game_tbl_btn[i]) return;
    if (checked) lv_obj_add_state(s_game_tbl_btn[i], LV_STATE_CHECKED);
    else lv_obj_remove_state(s_game_tbl_btn[i], LV_STATE_CHECKED);
}

static void on_game_table(lv_event_t *e)
{
    lv_obj_t *chip = lv_event_get_target(e);
    int i = 0;
    for (; i < 10; i++) if (s_game_tbl_btn[i] == chip) break;
    if (i == 10) return;
    if (lv_obj_has_state(chip, LV_STATE_CHECKED)) s_game_tables |= (1 << i);
    else s_game_tables &= ~(1 << i);
}

static void on_game_all(lv_event_t *e)
{
    (void)e;
    s_game_tables = GAME_TABLES_ALL;
    for (int i = 0; i < 10; i++) game_set_chip(i, true);
}

static void on_game_none(lv_event_t *e)
{
    (void)e;
    s_game_tables = 0;
    for (int i = 0; i < 10; i++) game_set_chip(i, false);
}

static void on_game_start(lv_event_t *e)
{
    (void)e;
    if (s_game_tables == 0) { toast(T(STR_GAME_PICK_ONE)); return; }
    s_game_a = 0;  // force a fresh question in build_game
    show(build_game);
}

// Shown before build_game so the child can pick which tables (1..10) this
// session drills. Selection lives only in s_game_tables (RAM), reset to all
// 10 on every entry from on_open_game: see the file header comment.
static void build_game_setup(lv_obj_t *scr)
{
    add_back_cb(scr, on_game_back);
    add_title(scr, T(STR_GAME_PICK_TABLES));  // narrower than add_title_wide: a corner button sits on both sides
    // Start: round accent button, top-right (mirrors the back button), same
    // checkmark the keypad uses for "confirm". A bottom CTA row was tried
    // first but did not fit under the chips in landscape (188 px available)
    // without scrolling; the corner spot costs no vertical space at all.
    lv_obj_t *start_b = make_round_btn(scr, LV_SYMBOL_OK, 44, true, on_game_start);
    lv_obj_align(start_b, LV_ALIGN_TOP_RIGHT, -8, 8);

    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_pos(content, 0, 52);
    lv_obj_set_size(content, scr_w(), scr_h() - 52);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_hor(content, PAD_SIDE, 0);
    lv_obj_set_style_pad_top(content, 4, 0);
    lv_obj_set_style_pad_bottom(content, MINI_CLEAR, 0);
    lv_obj_set_style_pad_row(content, 10, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    // 10 checkable chips, 1..10, wrapping to fit either orientation.
    lv_obj_t *row_tbl = lv_obj_create(content);
    lv_obj_set_width(row_tbl, LV_PCT(100));
    lv_obj_set_height(row_tbl, LV_SIZE_CONTENT);
    lv_obj_remove_flag(row_tbl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row_tbl, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row_tbl, 0, 0);
    lv_obj_set_flex_flow(row_tbl, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(row_tbl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_tbl, 6, 0);
    lv_obj_set_style_pad_row(row_tbl, 6, 0);
    for (int i = 0; i < 10; i++) {
        lv_obj_t *chip = lv_button_create(row_tbl);
        lv_obj_set_size(chip, 40, 40);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_add_style(chip, &s_round_surface, 0);
        lv_obj_add_style(chip, &s_round_surface_pr, LV_STATE_PRESSED);
        lv_obj_add_style(chip, &s_chip_ck, LV_STATE_CHECKED);
        if (s_game_tables & (1 << i)) lv_obj_add_state(chip, LV_STATE_CHECKED);
        lv_obj_t *cl = lv_label_create(chip);
        char n[3];
        snprintf(n, sizeof(n), "%d", i + 1);
        lv_label_set_text(cl, n);
        lv_obj_center(cl);
        lv_obj_add_event_cb(chip, on_game_table, LV_EVENT_CLICKED, NULL);
        s_game_tbl_btn[i] = chip;
    }

    // All / None.
    lv_obj_t *row_bulk = alarm_row(content, LV_FLEX_ALIGN_SPACE_EVENLY);
    lv_obj_t *all_b = lv_button_create(row_bulk);
    lv_obj_t *all_l = lv_label_create(all_b);
    lv_label_set_text(all_l, T(STR_GAME_ALL));
    lv_obj_center(all_l);
    lv_obj_add_event_cb(all_b, on_game_all, LV_EVENT_CLICKED, NULL);
    lv_obj_t *none_b = lv_button_create(row_bulk);
    lv_obj_t *none_l = lv_label_create(none_b);
    lv_label_set_text(none_l, T(STR_GAME_NONE));
    lv_obj_center(none_l);
    lv_obj_add_event_cb(none_b, on_game_none, LV_EVENT_CLICKED, NULL);
}

static void build_game(lv_obj_t *scr)
{
    add_back_cb(scr, on_game_back);
    const bool ls = scr_w() > scr_h();

    // Score header: centered on top in portrait; in landscape it lives in the
    // left column (two lines, below the back button) so the keypad never
    // covers it.
    s_game_score_lbl = lv_label_create(scr);
    muted(s_game_score_lbl);  // secondary: the question is the focus, not the tally
    lv_obj_set_style_text_align(s_game_score_lbl, LV_TEXT_ALIGN_CENTER, 0);
    if (ls) {
        lv_obj_set_width(s_game_score_lbl, 110);
        lv_obj_align(s_game_score_lbl, LV_ALIGN_TOP_LEFT, 8, 52);
    } else {
        lv_obj_align(s_game_score_lbl, LV_ALIGN_TOP_MID, 0, 10);
    }

    lv_obj_t *q = lv_label_create(scr);
    lv_obj_set_style_text_font(q, &bugne_font_20, 0);  // the question is the focal point
    lv_label_set_long_mode(q, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *fb = lv_label_create(scr);
    lv_label_set_long_mode(fb, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(fb, LV_TEXT_ALIGN_CENTER, 0);

    static const char *KEYS[] = {"1", "2", "3", "\n", "4", "5", "6", "\n",
                                 "7", "8", "9", "\n",
                                 LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, ""};
    lv_obj_t *km = lv_buttonmatrix_create(scr);
    lv_buttonmatrix_set_map(km, KEYS);
    if (ls) {
        // Left column: score above, then question, then feedback; keypad on
        // the right, sized so its bottom edge clears the floating mini bar.
        // The question is bugne_font_20 now, wide enough that a two-digit
        // pair ("10 x 10 = ?") wraps to 2 lines in this 118 px column, so
        // it gets more headroom than the single-line 14 px feedback below it.
        lv_obj_set_width(q, 116);
        lv_obj_align(q, LV_ALIGN_TOP_LEFT, 8, 130);
        lv_obj_set_width(fb, 116);
        lv_obj_align(fb, LV_ALIGN_TOP_LEFT, 8, 172);
        lv_obj_set_size(km, 190, 160);
        lv_obj_align(km, LV_ALIGN_RIGHT_MID, -4, -20);  // spans y 20..180, bar starts at 184
    } else {
        lv_obj_set_width(q, scr_w() - 20);
        lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 48);
        lv_obj_set_width(fb, scr_w() - 20);
        lv_obj_align(fb, LV_ALIGN_TOP_MID, 0, 72);
        lv_obj_set_size(km, 220, 160);
        lv_obj_align(km, LV_ALIGN_BOTTOM_MID, 0, -MINI_CLEAR);  // above the mini bar
    }
    lv_obj_add_event_cb(km, on_game_key, LV_EVENT_VALUE_CHANGED, NULL);
    s_game_q_lbl = q;
    s_game_fb_lbl = fb;
    if (s_game_a == 0) game_new_question();  // fresh session (see on_open_game)
    else game_refresh();                     // rebuild: keep the running game
}

// Podcasts open without Wi-Fi: manifests live in flash and downloaded episodes
// play from SD. Only non-downloaded episodes are greyed out while offline.
static void on_open_podcasts(lv_event_t *e)  { (void)e; show(build_podcasts); }
// ---- Instrument tuner (engine + screen; state declared before ui_play) ----

// Note names. English letters are shown in both languages; French prepends
// the Latin (solfege) name: "Mi (E2)". Octave is the MIDI convention (C4=60).
static const char *TUNER_NOTE_EN[12] =
    {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *TUNER_NOTE_FR[12] =
    {"Do", "Do#", "Ré", "Ré#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si"};

// Capture task: blocking 128 ms mic reads, YIN pitch detection (pitch.c),
// median-of-3 smoothing, results published in the s_tuner_* statics read by
// the 50 ms UI tick. PSRAM stack is safe: no flash API is called here.
static void tuner_task(void *arg)
{
    (void)arg;
    // The sources release the arbiter asynchronously after the takeover in
    // tuner_begin(), and an engaged Music Assistant session needs a server
    // round-trip to end, so retry for a few seconds instead of giving up.
    bool acquired = false;
    for (int i = 0; i < 80 && s_tuner_run; i++) {
        if (audio_arbiter_acquire(AUDIO_SOURCE_TUNER) == ESP_OK) { acquired = true; break; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (acquired && s_tuner_run && audio_record_open(TUNER_SAMPLE_RATE) == ESP_OK) {
        int16_t *buf = heap_caps_malloc(PITCH_BUF_SAMPLES * sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM);
        float hist[3];
        int nhit = 0;
        int64_t last_log = 0;
        // Sliding window: after the first full 128 ms fill, each turn keeps
        // the newest half and reads only a 64 ms hop, so the readout updates
        // twice as fast and settles in ~200 ms with the median below.
        const int hop = PITCH_BUF_SAMPLES / 2;
        int have = 0;
        int warm = 2;  // windows discarded after mic open (~190 ms)
        while (buf && s_tuner_run) {
            if (audio_record_read(buf + have,
                                  (PITCH_BUF_SAMPLES - have) * sizeof(int16_t)) != ESP_OK) {
                ESP_LOGW(TAG, "tuner: mic read failed");
                break;
            }
            float f = 0.0f, rms = 0.0f;
            bool hit = pitch_detect(buf, (float)TUNER_SAMPLE_RATE, &f, &rms);
            memmove(buf, buf + hop, (PITCH_BUF_SAMPLES - hop) * sizeof(int16_t));
            have = PITCH_BUF_SAMPLES - hop;
            if (warm > 0) {  // capture-start transient (bench: rms ~25000 pop
                warm--;      // on the first window): never show it as a note
                hit = false;
            }
            if (hit) {
                hist[nhit % 3] = f;
                nhit++;
                float m = f;  // median of the last 3 once available
                if (nhit >= 3) {
                    float a = hist[0], b = hist[1], c = hist[2];
                    m = (a > b) ? ((b > c) ? b : (a > c ? c : a))
                                : ((a > c) ? a : (b > c ? c : b));
                }
                int midi;
                float cents;
                pitch_note_from_freq(m, &midi, &cents);
                s_tuner_freq = m;
                s_tuner_cents = cents;
                s_tuner_midi = midi;
                s_tuner_hit_us = esp_timer_get_time();
            } else {
                nhit = 0;  // a gap breaks the median window
            }
            // Bench evidence line (mic gain tuning): every ~2 s while capturing.
            int64_t now = esp_timer_get_time();
            if (now - last_log >= 2000000) {
                last_log = now;
                ESP_LOGI(TAG, "tuner: rms=%.0f %s freq=%.1f Hz",
                         rms, hit ? "pitch" : "no-pitch", hit ? f : 0.0f);
            }
        }
        free(buf);
        audio_record_close();
    } else if (acquired) {
        ESP_LOGW(TAG, "tuner: mic open failed");
    } else {
        ESP_LOGW(TAG, "tuner: arbiter busy, giving up");
    }
    if (acquired) audio_arbiter_release(AUDIO_SOURCE_TUNER);
    ESP_LOGI(TAG, "tuner: task exit");
    s_tuner_active = false;
    vTaskDeleteWithCaps(NULL);
}

// Take over playback (same sequence as ui_play) and start the capture task.
// Shared by the tile handler and the debug-nav applier. Safe to call while
// already running (screen rebuilds on orientation/language change).
static void tuner_begin(void)
{
    if (s_tuner_active) return;
    ui_stop();
    if (source_sendspin_session_active()) {
        source_sendspin_command(SENDSPIN_CMD_STOP);
    }
    s_tuner_midi = -1;
    s_tuner_hit_us = 0;
    s_tuner_run = true;
    s_tuner_active = true;
    if (xTaskCreateWithCaps(tuner_task, "tuner", 6144, NULL, 4, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        s_tuner_active = false;
        s_tuner_run = false;
        ESP_LOGE(TAG, "tuner: task create failed");
    }
}

static void on_tuner_back(lv_event_t *e)
{
    (void)e;
    tuner_stop_sync();
    show(build_home);
}

// Deliberately NOT gated on quiet hours: the tuner makes no sound (the amp
// stays muted on the record path) and is an adult tool.
static void on_open_tuner(lv_event_t *e)
{
    (void)e;
    tuner_begin();
    show(build_tuner);
}

static void build_tuner(lv_obj_t *scr)
{
    add_back_cb(scr, on_tuner_back);
    add_title_wide(scr, T(STR_TUNER));

    // Big note name, refreshed by the 50 ms tick in sleep_timer_cb.
    s_tuner_note_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_tuner_note_lbl, &bugne_font_20, 0);
    lv_label_set_text(s_tuner_note_lbl, T(STR_TUNER_PLAY_NOTE));
    lv_obj_align(s_tuner_note_lbl, LV_ALIGN_CENTER, 0, -44);

    s_tuner_freq_lbl = lv_label_create(scr);
    muted(s_tuner_freq_lbl);
    lv_label_set_text(s_tuner_freq_lbl, "");
    lv_obj_align(s_tuner_freq_lbl, LV_ALIGN_CENTER, 0, -14);

    // Cents gauge, -50..+50, drawn from the center (symmetrical bar). The
    // indicator takes the accent color from the theme like every bar.
    s_tuner_bar = lv_bar_create(scr);
    lv_bar_set_range(s_tuner_bar, -50, 50);
    lv_bar_set_mode(s_tuner_bar, LV_BAR_MODE_SYMMETRICAL);
    lv_bar_set_value(s_tuner_bar, 0, LV_ANIM_OFF);
    lv_obj_set_size(s_tuner_bar, scr_w() - 2 * PAD_SIDE - 32, 14);
    lv_obj_align(s_tuner_bar, LV_ALIGN_CENTER, 0, 28);

    // Center (in tune) marker over the gauge.
    lv_obj_t *tick = lv_obj_create(scr);
    lv_obj_set_size(tick, 2, 24);
    lv_obj_set_style_bg_color(tick, col_muted(), 0);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tick, 0, 0);
    lv_obj_set_style_radius(tick, 0, 0);
    lv_obj_align(tick, LV_ALIGN_CENTER, 0, 28);
}

// ---- Voice memos (record, keep, send to another Bugne, play received) ----
// Files live in /sdcard/memos (see components/memo). Capture and playback run
// on the ui_play worker (internal stack: they write/read the SD); the arbiter
// source is AUDIO_SOURCE_MEMO. State machine: s_memo_state, advanced by the
// worker and rendered by build_memo_record via the 50 ms tick edge detector.

#define MEMO_REC_CHUNK 4096  // bytes per mic read/SD write: 128 ms at 16 kHz mono

static memo_entry_t *s_memo_entries;         // list rows (SPIRAM, lazy)
static int  s_memo_entry_count;
static char s_memo_sel_name[MEMO_NAME_MAX];  // memo behind build_memo_play
static char s_memo_sel_title[56];
static int  s_memo_state_shown = MEMO_UI_IDLE;  // what build_memo_record last rendered
static int  s_memo_peers_shown;

// Record the mic to memos/.rec.part, finalize the WAV header, rename to
// .rec.wav (kept hidden until Keep/Send). Ends on Stop, the 60 s cap, a
// takeover (s_memo_stop or a queued request), or an SD/mic error.
static void memo_record_run(void)
{
    s_memo_rec_ms = 0;
    bool ok = false;
    uint64_t free_b = 0;
    bool room = source_sd_present() && source_sd_usage(NULL, &free_b) && free_b >= (4u << 20);
    bool acquired = false;
    for (int i = 0; room && i < 80 && !s_memo_stop; i++) {  // tuner-style retry: sources
        if (audio_arbiter_acquire(AUDIO_SOURCE_MEMO) == ESP_OK) { acquired = true; break; }
        vTaskDelay(pdMS_TO_TICKS(50));                       // release asynchronously
    }
    if (acquired && !s_memo_stop && audio_record_open(MEMO_RATE_HZ) == ESP_OK) {
        source_sd_mkdir(MEMO_DIR);
        FILE *f = source_sd_create(MEMO_DIR "/.rec.part");
        uint8_t *buf = heap_caps_malloc(MEMO_REC_CHUNK, MALLOC_CAP_SPIRAM);
        if (f && buf) {
            uint8_t hdr[MEMO_WAV_HEADER_BYTES] = {0};
            uint32_t data_bytes = 0;
            ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);  // placeholder header
            // Discard the capture-start transient (same warm-up as the tuner:
            // the first windows carry a loud pop). Without this the pop is the
            // file's peak and caps the normalization gain on the actual voice.
            for (int w = 0; ok && w < 2 && !s_memo_stop; w++) {
                if (audio_record_read(buf, MEMO_REC_CHUNK) != ESP_OK) ok = false;
            }
            while (ok && !s_memo_stop && s_memo_rec_ms < MEMO_MAX_MS &&
                   uxQueueMessagesWaiting(s_play_q) == 0) {
                if (audio_record_read(buf, MEMO_REC_CHUNK) != ESP_OK ||
                    fwrite(buf, 1, MEMO_REC_CHUNK, f) != MEMO_REC_CHUNK) {
                    ok = false;
                    break;
                }
                data_bytes += MEMO_REC_CHUNK;
                s_memo_rec_ms = (int)((uint64_t)data_bytes * 1000 / (MEMO_RATE_HZ * 2));
            }
            if (ok && data_bytes == 0) ok = false;  // stopped before the first chunk
            if (ok) {  // rewrite the header with the real size
                memo_wav_header(hdr, data_bytes);
                ok = fseek(f, 0, SEEK_SET) == 0 &&
                     fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
            }
        }
        if (f) fclose(f);
        audio_record_close();
        if (ok && buf) {
            // Voice through the onboard mic records quietly even at the 42 dB
            // PGA max (bench-confirmed): normalize the finished file so the
            // peak lands near full scale (digital gain capped at x16).
            FILE *nf = fopen(MEMO_ABS_DIR "/.rec.part", "r+b");
            if (nf) {
                if (!memo_wav_normalize(nf, buf, MEMO_REC_CHUNK)) {
                    ESP_LOGW(TAG, "memo: normalize failed, keeping raw level");
                }
                fclose(nf);
            }
        }
        free(buf);
    }
    if (acquired) audio_arbiter_release(AUDIO_SOURCE_MEMO);
    if (ok && rename(MEMO_ABS_DIR "/.rec.part", MEMO_ABS_DIR "/" MEMO_REC_NAME) != 0) ok = false;
    if (!ok) {
        remove(MEMO_ABS_DIR "/.rec.part");
        s_memo_result = -2;
        s_memo_state = MEMO_UI_IDLE;
        ESP_LOGW(TAG, "memo: recording failed or empty");
        return;
    }
    ESP_LOGI(TAG, "memo: recorded %d ms", s_memo_rec_ms);
    s_memo_state = MEMO_UI_PREVIEW;
}

// Play a memo WAV straight to the PCM output (beep_run shape: audio_write
// blocks on the I2S DMA and paces the loop; no decoder involved).
static void memo_play_run(const char *path)
{
    bool acquired = false;
    for (int i = 0; i < 80 && !s_memo_stop; i++) {
        if (audio_arbiter_acquire(AUDIO_SOURCE_MEMO) == ESP_OK) { acquired = true; break; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!acquired) return;
    FILE *f = fopen(path, "rb");
    uint8_t head[512];
    uint32_t off = 0, len = 0;
    bool ok = false;
    if (f) {
        size_t n = fread(head, 1, sizeof(head), f);
        ok = memo_wav_parse(head, n, &off, &len) && fseek(f, (long)off, SEEK_SET) == 0;
    }
    if (ok && audio_open(MEMO_RATE_HZ, 16, 1) == ESP_OK) {
        uint8_t *buf = heap_caps_malloc(MEMO_REC_CHUNK, MALLOC_CAP_SPIRAM);
        uint32_t left = len;
        s_memo_playing = true;
        while (buf && left > 0 && !s_memo_stop && uxQueueMessagesWaiting(s_play_q) == 0) {
            size_t want = left < MEMO_REC_CHUNK ? left : MEMO_REC_CHUNK;
            size_t n = fread(buf, 1, want, f);
            if (n == 0 || audio_write(buf, n) != ESP_OK) break;
            left -= (uint32_t)n;
        }
        free(buf);
        audio_close();
    } else if (f) {
        ESP_LOGW(TAG, "memo: cannot play %s", path);
    }
    if (f) fclose(f);
    s_memo_playing = false;  // after fclose: memo_stop_sync callers rename/delete the file
    audio_arbiter_release(AUDIO_SOURCE_MEMO);
}

// POST the finalized capture to the picked peer, as this device's name.
static void memo_send_run(int peer)
{
    esp_err_t r = ESP_FAIL;
    if (peer >= 0 && peer < s_memo_peer_count) {
        char raw[48], from[MEMO_SENDER_MAX];
        const config_t *c = config_store_get();
        if (c && c->device_name[0]) strlcpy(raw, c->device_name, sizeof(raw));
        else snprintf(raw, sizeof(raw), "Bugne %s", board_device_id());
        memo_sanitize_sender(from, sizeof(from), raw);
        ESP_LOGI(TAG, "memo: sending to %s (%s:%u)", s_memo_peers[peer].name,
                 s_memo_peers[peer].ip, (unsigned)s_memo_peers[peer].port);
        r = memo_send(s_memo_peers[peer].ip, s_memo_peers[peer].port, from,
                      MEMO_ABS_DIR "/" MEMO_REC_NAME, NULL);
    }
    if (r == ESP_OK) {
        remove(MEMO_ABS_DIR "/" MEMO_REC_NAME);  // delivered: nothing kept locally
        s_memo_state = MEMO_UI_IDLE;
        s_memo_result = 1;
    } else {
        s_memo_state = MEMO_UI_PREVIEW;  // keep the capture so the child can retry
        s_memo_result = -1;
    }
}

// Stop a memo playback and wait for the worker to close the file (bounded;
// needed before deleting or renaming the file under it).
static void memo_stop_sync(void)
{
    s_memo_stop = true;
    for (int i = 0; i < 50 && s_memo_playing; i++) vTaskDelay(pdMS_TO_TICKS(10));
}

// Take over playback (same sequence as tuner_begin) and queue a memo request.
static void memo_request(req_kind_t kind, const char *path)
{
    ui_stop();
    if (source_sendspin_session_active()) {
        source_sendspin_command(SENDSPIN_CMD_STOP);
    }
    tuner_stop_sync();
    s_memo_stop = false;  // ui_stop just set it; this is a fresh memo operation
    play_req_t req = { .kind = kind };
    if (path) strlcpy(req.target, path, sizeof(req.target));
    xQueueOverwrite(s_play_q, &req);
}

// Show the record screen for the current state and remember what was shown, so
// the tick's edge detector does not rebuild it again on its next run.
static void memo_show_record(void)
{
    s_memo_state_shown = s_memo_state;
    s_memo_peers_shown = s_memo_peer_count;
    show(build_memo_record);
}

// Human title of a memo entry. The sender inside the filename is sanitized
// ("Bugne-Alpha"); show the dashes as spaces.
static void memo_row_title(const memo_entry_t *en, char *dst, size_t size)
{
    if (en->is_mine) {
        snprintf(dst, size, T(STR_MEMO_MINE_FMT), en->seq);
    } else {
        char nice[MEMO_SENDER_MAX];
        strlcpy(nice, en->sender, sizeof(nice));
        for (char *p = nice; *p; p++) {
            if (*p == '-') *p = ' ';
        }
        snprintf(dst, size, T(STR_MEMO_FROM_FMT), nice);
    }
}

static void on_open_memos(lv_event_t *e)
{
    (void)e;
    if (play_denied()) return;
    if (!source_sd_present()) return;  // the tile is greyed; belt and braces
    show(build_memos);
}

static void on_memo_new(lv_event_t *e)
{
    (void)e;
    s_memo_state = MEMO_UI_IDLE;
    s_memo_rec_ms = 0;
    memo_show_record();
}

static void on_memo_rec_start(lv_event_t *e)
{
    (void)e;
    uint64_t free_b = 0;
    if (!source_sd_present() || !source_sd_usage(NULL, &free_b) || free_b < (4u << 20)) {
        toast(T(STR_MEMO_REC_FAILED));  // no card or almost full
        return;
    }
    s_memo_rec_ms = 0;
    s_memo_state = MEMO_UI_RECORDING;
    memo_request(REQ_MEMO_RECORD, NULL);
    memo_show_record();
}

static void on_memo_rec_stop(lv_event_t *e)
{
    (void)e;
    s_memo_stop = true;  // the worker finalizes and advances to PREVIEW
}

static void on_memo_prev_play(lv_event_t *e)
{
    (void)e;
    memo_request(REQ_MEMO_PLAY, MEMO_ABS_DIR "/" MEMO_REC_NAME);
}

static void on_memo_keep(lv_event_t *e)
{
    (void)e;
    memo_stop_sync();
    if (memo_count() >= MEMO_MAX_COUNT) {
        toast(T(STR_MEMO_FULL));
        return;
    }
    if (memo_keep_rec() < 0) {
        toast(T(STR_MEMO_REC_FAILED));
        return;
    }
    s_memo_state = MEMO_UI_IDLE;
    show(build_memos);
}

static void on_memo_send_open(lv_event_t *e)
{
    (void)e;
    memo_stop_sync();
    s_memo_peer_count = -1;  // browse running
    s_memo_state = MEMO_UI_PICK_PEER;
    play_req_t req = { .kind = REQ_MEMO_PEERS };
    xQueueOverwrite(s_play_q, &req);
    memo_show_record();
}

static void on_memo_discard(lv_event_t *e)
{
    (void)e;
    memo_stop_sync();
    remove(MEMO_ABS_DIR "/" MEMO_REC_NAME);
    s_memo_state = MEMO_UI_IDLE;
    memo_show_record();  // fresh record screen, ready to retry
}

static void on_memo_peer(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (idx < 0 || idx >= s_memo_peer_count) return;
    s_memo_state = MEMO_UI_SENDING;
    play_req_t req = { .kind = REQ_MEMO_SEND, .id = idx };
    xQueueOverwrite(s_play_q, &req);
    memo_show_record();
}

static void on_memo_record_back(lv_event_t *e)
{
    (void)e;
    int st = s_memo_state;
    if (st == MEMO_UI_RECORDING) {
        s_memo_stop = true;  // finalize to PREVIEW, stay: never silently lose a capture
        return;
    }
    if (st == MEMO_UI_PICK_PEER) {
        s_memo_state = MEMO_UI_PREVIEW;
        memo_show_record();
        return;
    }
    if (st == MEMO_UI_SENDING) return;  // let the send finish (a toast follows)
    if (st == MEMO_UI_PREVIEW) {        // back out of the preview = discard
        memo_stop_sync();
        remove(MEMO_ABS_DIR "/" MEMO_REC_NAME);
        s_memo_state = MEMO_UI_IDLE;
    }
    show(build_memos);
}

// Small labeled action button (icon + text) for the preview row.
static lv_obj_t *memo_btn(lv_obj_t *scr, const char *icon, str_id_t id,
                          int x, int y, int w, int h, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text_fmt(l, "%s %s", icon, T(id));
    lv_obj_center(l);
    return b;
}

static void build_memo_record(lv_obj_t *scr)
{
    add_back_cb(scr, on_memo_record_back);
    add_title_wide(scr, T(STR_MEMO_RECORD));  // only corner widget is the back button
    switch ((memo_ui_state_t)s_memo_state) {
    case MEMO_UI_RECORDING: {
        s_memo_time_lbl = lv_label_create(scr);
        lv_obj_set_style_text_font(s_memo_time_lbl, &bugne_font_20, 0);
        lv_label_set_text(s_memo_time_lbl, "0:00");
        lv_obj_align(s_memo_time_lbl, LV_ALIGN_CENTER, 0, -52);
        s_memo_prog_bar = lv_bar_create(scr);
        lv_bar_set_range(s_memo_prog_bar, 0, MEMO_MAX_MS);
        lv_obj_set_size(s_memo_prog_bar, scr_w() - 2 * PAD_SIDE - 32, 14);
        lv_obj_align(s_memo_prog_bar, LV_ALIGN_CENTER, 0, -16);
        lv_obj_t *stop = make_round_btn(scr, LV_SYMBOL_STOP, 72, true, on_memo_rec_stop);
        lv_obj_align(stop, LV_ALIGN_CENTER, 0, 56);
        break;
    }
    case MEMO_UI_PREVIEW: {
        lv_obj_t *play = make_round_btn(scr, LV_SYMBOL_PLAY, 56, false, on_memo_prev_play);
        lv_obj_align(play, LV_ALIGN_TOP_MID, 0, 46);
        lv_obj_t *len = lv_label_create(scr);
        int sdur = s_memo_rec_ms / 1000;
        lv_label_set_text_fmt(len, "%d:%02d", sdur / 60, sdur % 60);
        muted(len);
        lv_obj_align(len, LV_ALIGN_TOP_MID, 0, 106);
        const int bw = (scr_w() - 2 * PAD_SIDE - 10) / 2;
        memo_btn(scr, LV_SYMBOL_OK, STR_MEMO_KEEP, PAD_SIDE, 128, bw, 42, on_memo_keep);
        memo_btn(scr, LV_SYMBOL_UPLOAD, STR_MEMO_SEND, PAD_SIDE + bw + 10, 128, bw, 42,
                 on_memo_send_open);
        memo_btn(scr, LV_SYMBOL_TRASH, STR_MEMO_DISCARD, PAD_SIDE, 180, 2 * bw + 10, 42,
                 on_memo_discard);
        break;
    }
    case MEMO_UI_PICK_PEER:
        if (s_memo_peer_count < 0) {
            empty_state_label(scr, STR_MEMO_SEARCHING);
        } else if (s_memo_peer_count == 0) {
            empty_state_label(scr, STR_MEMO_NO_PEERS);
        } else {
            lv_obj_t *list = lv_list_create(scr);
            lv_obj_set_size(list, scr_w(), scr_h() - 56);
            lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
            for (int i = 0; i < s_memo_peer_count; i++) {
                lv_obj_t *btn = lv_list_add_button(list, LV_SYMBOL_UPLOAD, s_memo_peers[i].name);
                lv_obj_set_user_data(btn, (void *)(intptr_t)i);
                lv_obj_add_event_cb(btn, on_memo_peer, LV_EVENT_CLICKED, NULL);
            }
            list_titles_static(list);
        }
        break;
    case MEMO_UI_SENDING:
        empty_state_label(scr, STR_MEMO_SENDING);
        break;
    default: {  // MEMO_UI_IDLE
        lv_obj_t *hint = lv_label_create(scr);
        lv_label_set_text(hint, T(STR_MEMO_TAP_RECORD));
        muted(hint);
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, -64);
        // A filled red circle is the universal record affordance; deliberately
        // not the theme accent (which can be red-adjacent, but the tile look
        // keeps this readable in every theme).
        lv_obj_t *rec = make_round_btn(scr, "", 80, true, on_memo_rec_start);
        lv_obj_set_style_bg_color(rec, lv_color_hex(0xE53935), 0);
        lv_obj_set_style_bg_color(rec, lv_color_hex(0xB71C1C), LV_STATE_PRESSED);
        lv_obj_align(rec, LV_ALIGN_CENTER, 0, 4);
        break;
    }
    }
}

static void on_memo_play_back(lv_event_t *e)
{
    (void)e;
    memo_stop_sync();
    show(build_memos);
}

static void on_memo_del(lv_event_t *e)
{
    (void)e;
    memo_stop_sync();  // never delete the file under the worker's fread
    char rel[MEMO_NAME_MAX + 8];
    snprintf(rel, sizeof(rel), MEMO_DIR "/%s", s_memo_sel_name);
    source_sd_delete(rel);
    show(build_memos);
}

static void on_memo_play_toggle(lv_event_t *e)
{
    (void)e;
    if (s_memo_playing) {
        s_memo_stop = true;
    } else {
        if (play_denied()) return;
        char path[MEMO_NAME_MAX + 20];
        memo_abs_path(path, sizeof(path), s_memo_sel_name);
        memo_request(REQ_MEMO_PLAY, path);
    }
}

static void build_memo_play(lv_obj_t *scr)
{
    add_back_cb(scr, on_memo_play_back);
    add_title(scr, s_memo_sel_title);  // two corner widgets: never add_title_wide
    lv_obj_t *del = make_round_btn(scr, LV_SYMBOL_TRASH, 44, false, on_memo_del);
    lv_obj_align(del, LV_ALIGN_TOP_RIGHT, -8, 8);
    s_memo_play_btn = make_round_btn(scr, s_memo_playing ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY,
                                     72, true, on_memo_play_toggle);
    lv_obj_align(s_memo_play_btn, LV_ALIGN_CENTER, 0, 0);
}

static void on_memo_row(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (!s_memo_entries || idx < 0 || idx >= s_memo_entry_count) return;
    if (play_denied()) return;
    memo_entry_t *en = &s_memo_entries[idx];
    // Opening a received memo marks it read at once (the rename drops ".new"):
    // the home badge means "something you have not listened to yet".
    if (en->unread) {
        char oldp[MEMO_NAME_MAX + 20], newn[MEMO_NAME_MAX], newp[MEMO_NAME_MAX + 20];
        memo_abs_path(oldp, sizeof(oldp), en->name);
        snprintf(newn, sizeof(newn), "rx-%s-%03d.wav", en->sender, en->seq);
        memo_abs_path(newp, sizeof(newp), newn);
        if (rename(oldp, newp) == 0) {
            strlcpy(en->name, newn, sizeof(en->name));
            en->unread = false;
        }
    }
    strlcpy(s_memo_sel_name, en->name, sizeof(s_memo_sel_name));
    memo_row_title(en, s_memo_sel_title, sizeof(s_memo_sel_title));
    show(build_memo_play);
    char path[MEMO_NAME_MAX + 20];
    memo_abs_path(path, sizeof(path), s_memo_sel_name);
    memo_request(REQ_MEMO_PLAY, path);  // tapping a memo plays it right away
}

static void build_memos(lv_obj_t *scr)
{
    add_back_button(scr);
    add_title(scr, T(STR_MEMOS));  // two corner widgets: never add_title_wide
    lv_obj_t *rec = make_round_btn(scr, LV_SYMBOL_PLUS, 44, true, on_memo_new);
    lv_obj_align(rec, LV_ALIGN_TOP_RIGHT, -8, 8);

    if (!s_memo_entries) {
        s_memo_entries = heap_caps_malloc(sizeof(*s_memo_entries) * MEMO_MAX_COUNT,
                                          MALLOC_CAP_SPIRAM);
    }
    s_memo_entry_count = s_memo_entries ? memo_list(s_memo_entries, MEMO_MAX_COUNT) : 0;
    if (s_memo_entry_count == 0) {
        empty_state_label(scr, STR_MEMO_EMPTY);
        return;
    }
    lv_obj_t *list = lv_list_create(scr);
    lv_obj_set_size(list, scr_w(), scr_h() - 56);  // below the round corner buttons
    lv_obj_set_style_pad_bottom(list, MINI_CLEAR, 0);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    for (int i = 0; i < s_memo_entry_count; i++) {
        const memo_entry_t *en = &s_memo_entries[i];
        char title[56], line[80];
        memo_row_title(en, title, sizeof(title));
        snprintf(line, sizeof(line), "%s (%d:%02d)", title,
                 en->duration_s / 60, en->duration_s % 60);
        const char *icon = en->unread ? LV_SYMBOL_BELL
                         : en->is_mine ? LV_SYMBOL_AUDIO : LV_SYMBOL_ENVELOPE;
        lv_obj_t *btn = lv_list_add_button(list, icon, line);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, on_memo_row, LV_EVENT_CLICKED, NULL);
    }
    list_titles_static(list);
}

// Receive-side bridge: the httpd task calls this after storing a memo; the
// 50 ms tick consumes the flag (toast + badge/list rebuild). No LVGL here.
void ui_memo_received(const char *from)
{
    strlcpy(s_memo_rx_from, from ? from : "", sizeof(s_memo_rx_from));
    s_memo_rx_flag = true;
}

static void on_open_sd(lv_event_t *e)        { (void)e; show(build_sd); }

// Favorites (B2) ------------------------------------------------------------

// The webradio with this stable id, or NULL if it was deleted from the list.
static const config_webradio_t *fav_radio_by_id(int id)
{
    const config_t *c = config_store_get();
    for (size_t i = 0; c && i < c->webradio_count; i++) {
        if (c->webradios[i].id == id) return &c->webradios[i];
    }
    return NULL;
}

// True when this favorite can play right now (radio still configured and
// Wi-Fi up, or SD file present). Used to grey the row; the tap handler
// re-checks and explains with a toast.
static bool fav_available(const config_favorite_t *f)
{
    if (f->type == 0) {
        return fav_radio_by_id(f->radio_id) != NULL && net_state() == NET_STATE_CONNECTED;
    }
    char path[16 + CFG_FAV_PATH_MAX];
    snprintf(path, sizeof(path), "/sdcard/%s", f->path);
    struct stat st;
    return source_sd_present() && stat(path, &st) == 0;
}

// Row tap: play the favorite. Parental gate here, at the user entry point,
// never inside ui_play (GATE PLACEMENT RULE). Missing content (radio id gone,
// SD file gone) refuses with a toast instead of a silent failure.
static void on_favorite_row(lv_event_t *e)
{
    if (play_denied()) return;
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    const config_t *c = config_store_get();
    if (!c || i < 0 || (size_t)i >= c->favorite_count) return;
    const config_favorite_t *f = &c->favorites[i];
    if (f->type == 0) {
        const config_webradio_t *r = fav_radio_by_id(f->radio_id);
        if (!r) { toast(T(STR_FAV_MISSING)); return; }  // deleted from the radio list
        if (net_state() != NET_STATE_CONNECTED) { toast(T(STR_FAV_OFFLINE)); return; }
        s_play_ctx = PLAY_CTX_NONE;  // a single stream, no next/previous
        ui_play(false, r->url, r->name, 0);
        show(build_now_playing);
    } else {
        char path[16 + CFG_FAV_PATH_MAX];
        snprintf(path, sizeof(path), "/sdcard/%s", f->path);
        struct stat st;
        if (!source_sd_present() || stat(path, &st) != 0) { toast(T(STR_FAV_MISSING)); return; }
        const char *slash = strrchr(f->path, '/');
        s_play_ctx = PLAY_CTX_NONE;  // a single track, like the web remote's path play
        s_np_dur_override_ms = 0;
        ui_play(true, path, f->title[0] ? f->title : (slash ? slash + 1 : f->path), 0);
        show(build_now_playing);
    }
}

static void build_favorites(lv_obj_t *scr)
{
    add_back_button(scr);
    add_title_wide(scr, T(STR_FAVORITES));
    const config_t *c = config_store_get();
    if (!c || c->favorite_count == 0) {
        empty_state_label(scr, STR_NO_FAVORITES);
        return;
    }
    lv_obj_t *list = lv_list_create(scr);
    lv_obj_set_size(list, scr_w(), scr_h() - 56);  // below the 44 px round back button (8+44)
    lv_obj_set_style_pad_bottom(list, MINI_CLEAR, 0);  // last row scrolls clear of the mini bar
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    for (size_t i = 0; i < c->favorite_count; i++) {
        const config_favorite_t *f = &c->favorites[i];
        const config_webradio_t *r = (f->type == 0) ? fav_radio_by_id(f->radio_id) : NULL;
        const char *slash = (f->type == 1) ? strrchr(f->path, '/') : NULL;
        const char *title = f->title[0] ? f->title
                          : (f->type == 0) ? (r ? r->name : "?")
                          : (slash ? slash + 1 : f->path);
        lv_obj_t *btn = lv_list_add_button(list, (f->type == 0) ? LV_SYMBOL_AUDIO : LV_SYMBOL_SD_CARD, title);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, on_favorite_row, LV_EVENT_CLICKED, NULL);
        // Grey unavailable rows (deleted radio, offline radio, missing SD
        // file); they stay tappable so the tap can explain why (toast above).
        if (!fav_available(f)) {
            lv_obj_t *lbl = lv_obj_get_child(btn, -1);  // label is last child
            if (lbl) muted(lbl);
        }
    }
    list_titles_static(list);
}

static void on_open_favorites(lv_event_t *e) { (void)e; show(build_favorites); }
static void on_open_settings(lv_event_t *e)  { (void)e; show(build_settings); }

// Position of menu button idx on a menu screen of `count` items. Since the
// 2026-07-03 tile redesign only the 5-item settings screen uses this; the
// count == 4 spacing is kept for a future 4-item menu.
// Portrait = one column, landscape = a 2-column grid.
static void menu_pos(int idx, int count, int *x, int *y)
{
    // The last row's bottom edge must clear the floating mini bar, so the row
    // spacing derives from that limit. Button heights match add_menu_button's
    // callers: 40 px on 5-item menus, 50 px on 4-item ones. 40 (not 44) so
    // the resulting pitch stays >= the button height in both orientations
    // (five 44 px buttons do not fit the band and overlapped by 1-2 px).
    const int limit = scr_h() - MINI_CLEAR;
    const int h = (count == 5) ? 40 : 50;
    if (scr_w() > scr_h()) {
        // 2x2 grid (4 items, 2 rows) or 2+2+1 with a centered last (5 items, 3 rows).
        *x = (count == 5 && idx == 4) ? 0 : (idx % 2) ? 80 : -80;
        const int y0 = (count == 5) ? 46 : 60;
        const int rows = (count == 5) ? 3 : 2;
        *y = y0 + (idx / 2) * ((limit - y0 - h) / (rows - 1));
    } else {
        *x = 0;
        const int y0 = (count == 5) ? 44 : 56;  // 4-item menus clear the 44 px round back button
        *y = y0 + idx * ((limit - y0 - h) / (count - 1));
    }
}

static lv_obj_t *add_menu_button(lv_obj_t *scr, const char *text, int x, int y, int h, lv_event_cb_t cb)
{
    // 150 px wide in landscape so two fit side by side (2x2 grid); 180 portrait.
    // Height: 50 on 4-item menus, 40 on the 5-item settings (clears the mini bar).
    int w = (scr_w() > scr_h()) ? 150 : 180;
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, LV_ALIGN_TOP_MID, x, y);
    lv_obj_t *l = lv_label_create(b);
    // Wrap within the button so longer labels (e.g. French translations) do not
    // overflow; centered on both axes.
    lv_obj_set_width(l, w - 16);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

// Menu button whose label is "<icon> <translated text>". add_menu_button copies the
// text immediately, so the shared format buffer is safe to reuse across calls.
static lv_obj_t *add_menu_button_t(lv_obj_t *scr, const char *icon, str_id_t id, int x, int y, int h, lv_event_cb_t cb)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%s %s", icon, T(id));
    return add_menu_button(scr, buf, x, y, h, cb);
}

// A centered caption that wraps within the given width, so longer translations
// (e.g. French) do not run off the edge. Portrait uses the full screen width;
// landscape uses the column right of the QR. Always secondary text (muted),
// since it only explains the QR next to it. Returns the label for further
// styling if a caller needs it.
static lv_obj_t *qr_caption(lv_obj_t *scr, const char *text, lv_align_t align, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, text);
    lv_obj_align(l, align, x, y);
    muted(l);
    return l;
}

// A home-screen tile: an lv_button with the "playful tiles" look (s_tile /
// s_tile_pr / s_tile_dis), absolutely positioned within scr. Vertical layout
// centers an icon (montserrat_28) above a label (bugne_font_14, wrapped);
// horizontal layout puts the icon at the left and a single LONG_DOT line to
// its right, both vertically centered. A short vertical tile (the landscape
// 3x2 mini-grid) cannot stack a 28 px icon plus two text lines, so it drops
// to a montserrat_20 icon pinned at the top and one LONG_DOT line at the
// bottom. The button padding is zeroed so these child alignments are exact
// (the default theme's button padding pushed the label into the icon).
static lv_obj_t *make_tile(lv_obj_t *scr, const char *icon, str_id_t label_id,
                            int x, int y, int w, int h, bool horizontal, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_style(b, &s_tile, 0);
    lv_obj_add_style(b, &s_tile_pr, LV_STATE_PRESSED);
    lv_obj_add_style(b, &s_tile_dis, LV_STATE_DISABLED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon_l = lv_label_create(b);
    lv_label_set_text(icon_l, icon);

    lv_obj_t *text_l = lv_label_create(b);
    lv_obj_set_style_text_font(text_l, &bugne_font_14, 0);
    lv_label_set_text(text_l, T(label_id));
    const int line_h = lv_font_get_line_height(&bugne_font_14);

    if (horizontal) {
        lv_obj_set_style_text_font(icon_l, &lv_font_montserrat_28, 0);
        lv_obj_align(icon_l, LV_ALIGN_LEFT_MID, 12, 0);
        const int text_x = 46;  // 12 pad + ~30 icon glyph + 4 gap
        lv_obj_set_size(text_l, w - text_x - 6, line_h);
        lv_obj_set_style_text_align(text_l, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(text_l, LV_LABEL_LONG_DOT);
        lv_obj_align(text_l, LV_ALIGN_LEFT_MID, text_x, 0);
    } else if (h / 2 < 2 * line_h) {
        // Cramped: smaller icon pinned top, one LONG_DOT line pinned bottom.
        lv_obj_set_style_text_font(icon_l, &lv_font_montserrat_20, 0);
        lv_obj_align(icon_l, LV_ALIGN_TOP_MID, 0, 5);
        lv_obj_set_size(text_l, w - 8, line_h);
        lv_obj_set_style_text_align(text_l, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(text_l, LV_LABEL_LONG_DOT);
        lv_obj_align(text_l, LV_ALIGN_BOTTOM_MID, 0, -5);
    } else {
        lv_obj_set_style_text_font(icon_l, &lv_font_montserrat_28, 0);
        lv_obj_align(icon_l, LV_ALIGN_TOP_MID, 0,
                     (h / 2 - lv_font_get_line_height(&lv_font_montserrat_28)) / 2);
        lv_obj_set_width(text_l, w - 8);
        lv_obj_set_style_text_align(text_l, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(text_l, LV_LABEL_LONG_WRAP);
        lv_obj_align(text_l, LV_ALIGN_BOTTOM_MID, 0, -4);
    }
    return b;
}

static void build_home(lv_obj_t *scr)
{
    // Use the configured device name; fall back to the unique "Bugne <id>".
    char t[40];
    const config_t *hc = config_store_get();
    if (hc && hc->device_name[0]) {
        strlcpy(t, hc->device_name, sizeof(t));
    } else {
        snprintf(t, sizeof(t), "Bugne %s", board_device_id());
    }
    // add_title's default width (scr_w()-100) reserves equal room on both
    // sides for screens with a back button AND a gear/refresh icon. Home has
    // only the right-side gear, so reclaim the dead left margin: widen and
    // shift left to fill up to just before the gear (44 px wide, 8 px pad).
    lv_obj_t *title = add_title(scr, t);
    lv_obj_set_width(title, scr_w() - 64);
    lv_obj_align(title, LV_ALIGN_TOP_MID, -24, 8);

    // Settings button (gear), top-right: round, matches the round transport
    // buttons look (s_round_surface).
    lv_obj_t *gear = make_round_btn(scr, LV_SYMBOL_SETTINGS, 44, false, on_open_settings);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -8, 8);

    // Tile grid: margins PAD_SIDE, gutter 10. The grid starts below the round
    // gear button (y 8, height 44, so its bottom edge is at 54) so no tile
    // ever underlaps it, and ends at scr_h()-MINI_CLEAR so the mini bar never
    // covers a tile. Tile sizes are picked per orientation/count to exactly
    // fill that band (tuned against on-device screenshots).
    const bool game_on = (hc && hc->ui.game == 1);
    const bool fav_on = (hc && hc->favorite_count > 0);  // Favorites tile only when some exist
    const bool tuner_on = (hc && hc->ui.tuner == 1);
    const bool ls = scr_w() > scr_h();
    const int M = PAD_SIDE, G = 10, Y0 = 54;
    const int limit = scr_h() - MINI_CLEAR;
    lv_obj_t *wr, *pod, *lib, *sd, *game = NULL, *fav = NULL, *mem = NULL;

    // Extras beyond the 4 base tiles, in display order. Memos is always shown
    // (it greys out without SD like the other SD features), so nb is 1..4.
    const char *ex_icon[4];
    str_id_t ex_id[4];
    lv_event_cb_t ex_cb[4];
    lv_obj_t **ex_out[4];
    int nb = 0;
    if (game_on) {
        ex_icon[nb] = LV_SYMBOL_EDIT; ex_id[nb] = STR_GAME;
        ex_cb[nb] = on_open_game; ex_out[nb] = &game; nb++;
    }
    if (fav_on) {
        ex_icon[nb] = LV_SYMBOL_CHARGE; ex_id[nb] = STR_FAVORITES;
        ex_cb[nb] = on_open_favorites; ex_out[nb] = &fav; nb++;
    }
    if (tuner_on) {
        ex_icon[nb] = LV_SYMBOL_VOLUME_MID; ex_id[nb] = STR_TUNER;
        ex_cb[nb] = on_open_tuner; ex_out[nb] = NULL; nb++;
    }
    ex_icon[nb] = LV_SYMBOL_ENVELOPE; ex_id[nb] = STR_MEMOS;
    ex_cb[nb] = on_open_memos; ex_out[nb] = &mem; nb++;

    if (!ls) {
        // Portrait: a 2x2 grid of vertical tiles plus banner rows below for
        // the extras, packed two per row in display order; a lone last extra
        // spans the full width. One banner row keeps the original 38 px
        // game-banner geometry; two shrink to 34 px each.
        const int rows = (nb + 1) / 2;  // nb is 1..4: one or two rows
        const int banner_h = (rows == 2) ? 34 : 38;
        const int w2 = (scr_w() - 2 * M - G) / 2;
        const int h2 = (limit - Y0 - (1 + rows) * G - rows * banner_h) / 2;
        wr = make_tile(scr, LV_SYMBOL_AUDIO, STR_WEBRADIOS, M, Y0, w2, h2, false, on_open_webradios);
        pod = make_tile(scr, LV_SYMBOL_LIST, STR_PODCASTS, M + w2 + G, Y0, w2, h2, false, on_open_podcasts);
        lib = make_tile(scr, LV_SYMBOL_AUDIO, STR_LIBRARY, M, Y0 + h2 + G, w2, h2, false, on_open_library);
        sd = make_tile(scr, LV_SYMBOL_SD_CARD, STR_SDCARD, M + w2 + G, Y0 + h2 + G, w2, h2, false, on_open_sd);
        const int by = Y0 + 2 * (h2 + G);
        for (int i = 0; i < nb; i++) {
            const bool lone = (i == nb - 1) && (nb % 2 == 1);  // odd count: last row is full width
            const int bx = M + (i % 2) * (w2 + G);
            const int y = by + (i / 2) * (banner_h + G);
            lv_obj_t *b = make_tile(scr, ex_icon[i], ex_id[i], bx, y,
                                    lone ? 2 * w2 + G : w2, banner_h, true, ex_cb[i]);
            if (ex_out[i]) *ex_out[i] = b;
        }
    } else {
        // Landscape: a 3-column top row of vertical mini-tiles; row 2 holds
        // the SD card plus the extras. With 1 extra the pair is centered;
        // 2 extras fill the 3 columns; 3 switch row 2 to 4 narrower cells;
        // with all 4 the memos tile is promoted to the top row and the grid
        // becomes a symmetric 4x2 (row 2 never shrinks below 4 cells).
        const bool e4 = (nb == 4);
        const int w3 = (scr_w() - 2 * M - 2 * G) / 3;
        const int w4 = (scr_w() - 2 * M - 3 * G) / 4;
        const int wt = e4 ? w4 : w3;               // top-row cell width
        const int w2b = (nb >= 3) ? w4 : w3;       // row-2 cell width
        const int h3 = (limit - Y0 - G) / 2;
        const int row2_off = (nb == 1) ? (w3 + G) / 2 : 0;  // centers a 2-tile row
        wr = make_tile(scr, LV_SYMBOL_AUDIO, STR_WEBRADIOS, M, Y0, wt, h3, false, on_open_webradios);
        pod = make_tile(scr, LV_SYMBOL_LIST, STR_PODCASTS, M + wt + G, Y0, wt, h3, false, on_open_podcasts);
        lib = make_tile(scr, LV_SYMBOL_AUDIO, STR_LIBRARY, M + 2 * (wt + G), Y0, wt, h3, false, on_open_library);
        const int n_row2 = e4 ? nb - 1 : nb;
        if (e4) {  // memos (always the last extra) joins the top row
            lv_obj_t *b = make_tile(scr, ex_icon[nb - 1], ex_id[nb - 1],
                                    M + 3 * (wt + G), Y0, wt, h3, false, ex_cb[nb - 1]);
            if (ex_out[nb - 1]) *ex_out[nb - 1] = b;
        }
        sd = make_tile(scr, LV_SYMBOL_SD_CARD, STR_SDCARD, M + row2_off, Y0 + h3 + G, w2b, h3, false, on_open_sd);
        int x5 = M + row2_off + w2b + G;
        for (int i = 0; i < n_row2; i++) {
            lv_obj_t *b = make_tile(scr, ex_icon[i], ex_id[i], x5, Y0 + h3 + G, w2b, h3,
                                    false, ex_cb[i]);
            if (ex_out[i]) *ex_out[i] = b;
            x5 += w2b + G;
        }
    }

    // Grey out web radios until Wi-Fi is connected (they only stream). Podcasts
    // stay enabled: downloaded episodes play offline, the episodes screen greys
    // the rest. The home screen is rebuilt when the connection state changes
    // (see sleep_timer_cb).
    bool q = parental_blocked();  // quiet hours or exhausted daily limit
    if (net_state() != NET_STATE_CONNECTED || q) lv_obj_add_state(wr, LV_STATE_DISABLED);
    if (q) {
        lv_obj_add_state(pod, LV_STATE_DISABLED);
        lv_obj_add_state(lib, LV_STATE_DISABLED);
        lv_obj_add_state(sd, LV_STATE_DISABLED);
        if (game) lv_obj_add_state(game, LV_STATE_DISABLED);
        if (fav) lv_obj_add_state(fav, LV_STATE_DISABLED);
        // The tuner tile stays enabled on purpose: it makes no sound.
    }
    if (mem && (q || !source_sd_present())) lv_obj_add_state(mem, LV_STATE_DISABLED);
    // Discreet unread-memos alert: a small red dot on the memos tile. Computed
    // at build time only; the receive flag, the SD-presence edge and the memo
    // list actions all rebuild home (no per-tick directory scan).
    if (mem && source_sd_present() && memo_unread_count() > 0) {
        lv_obj_t *dot = lv_obj_create(mem);
        lv_obj_set_size(dot, 12, 12);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0xE53935), 0);  // notification red
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(dot, lv_color_white(), 0);    // ring: visible on any accent
        lv_obj_set_style_border_width(dot, 2, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, -4, 4);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    if (net_state() != NET_STATE_CONNECTED) {
        lv_obj_t *hint = lv_label_create(scr);
        char h[48];
        snprintf(h, sizeof(h), LV_SYMBOL_WIFI " %s", T(STR_CONNECTING));
        lv_label_set_text(hint, h);
        muted(hint);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
    } else {
        // Wall clock, in the same free band as the offline hint above (the two
        // are mutually exclusive: this is only built once connected). Created
        // hidden; the 1 Hz tick in sleep_timer_cb sets its text and shows it
        // once a wall time is available and nothing is playing.
        s_home_clock = lv_label_create(scr);
        lv_obj_set_style_text_font(s_home_clock, &bugne_font_20, 0);
        lv_obj_align(s_home_clock, LV_ALIGN_BOTTOM_MID, 0, -(MINI_BAR_GAP + 12));
        lv_obj_add_flag(s_home_clock, LV_OBJ_FLAG_HIDDEN);
    }

    // When the alarm is snoozed, a muted reminder sits just above the clock.
    // Built only when nothing is playing (like the clock): the mini bar would
    // otherwise cover it. An edge-triggered rebuild (sleep_timer_cb) refreshes
    // this when the alarm state changes while home is shown.
    if (s_alarm_state == ALARM_SNOOZED && !audio_is_active()) {
        struct tm tmv;
        localtime_r(&s_alarm_snooze_until, &tmv);
        char sbuf[48];
        snprintf(sbuf, sizeof(sbuf), T(STR_SNOOZE_UNTIL_FMT), tmv.tm_hour, tmv.tm_min);
        lv_obj_t *sn = lv_label_create(scr);
        lv_label_set_text(sn, sbuf);
        muted(sn);
        lv_obj_align(sn, LV_ALIGN_BOTTOM_MID, 0, -(MINI_BAR_GAP + 12 + 26));
    }
}

// Setup (provisioning) ----------------------------------------------------

// A QR code needs a white quiet zone to scan: on the stock dark UI (or a
// pastel light theme) a QR straight on the background does not scan. So this
// draws a fixed-white rounded card of `size` and renders the QR smaller
// inside it, centered, with a comfortable margin. White in both themes: the
// card ignores dark/accent on purpose. `size` is the same footprint callers
// already align, so no caller/layout changes were needed for this.
static lv_obj_t *make_qr(lv_obj_t *parent, const char *data, int size)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, size, size);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    const int margin = 10;  // quiet zone around the QR modules
    lv_obj_t *qr = lv_qrcode_create(card);
    lv_qrcode_set_size(qr, size - 2 * margin);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, data, strlen(data));
    lv_obj_center(qr);
    return card;
}

static void build_setup(lv_obj_t *scr)
{
    char head[48];
    snprintf(head, sizeof(head), T(STR_SETUP_HEADER_FMT), board_device_id());
    // Setup has no back button and no other icon at all (it is the very first
    // screen, before there is anywhere to go back to), so the title can use
    // the full width, centered.
    lv_obj_t *title = add_title(scr, head);
    lv_obj_set_width(title, scr_w() - 16);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // Only the Wi-Fi QR is shown here: the config page lives at 192.168.4.1, which
    // is unreachable until the phone has joined the setup hotspot, and once joined
    // the captive portal opens the config page on its own. A second QR would just
    // confuse, so it is offered separately under Settings > Config page (QR).
    char wifi[160];
    snprintf(wifi, sizeof(wifi), "WIFI:S:Bugne-Setup-%s;T:WPA;P:%s;;",
             board_device_id(), board_ap_password());
    // Landscape: QR on the left, captions in a right-hand column (the portrait
    // vertical stack does not fit 240 px of height). The QR stays 150 px so it
    // remains easy to scan.
    const bool ls = scr_w() > scr_h();
    const int cw = ls ? 148 : scr_w() - 16;  // caption column width
    qr_caption(scr, T(STR_SCAN_JOIN_WIFI),
               ls ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_MID, ls ? -8 : 0, 50, cw);
    lv_obj_t *qr1 = make_qr(scr, wifi, 150);
    if (ls) lv_obj_align(qr1, LV_ALIGN_LEFT_MID, 10, 0);
    else    lv_obj_align(qr1, LV_ALIGN_CENTER, 0, 18);  // nudged down to clear a 2-line caption
    qr_caption(scr, T(STR_CONFIG_AFTER_JOIN),
               ls ? LV_ALIGN_BOTTOM_RIGHT : LV_ALIGN_BOTTOM_MID,
               ls ? -8 : 0, ls ? -8 : -22, cw);
}

// Settings is a small menu of sub-screens.
static lv_obj_t *s_set_lib_lbl;  // library-sync status label (live-updated)
static lv_obj_t *s_set_pod_lbl;  // refresh-all-podcasts status label (live-updated)
static bool s_pod_was_refreshing;  // edge-detect refresh completion for the label

static void on_settings_back(lv_event_t *e) { (void)e; show(build_settings); }

// Sub-screen: QR + URL to the config web page (mDNS host when connected, else the
// setup AP gateway).
static void build_settings_web(lv_obj_t *scr)
{
    add_back_cb(scr, on_settings_back);
    add_title_wide(scr, T(STR_CONFIG_PAGE));
    char url[40];
    if (net_state() == NET_STATE_CONNECTED) {
        char id[8];
        strlcpy(id, board_device_id(), sizeof(id));
        for (char *p = id; *p; p++) *p = (char)tolower((unsigned char)*p);
        snprintf(url, sizeof(url), "http://bugne-%s.local", id);
    } else {
        snprintf(url, sizeof(url), "http://192.168.4.1");
    }
    // The QR encodes the raw IP when known: phones open it without depending
    // on mDNS (.local names fail on some Android). The friendlier .local URL
    // stays as the displayed text below.
    char qurl[40];
    char qip[20];
    if (net_state() == NET_STATE_CONNECTED && net_ip(qip, sizeof(qip))) {
        snprintf(qurl, sizeof(qurl), "http://%s", qip);
    } else {
        strlcpy(qurl, url, sizeof(qurl));
    }
    // Landscape: QR left, caption/URL/IP in a right-hand column (see build_setup).
    const bool ls = scr_w() > scr_h();
    const int cw = ls ? 148 : scr_w() - 16;
    qr_caption(scr, T(STR_SCAN_CONFIG_PAGE),
               ls ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_MID, ls ? -8 : 0, 50, cw);
    lv_obj_t *qr = make_qr(scr, qurl, 150);
    if (ls) lv_obj_align(qr, LV_ALIGN_LEFT_MID, 10, 0);
    else    lv_obj_align(qr, LV_ALIGN_CENTER, 0, 18);  // nudged down to clear a 2-line caption
    // The URL wraps in the narrow landscape column, so use a wrapping caption.
    qr_caption(scr, url, ls ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_BOTTOM_MID,
               ls ? -8 : 0, ls ? 120 : -22, cw);

    // Also show the raw IP, in case mDNS (the .local name) is not resolvable.
    char ip[20];
    if (net_ip(ip, sizeof(ip))) {
        lv_obj_t *ipl = lv_label_create(scr);
        muted(ipl);
        char line[40];
        snprintf(line, sizeof(line), "IP: %s", ip);
        lv_label_set_text(ipl, line);
        if (ls) lv_obj_align(ipl, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
        else    lv_obj_align(ipl, LV_ALIGN_BOTTOM_MID, 0, -4);
    }
}

// Sub-screen: QR to join the setup hotspot (only beaconing while in AP mode).
static void build_settings_ap(lv_obj_t *scr)
{
    add_back_cb(scr, on_settings_back);
    add_title_wide(scr, T(STR_SETUP_WIFI));
    char wifi[160];
    snprintf(wifi, sizeof(wifi), "WIFI:S:Bugne-Setup-%s;T:WPA;P:%s;;",
             board_device_id(), board_ap_password());
    // Landscape: QR left, captions in a right-hand column (see build_setup).
    const bool ls = scr_w() > scr_h();
    const int cw = ls ? 148 : scr_w() - 16;
    qr_caption(scr, T(STR_SCAN_HOTSPOT),
               ls ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_MID, ls ? -8 : 0, 50, cw);
    lv_obj_t *qr = make_qr(scr, wifi, 150);
    if (ls) lv_obj_align(qr, LV_ALIGN_LEFT_MID, 10, 0);
    else    lv_obj_align(qr, LV_ALIGN_CENTER, 0, 18);  // nudged down to clear a 2-line caption
    if (net_state() != NET_STATE_PROVISIONING) {
        qr_caption(scr, T(STR_ACTIVE_DURING_SETUP),
                   ls ? LV_ALIGN_BOTTOM_RIGHT : LV_ALIGN_BOTTOM_MID,
                   ls ? -8 : 0, ls ? -8 : -20, cw);
    }
}

static void on_set_lib_scan(lv_event_t *e)
{
    (void)e;
    if (library_scan_start() && s_set_lib_lbl) {
        lv_label_set_text(s_set_lib_lbl, T(STR_SCANNING_SD));
    }
}

// Sub-screen: music library status + a synchronize button.
static void build_settings_library(lv_obj_t *scr)
{
    add_back_cb(scr, on_settings_back);
    add_title_wide(scr, T(STR_MUSIC_LIBRARY));
    if (library_track_count() == 0) library_load();
    lv_obj_t *info = lv_label_create(scr);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(info, scr_w() - 20);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(info, LV_ALIGN_CENTER, 0, -30);
    muted(info);
    s_set_lib_lbl = info;
    char t[64];
    if (library_scanning()) strlcpy(t, T(STR_SCANNING_SD), sizeof(t));
    else snprintf(t, sizeof(t), T(STR_TRACKS_INDEXED_FMT), (unsigned)library_track_count());
    lv_label_set_text(info, t);

    lv_obj_t *b = lv_button_create(scr);
    lv_obj_set_size(b, 200, 50);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, 30);
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, T(STR_SYNC_NOW));
    lv_obj_center(bl);
    lv_obj_add_event_cb(b, on_set_lib_scan, LV_EVENT_CLICKED, NULL);
}

// Refresh every configured podcast feed on the worker. Blocked while audio plays
// (the refresh shares the playback worker), matching the per-podcast refresh.
static void on_set_pod_refresh(lv_event_t *e)
{
    (void)e;
    if (audio_is_active()) {
        if (s_set_pod_lbl) lv_label_set_text(s_set_pod_lbl, T(STR_STOP_PLAYBACK_FIRST));
        return;
    }
    if (s_refreshing || s_downloading) return;  // see on_refresh: a download holds
                                                 // the worker; don't queue behind it
    s_refreshing = true;
    s_pod_was_refreshing = true;
    play_req_t req = { .kind = REQ_REFRESH_ALL };
    xQueueOverwrite(s_play_q, &req);
    if (s_set_pod_lbl) lv_label_set_text(s_set_pod_lbl, T(STR_REFRESHING_ALL));
}

// Sub-screen: refresh all podcast feeds.
static void build_settings_podcasts(lv_obj_t *scr)
{
    add_back_cb(scr, on_settings_back);
    add_title_wide(scr, T(STR_PODCASTS));
    const config_t *c = config_store_get();
    lv_obj_t *info = lv_label_create(scr);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(info, scr_w() - 20);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(info, LV_ALIGN_CENTER, 0, -30);
    muted(info);
    s_set_pod_lbl = info;
    char t[64];
    if (s_refreshing) strlcpy(t, T(STR_REFRESHING_ALL), sizeof(t));
    else snprintf(t, sizeof(t), T(STR_PODCAST_FEEDS_FMT), (unsigned)(c ? c->podcast_count : 0));
    lv_label_set_text(info, t);

    lv_obj_t *b = lv_button_create(scr);
    lv_obj_set_size(b, 200, 50);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, 30);
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, T(STR_REFRESH_ALL_FEEDS));
    lv_obj_center(bl);
    lv_obj_add_event_cb(b, on_set_pod_refresh, LV_EVENT_CLICKED, NULL);
}

static void on_set_web(lv_event_t *e) { (void)e; show(build_settings_web); }
static void on_set_ap(lv_event_t *e)  { (void)e; show(build_settings_ap); }
static void on_set_lib(lv_event_t *e) { (void)e; show(build_settings_library); }
static void on_set_pod(lv_event_t *e) { (void)e; show(build_settings_podcasts); }

// Flip portrait/landscape and persist. Only the config is written here: the
// sleep_timer_cb poll applies it within one tick (same path as a web-side
// change), rotating the display and rebuilding this screen. Safe to write
// flash from this callback: the LVGL task stack is internal RAM.
static void on_toggle_orientation(lv_event_t *e)
{
    (void)e;
    config_store_set_orientation(s_landscape ? 0 : 1);
}

static void on_open_theme(lv_event_t *e) { (void)e; show(build_settings_theme); }

// Theme picker taps. Only the config is written here (same one-code-path rule
// as on_toggle_orientation): the sleep_timer_cb poll applies it within one
// tick, which is the instant live preview. Flash write is safe from the LVGL
// task. Each tap changes one of the two choices and keeps the other.
static void on_theme_mode(lv_event_t *e)
{
    config_store_set_theme((int)(intptr_t)lv_event_get_user_data(e), s_accent_applied);
}

static void on_theme_accent(lv_event_t *e)
{
    config_store_set_theme(s_dark_applied, (int)(intptr_t)lv_event_get_user_data(e));
}

// Label helper: prefix with a checkmark when this choice is the active one.
static void theme_check_label(lv_obj_t *btn, str_id_t id, bool checked)
{
    lv_obj_t *l = lv_label_create(btn);
    char t[40];
    if (checked) snprintf(t, sizeof(t), LV_SYMBOL_OK " %s", T(id));
    else strlcpy(t, T(id), sizeof(t));
    lv_label_set_text(l, t);
    lv_obj_center(l);
}

// Sub-screen: dark/light mode row plus 5 accent color swatches, instant apply.
static void build_settings_theme(lv_obj_t *scr)
{
    add_back_cb(scr, on_settings_back);
    add_title_wide(scr, T(STR_THEME));
    const bool ls = scr_w() > scr_h();

    // Mode row: Light and Dark, styled as previews of themselves.
    static const str_id_t MODES[2] = { STR_THEME_LIGHT, STR_THEME_DARK };
    for (int m = 0; m < 2; m++) {
        lv_obj_t *b = lv_button_create(scr);
        lv_obj_set_size(b, 100, 50);
        lv_obj_align(b, LV_ALIGN_TOP_MID, m ? 55 : -55, 52);
        lv_obj_set_style_bg_color(b, m ? lv_color_hex(0x282b30) : lv_color_white(), 0);
        lv_obj_set_style_text_color(b, m ? lv_color_white() : lv_color_black(), 0);
        lv_obj_set_style_border_color(b, lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        theme_check_label(b, MODES[m], s_dark_applied == m);
        lv_obj_add_event_cb(b, on_theme_mode, LV_EVENT_CLICKED, (void *)(intptr_t)m);
    }

    // Accent swatches: portrait 3+2 grid, landscape one row of 5.
    for (int i = 0; i < ACCENT_COUNT; i++) {
        int x, y;
        if (ls) { x = (i - 2) * 63; y = 130; }
        else    { x = (i < 3) ? (i - 1) * 72 : (i == 3) ? -36 : 36; y = 130 + (i / 3) * 64; }
        lv_obj_t *b = lv_button_create(scr);
        lv_obj_set_size(b, ls ? 60 : 68, 56);
        lv_obj_align(b, LV_ALIGN_TOP_MID, x, y);
        lv_obj_set_style_radius(b, 14, 0);  // rounder than the RADIUS_BTN default, a swatch not a button
        // Local styles beat the theme layer, so the swatch keeps its own color.
        // The active accent is marked with a thick white border (a checkmark
        // prefix would not fit the narrow swatch).
        lv_obj_set_style_bg_color(b, lv_palette_main(ACCENTS[i].primary), 0);
        lv_obj_set_style_text_color(b, lv_color_white(), 0);
        if (i == s_accent_applied) {
            lv_obj_set_style_border_color(b, lv_color_white(), 0);
            lv_obj_set_style_border_width(b, 3, 0);
        }
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, T(ACCENTS[i].name));
        lv_obj_center(l);
        lv_obj_add_event_cb(b, on_theme_accent, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

// ---- Alarm ringing screen (stub Stop/Snooze; the engine chunk replaces them) ----

// Stop the alarm: end playback/beep and restore the user's volume. Guard for a
// nav-opened ringing screen while idle (state != RINGING): just go home.
static void on_alarm_stop(lv_event_t *e)
{
    (void)e;
    if (s_alarm_state == ALARM_RINGING) alarm_finish(false);
    else show(build_home);
}

// Snooze: schedule a +10 min re-fire before leaving (same idle guard).
static void on_alarm_snooze(lv_event_t *e)
{
    (void)e;
    if (s_alarm_state == ALARM_RINGING) alarm_finish(true);
    else show(build_home);
}

// What the active alarm (s_alarm_active_idx) is set to play, for the muted
// line under the clock: the configured radio's name (looked up by the stable
// id, not the array index), the chosen SD track's title, or the beep
// fallback name when unresolved.
static void alarm_source_text(char *buf, size_t sz)
{
    const config_t *c = config_store_get();
    if (!c) { strlcpy(buf, T(STR_ALARM_BEEP), sz); return; }
    const config_alarm_t *ca = &c->alarms[s_alarm_active_idx];
    if (ca->source == 0) {
        for (size_t i = 0; i < c->webradio_count; i++) {
            if (c->webradios[i].id == ca->radio_id) {
                strlcpy(buf, c->webradios[i].name, sz);
                return;
            }
        }
        strlcpy(buf, T(STR_ALARM_BEEP), sz);
    } else {
        strlcpy(buf, ca->sd_title[0] ? ca->sd_title : T(STR_ALARM_SD_UNSET), sz);
    }
}

// No back button: only Stop and Snooze may leave this screen, so a child
// mashing the glass cannot dismiss the alarm any other way. Portrait stacks
// bell/clock/source/Stop/Snooze; landscape splits clock+source on the left
// from Stop+Snooze on the right (screen too short to stack all five rows).
static void build_alarm_ringing(lv_obj_t *scr)
{
    const bool ls = scr_w() > scr_h();

    lv_obj_t *title = lv_label_create(scr);
    char tbuf[40];
    snprintf(tbuf, sizeof(tbuf), LV_SYMBOL_BELL " %s", T(STR_ALARM));
    lv_label_set_text(title, tbuf);
    lv_obj_set_width(title, scr_w() - 20);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    muted(title);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *clock = lv_label_create(scr);
    lv_obj_set_style_text_font(clock, &bugne_font_48, 0);
    time_t nowt = time(NULL);
    struct tm tmv;
    localtime_r(&nowt, &tmv);
    char cbuf[8];
    snprintf(cbuf, sizeof(cbuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    lv_label_set_text(clock, cbuf);
    s_alarm_time_lbl = clock;  // refreshed once a second by sleep_timer_cb

    char srcbuf[64];
    alarm_source_text(srcbuf, sizeof(srcbuf));
    lv_obj_t *src = lv_label_create(scr);
    // Wrap to a second line instead of truncating: French names run longer
    // (e.g. "Sonnerie integree") than the narrow landscape left column allows
    // on one line.
    lv_label_set_long_mode(src, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(src, (ls ? scr_w() / 2 : scr_w()) - 2 * PAD_SIDE,
                    2 * lv_font_get_line_height(&bugne_font_14));
    lv_obj_set_style_text_align(src, LV_TEXT_ALIGN_CENTER, 0);
    muted(src);
    lv_label_set_text(src, srcbuf);
    s_alarm_src_lbl = src;  // engine switches it to STR_ALARM_BEEP on beep takeover

    lv_obj_t *stop = make_round_btn(scr, LV_SYMBOL_STOP, 76, true, on_alarm_stop);

    lv_obj_t *snooze = lv_button_create(scr);
    lv_obj_set_size(snooze, 130, 44);
    lv_obj_t *sl = lv_label_create(snooze);
    lv_label_set_text(sl, "+10 min");
    lv_obj_center(sl);
    lv_obj_add_event_cb(snooze, on_alarm_snooze, LV_EVENT_CLICKED, NULL);

    if (ls) {
        // Two columns that must never intersect: "23:58" at 48 px is ~150 px
        // wide, so the clock (left, from x 12) ends by ~162 while the right
        // column (STOP and the pill, both centered on x = 3/4 of the width,
        // 240 on a 320 px panel) starts at 175 (the 130 px pill's left edge).
        lv_obj_align(clock,  LV_ALIGN_LEFT_MID, 12, -30);
        lv_obj_align(src,    LV_ALIGN_LEFT_MID, 12, 30);
        lv_obj_align(stop,   LV_ALIGN_CENTER, scr_w() / 4, -32);
        lv_obj_align(snooze, LV_ALIGN_CENTER, scr_w() / 4, 46);
    } else {
        // Vertical stack with explicit clearances: clock glyphs end by ~104,
        // the two source lines by ~136; the 76 px STOP (center y 190) spans
        // 152..228, so >= 16 px to the source above and 28 px to the pill
        // below (top edge 256).
        lv_obj_align(clock,  LV_ALIGN_TOP_MID, 0, 52);
        lv_obj_align(src,    LV_ALIGN_TOP_MID, 0, 108);
        lv_obj_align(stop,   LV_ALIGN_CENTER, 0, 30);
        lv_obj_align(snooze, LV_ALIGN_BOTTOM_MID, 0, -20);
    }
}

// Minimal sunrise-light screen (C1): big HH:MM on a pure black background.
// Local styles only (screen-lifetime), so there is nothing to refill in
// apply_theme. No buttons on purpose: any touch rides the normal wake path
// (exit_sleep) and the sunrise block in sleep_timer_cb cancels the ramp.
// Reuses s_alarm_time_lbl, so the 1 Hz tick keeps the clock current.
static void build_sunrise(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_t *clock = lv_label_create(scr);
    lv_obj_set_style_text_font(clock, &bugne_font_48, 0);
    lv_obj_set_style_text_color(clock, lv_color_white(), 0);
    time_t nowt = time(NULL);
    struct tm tmv;
    localtime_r(&nowt, &tmv);
    char cbuf[8];
    snprintf(cbuf, sizeof(cbuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    lv_label_set_text(clock, cbuf);
    lv_obj_center(clock);
    s_alarm_time_lbl = clock;  // refreshed once a second by sleep_timer_cb
}

// ---- Alarm settings screen ----

// A "card" row: a rounded SURFACE container (s_th_row, same look as a list
// row) laid out as a horizontal flex box. Not scrollable itself: the screen's
// content container is the one scrollable area.
static lv_obj_t *alarm_row(lv_obj_t *parent, lv_flex_align_t main_align)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(row, &s_th_row, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, main_align, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    return row;
}

// Recompute the muted summary line from the live widget states (not yet-saved
// config), so it reflects taps immediately: the time warning when the alarm is
// on but the clock never synced, else "HH:MM" plus the checked day letters.
static void alarm_status_refresh(void)
{
    if (!s_as_status_lbl) return;
    bool enabled = s_as_switch && lv_obj_has_state(s_as_switch, LV_STATE_CHECKED);
    char buf[64];
    if (enabled && !time_valid()) {
        strlcpy(buf, T(STR_TIME_NOT_SET), sizeof(buf));
    } else {
        int hour = s_as_hour_roller ? (int)lv_roller_get_selected(s_as_hour_roller) : 0;
        int min  = s_as_min_roller  ? (int)lv_roller_get_selected(s_as_min_roller)  : 0;
        const char *letters = T(STR_DAY_LETTERS);
        char days[8];
        int di = 0;
        for (int i = 0; i < 7 && di < 7; i++) {
            if (s_as_day_btn[i] && lv_obj_has_state(s_as_day_btn[i], LV_STATE_CHECKED)) {
                days[di++] = letters[i];
            }
        }
        days[di] = '\0';
        snprintf(buf, sizeof(buf), "%02d:%02d  %s", hour, min, days);
    }
    lv_label_set_text(s_as_status_lbl, buf);
}

// Shared save path for every control on this screen: read the live widget
// states into a config_alarm_t and persist it (config_store_set_alarm is the
// one write path, same as the web page). sd_path/sd_title are carried over
// unchanged (choosing an SD track is web-only; on-device you only switch to
// an already-chosen one).
static void alarm_settings_save(void)
{
    config_alarm_t a = config_store_get()->alarms[s_alarm_edit_idx];
    a.enabled = (s_as_switch && lv_obj_has_state(s_as_switch, LV_STATE_CHECKED)) ? 1 : 0;
    if (s_as_hour_roller) a.hour = (int)lv_roller_get_selected(s_as_hour_roller);
    if (s_as_min_roller)  a.minute = (int)lv_roller_get_selected(s_as_min_roller);
    int days = 0;
    for (int i = 0; i < 7; i++) {
        if (s_as_day_btn[i] && lv_obj_has_state(s_as_day_btn[i], LV_STATE_CHECKED)) days |= (1 << i);
    }
    a.days = days;
    a.source = s_as_src_mode;
    if (s_as_src_mode == 0) a.radio_id = s_as_src_radio_id;
    if (s_as_vol_slider) a.volume = (int)lv_slider_get_value(s_as_vol_slider);
    config_store_set_alarm(s_alarm_edit_idx, &a);
    alarm_status_refresh();
}

static void on_alarm_switch(lv_event_t *e) { (void)e; alarm_settings_save(); }
static void on_alarm_roller(lv_event_t *e) { (void)e; alarm_settings_save(); }
static void on_alarm_day(lv_event_t *e)    { (void)e; alarm_settings_save(); }
static void on_alarm_volume(lv_event_t *e) { (void)e; alarm_settings_save(); }

static void on_alarm_src_radio(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const config_t *c = config_store_get();
    if (!c || idx < 0 || (size_t)idx >= c->webradio_count) return;
    s_as_src_mode = 0;
    s_as_src_radio_id = c->webradios[idx].id;
    alarm_settings_save();
    show(build_alarm_edit);  // rebuild so the LV_SYMBOL_OK mark moves
}

static void on_alarm_src_sd(lv_event_t *e)
{
    (void)e;
    const config_t *c = config_store_get();
    if (!c || !c->alarms[s_alarm_edit_idx].sd_path[0]) return;  // row is disabled in this case anyway
    s_as_src_mode = 1;
    alarm_settings_save();
    show(build_alarm_edit);
}

// Back from the single-alarm editor: return to the 3-alarm list, not straight
// to Settings (on_settings_back), so the list keeps reflecting the edit.
static void on_alarm_edit_back(lv_event_t *e) { (void)e; show(build_settings_alarm); }

// Scrollable single-alarm editor (s_alarm_edit_idx, set by the list row that
// opened it): enable switch, hour/minute rollers, 7 day chips, a source
// picker (radios + the chosen SD track) and a volume slider, each writing
// config_store_set_alarm() on change (device UI and the web page share that
// one path; the alarm engine just reads it back).
static void build_alarm_edit(lv_obj_t *scr)
{
    add_back_cb(scr, on_alarm_edit_back);
    char title[24];
    snprintf(title, sizeof(title), T(STR_ALARM_N_FMT), s_alarm_edit_idx + 1);
    add_title_wide(scr, title);

    const config_t *c = config_store_get();
    const config_alarm_t *ca = &c->alarms[s_alarm_edit_idx];
    s_as_src_mode = ca->source;
    s_as_src_radio_id = ca->radio_id;

    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_pos(content, 0, 52);
    lv_obj_set_size(content, scr_w(), scr_h() - 52);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_hor(content, PAD_SIDE, 0);
    lv_obj_set_style_pad_top(content, 4, 0);
    lv_obj_set_style_pad_bottom(content, MINI_CLEAR, 0);
    lv_obj_set_style_pad_row(content, 10, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    // Enable.
    lv_obj_t *row_en = alarm_row(content, LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_obj_t *en_lbl = lv_label_create(row_en);
    // Truncate rather than overlap the switch: the French label runs longer.
    lv_label_set_long_mode(en_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_height(en_lbl, lv_font_get_line_height(&bugne_font_14));
    lv_obj_set_flex_grow(en_lbl, 1);
    lv_label_set_text(en_lbl, T(STR_ALARM_ENABLED));
    lv_obj_t *sw = lv_switch_create(row_en);
    if (ca->enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, on_alarm_switch, LV_EVENT_VALUE_CHANGED, NULL);
    s_as_switch = sw;

    // Time: hour/minute rollers with a colon between them.
    lv_obj_t *row_time = alarm_row(content, LV_FLEX_ALIGN_CENTER);
    char hbuf[100];
    int hp = 0;
    for (int h = 0; h < 24; h++) hp += snprintf(hbuf + hp, sizeof(hbuf) - hp, h ? "\n%02d" : "%02d", h);
    char mbuf[240];
    int mp = 0;
    for (int m = 0; m < 60; m++) mp += snprintf(mbuf + mp, sizeof(mbuf) - mp, m ? "\n%02d" : "%02d", m);

    lv_obj_t *hr = lv_roller_create(row_time);
    lv_obj_set_width(hr, 64);
    lv_roller_set_options(hr, hbuf, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(hr, 3);
    lv_roller_set_selected(hr, (uint32_t)ca->hour, LV_ANIM_OFF);
    lv_obj_add_event_cb(hr, on_alarm_roller, LV_EVENT_VALUE_CHANGED, NULL);
    s_as_hour_roller = hr;

    lv_obj_t *colon = lv_label_create(row_time);
    lv_label_set_text(colon, ":");

    lv_obj_t *mr = lv_roller_create(row_time);
    lv_obj_set_width(mr, 64);
    lv_roller_set_options(mr, mbuf, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(mr, 3);
    lv_roller_set_selected(mr, (uint32_t)ca->minute, LV_ANIM_OFF);
    lv_obj_add_event_cb(mr, on_alarm_roller, LV_EVENT_VALUE_CHANGED, NULL);
    s_as_min_roller = mr;

    // Days: 7 round checkable chips, Monday first.
    lv_obj_t *row_days = alarm_row(content, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_days, 3, 0);
    const char *letters = T(STR_DAY_LETTERS);
    for (int i = 0; i < 7; i++) {
        lv_obj_t *chip = lv_button_create(row_days);
        lv_obj_set_size(chip, 28, 28);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_add_style(chip, &s_round_surface, 0);
        lv_obj_add_style(chip, &s_round_surface_pr, LV_STATE_PRESSED);
        lv_obj_add_style(chip, &s_chip_ck, LV_STATE_CHECKED);
        if (ca->days & (1 << i)) lv_obj_add_state(chip, LV_STATE_CHECKED);
        lv_obj_t *cl = lv_label_create(chip);
        char ch[2] = { letters[i], '\0' };
        lv_label_set_text(cl, ch);
        lv_obj_center(cl);
        lv_obj_add_event_cb(chip, on_alarm_day, LV_EVENT_CLICKED, NULL);
        s_as_day_btn[i] = chip;
    }

    // Source: a caption plus card rows (lv_list, so rows get the same readable
    // SURFACE look as any other list without a local style), one per
    // configured radio and a final SD row. Not scrollable itself (see
    // alarm_row); the current pick shows an LV_SYMBOL_OK prefix.
    lv_obj_t *src_lbl = lv_label_create(content);
    lv_label_set_text(src_lbl, T(STR_ALARM_SOURCE));
    muted(src_lbl);

    lv_obj_t *list = lv_list_create(content);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    for (size_t i = 0; i < c->webradio_count; i++) {
        char label[80];
        bool sel = (ca->source == 0 && ca->radio_id == c->webradios[i].id);
        if (sel) snprintf(label, sizeof(label), LV_SYMBOL_OK " %s", c->webradios[i].name);
        else strlcpy(label, c->webradios[i].name, sizeof(label));
        lv_obj_t *btn = lv_list_add_button(list, LV_SYMBOL_AUDIO, label);
        lv_obj_add_event_cb(btn, on_alarm_src_radio, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    {
        char label[80];
        bool sel = (ca->source == 1);
        const char *name = ca->sd_title[0] ? ca->sd_title : T(STR_ALARM_SD_UNSET);
        if (sel) snprintf(label, sizeof(label), LV_SYMBOL_OK " %s", name);
        else strlcpy(label, name, sizeof(label));
        lv_obj_t *btn = lv_list_add_button(list, LV_SYMBOL_SD_CARD, label);
        if (!ca->sd_path[0]) lv_obj_add_state(btn, LV_STATE_DISABLED);
        lv_obj_add_event_cb(btn, on_alarm_src_sd, LV_EVENT_CLICKED, NULL);
    }

    // Volume.
    lv_obj_t *row_vol = alarm_row(content, LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_obj_t *vol_lbl = lv_label_create(row_vol);
    // Truncate rather than overlap the slider: the French label runs longer.
    lv_label_set_long_mode(vol_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_height(vol_lbl, lv_font_get_line_height(&bugne_font_14));
    lv_obj_set_flex_grow(vol_lbl, 1);
    lv_label_set_text(vol_lbl, T(STR_ALARM_VOLUME));
    lv_obj_t *vol = lv_slider_create(row_vol);
    lv_obj_set_width(vol, 90);
    lv_slider_set_range(vol, 5, audio_get_volume_limit());
    lv_slider_set_value(vol, ca->volume, LV_ANIM_OFF);
    lv_obj_add_event_cb(vol, on_alarm_volume, LV_EVENT_VALUE_CHANGED, NULL);
    s_as_vol_slider = vol;

    // Muted summary / warning, last.
    lv_obj_t *status = lv_label_create(content);
    lv_obj_set_width(status, LV_PCT(100));
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    muted(status);
    s_as_status_lbl = status;
    alarm_status_refresh();
}

static void on_alarm_row_click(lv_event_t *e)
{
    s_alarm_edit_idx = (int)(intptr_t)lv_event_get_user_data(e);
    show(build_alarm_edit);
}

// Alarm list: one card row per alarms[] entry (time, on/off, day letters),
// tapping opens build_alarm_edit bound to that index. This is the settings
// screen's entry point ("settings_alarm" in NAV_SCREENS / POST /api/debug/nav),
// unchanged from the single-alarm days of this screen.
static void build_settings_alarm(lv_obj_t *scr)
{
    add_back_cb(scr, on_settings_back);
    add_title_wide(scr, T(STR_ALARM));

    const config_t *c = config_store_get();

    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_pos(content, 0, 52);
    lv_obj_set_size(content, scr_w(), scr_h() - 52);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_hor(content, PAD_SIDE, 0);
    lv_obj_set_style_pad_top(content, 4, 0);
    lv_obj_set_style_pad_bottom(content, MINI_CLEAR, 0);
    lv_obj_set_style_pad_row(content, 10, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *list = lv_list_create(content);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    const char *letters = T(STR_DAY_LETTERS);
    for (int i = 0; i < CFG_MAX_ALARMS; i++) {
        const config_alarm_t *ca = &c->alarms[i];
        char name[24];
        snprintf(name, sizeof(name), T(STR_ALARM_N_FMT), i + 1);
        char label[64];
        if (!ca->enabled) {
            snprintf(label, sizeof(label), "%s  %02d:%02d  %s", name, ca->hour, ca->minute, T(STR_ALARM_OFF));
        } else {
            char days[8];
            int di = 0;
            for (int d = 0; d < 7 && di < 7; d++) {
                if (ca->days & (1 << d)) days[di++] = letters[d];
            }
            days[di] = '\0';
            snprintf(label, sizeof(label), "%s  %02d:%02d  %s", name, ca->hour, ca->minute, days);
        }
        lv_obj_t *btn = lv_list_add_button(list, LV_SYMBOL_BELL, label);
        lv_obj_add_event_cb(btn, on_alarm_row_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void on_open_settings_alarm(lv_event_t *e) { (void)e; show(build_settings_alarm); }

static void build_settings(lv_obj_t *scr)
{
    add_back_button(scr);
    // Two round 44 px icon buttons sit top-right here (rot at -8, theme at -60,
    // 8 px gap between them, same rhythm as the back button's 8 px inset), so the
    // default title width would underlap them. Center the title in the free gap
    // instead. Portrait: back button right edge at 52, theme button left edge at
    // scr_w()-104 (136 on a 240 px panel) leaves an 84 px frame, minus padding
    // ~76 px; "Reglages" needs 103 px at the 20 px font, so it still does not
    // fit and portrait keeps the 14 px body font. Landscape has a much wider
    // frame (back to 320-104=216) and keeps the 20 px font.
    lv_obj_t *title = add_title(scr, T(STR_SETTINGS));
    if (scr_w() > scr_h()) {
        lv_obj_set_width(title, 138);
        lv_obj_align(title, LV_ALIGN_TOP_MID, -30, 8);
    } else {
        lv_obj_set_style_text_font(title, &bugne_font_14, 0);
        lv_obj_set_width(title, 76);
        lv_obj_align(title, LV_ALIGN_TOP_MID, -26, 12);
    }
    s_set_lib_lbl = NULL;
    s_set_pod_lbl = NULL;

    // Orientation toggle, top-right (same spot and size as the home gear).
    lv_obj_t *rot = make_round_btn(scr, LV_SYMBOL_LOOP, 44, false, on_toggle_orientation);
    lv_obj_align(rot, LV_ALIGN_TOP_RIGHT, -8, 8);

    // Theme picker, next to the orientation toggle, 8 px gap.
    lv_obj_t *th = make_round_btn(scr, LV_SYMBOL_TINT, 44, false, on_open_theme);
    lv_obj_align(th, LV_ALIGN_TOP_RIGHT, -60, 8);
    int bx, by;
    menu_pos(0, 5, &bx, &by);
    add_menu_button_t(scr, LV_SYMBOL_WIFI, STR_CONFIG_PAGE_QR, bx, by, 40, on_set_web);
    menu_pos(1, 5, &bx, &by);
    add_menu_button_t(scr, LV_SYMBOL_WIFI, STR_SETUP_HOTSPOT_QR, bx, by, 40, on_set_ap);
    menu_pos(2, 5, &bx, &by);
    add_menu_button_t(scr, LV_SYMBOL_AUDIO, STR_SYNC_LIBRARY, bx, by, 40, on_set_lib);
    menu_pos(3, 5, &bx, &by);
    add_menu_button_t(scr, LV_SYMBOL_REFRESH, STR_REFRESH_PODCASTS, bx, by, 40, on_set_pod);
    menu_pos(4, 5, &bx, &by);
    add_menu_button_t(scr, LV_SYMBOL_BELL, STR_ALARM, bx, by, 40, on_open_settings_alarm);
}

// ---- Toast (brief top-layer message, survives screen changes) ----

static void toast_del_cb(lv_timer_t *t)
{
    lv_obj_delete((lv_obj_t *)lv_timer_get_user_data(t));
}

// Show a short auto-dismissing message above the mini bar. For cues that have
// no natural screen to live on (e.g. end of a list after navigating away).
static void toast(const char *text)
{
    lv_obj_t *box = lv_obj_create(lv_layer_top());
    lv_obj_set_style_text_font(box, &bugne_font_14, 0);  // top layer: set it here
    lv_obj_set_style_radius(box, RADIUS_TOAST, 0);
    lv_obj_set_style_bg_color(box, col_surface(), 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_ver(box, 12, 0);
    lv_obj_set_style_pad_hor(box, 14, 0);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(box, scr_w() - 20, 0);
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -MINI_CLEAR);  // clear of the mini bar
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(box);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_timer_t *t = lv_timer_create(toast_del_cb, 3000, box);
    lv_timer_set_repeat_count(t, 1);  // LVGL deletes the timer after one run
}

// ---- Now-playing mini bar (persistent across screens) ----

typedef enum { NP_NONE, NP_LOCAL, NP_SENDSPIN } np_source_t;

// What is currently playing. Sendspin wins if engaged; otherwise any open local
// source (the audio layer is open between audio_open/close).
static np_source_t np_current(void)
{
    // A memo playing/previewing stays out of the mini bar and the home clock
    // logic: it is a seconds-long message tied to its own screen, and the
    // s_now_* metadata still describes the previous source.
    if (audio_arbiter_active() == AUDIO_SOURCE_MEMO) return NP_NONE;
    np_source_t real = source_sendspin_session_active() ? NP_SENDSPIN
                     : audio_is_active() ? NP_LOCAL : NP_NONE;
    if (s_user_stopped) {
        // Keep reporting "nothing playing" until the source has finished tearing
        // down, then re-arm so the next playback shows the bar normally.
        if (real == NP_NONE) s_user_stopped = false;
        return NP_NONE;
    }
    return real;
}

static bool np_is_paused(np_source_t np)
{
    return (np == NP_SENDSPIN) ? !source_sendspin_active() : audio_is_paused();
}

static void np_fill_title(np_source_t np, char *buf, size_t size)
{
    if (np == NP_SENDSPIN) {
        ss_fill_title(buf, size);
    } else {
        // Prefer the SD tag title when known; else the file name / episode title.
        strlcpy(buf, s_meta_title[0] ? s_meta_title : s_now_title, size);
    }
}

static void on_mini_pause(lv_event_t *e)
{
    (void)e;
    if (source_sendspin_session_active()) {
        source_sendspin_command(source_sendspin_active() ? SENDSPIN_CMD_PAUSE : SENDSPIN_CMD_PLAY);
    } else {
        audio_set_paused(!audio_is_paused());
    }
}

// Tapping the bar opens the relevant full now-playing screen.
static void on_mini_open(lv_event_t *e)
{
    (void)e;
    if (source_sendspin_session_active()) show(build_sendspin_playing);
    else if (audio_is_active()) show(build_now_playing);
}

static void create_mini_bar(void)
{
    // Floating rounded accent card. ALL its geometry lives here: rotation and
    // theme switches delete + recreate it (sleep_timer_cb), the latter because
    // the accent bg below is a local style that report_style_change cannot
    // refresh.
    s_mini = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_mini, scr_w() - 2 * MINI_BAR_GAP, MINI_BAR_H);
    lv_obj_align(s_mini, LV_ALIGN_BOTTOM_MID, 0, -MINI_BAR_GAP);
    // The top layer is not covered by show()'s screen font, so set it here or the
    // mini-bar title would fall back to ASCII-only Montserrat (no accents).
    lv_obj_set_style_text_font(s_mini, &bugne_font_14, 0);
    lv_obj_set_style_pad_all(s_mini, 0, 0);  // children place themselves
    lv_obj_set_style_radius(s_mini, RADIUS_BAR, 0);
    lv_obj_set_style_bg_color(s_mini, col_accent(), 0);
    lv_obj_set_style_border_width(s_mini, 0, 0);
    lv_obj_remove_flag(s_mini, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_mini, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_mini, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_mini, on_mini_open, LV_EVENT_CLICKED, NULL);

    s_mini_title = lv_label_create(s_mini);
    lv_label_set_long_mode(s_mini_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_mini_title, lv_color_white(), 0);
    // Bar width minus left pad (14), pause button (40 + 8 right pad) and a gap.
    lv_obj_set_width(s_mini_title, scr_w() - 2 * MINI_BAR_GAP - 14 - 48 - 8);
    lv_obj_align(s_mini_title, LV_ALIGN_LEFT_MID, 14, 0);

    // Round translucent pause button: white at 25% opa over the accent works
    // on all five accents; a press deepens it for feedback.
    lv_obj_t *pb = lv_button_create(s_mini);
    lv_obj_set_size(pb, 40, 40);
    lv_obj_align(pb, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_radius(pb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pb, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(pb, 64, 0);  // 25%
    lv_obj_set_style_bg_color(pb, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(pb, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(pb, lv_color_white(), 0);
    s_mini_pause = lv_label_create(pb);
    lv_obj_center(s_mini_pause);
    lv_obj_add_event_cb(pb, on_mini_pause, LV_EVENT_CLICKED, NULL);
}

// Show/hide and refresh the mini bar. Hidden on the full now-playing screens.
static void mini_bar_update(void)
{
    if (!s_mini) return;
    np_source_t np = np_current();
    bool on_np = (s_active_builder == build_now_playing ||
                  s_active_builder == build_sendspin_playing ||
                  s_active_builder == build_alarm_ringing);
    if (np == NP_NONE || on_np) {
        lv_obj_add_flag(s_mini, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char title[96];
    np_fill_title(np, title, sizeof(title));
    if (strcmp(lv_label_get_text(s_mini_title), title) != 0) {
        lv_label_set_text(s_mini_title, title);
    }
    const char *icon = np_is_paused(np) ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE;
    if (strcmp(lv_label_get_text(s_mini_pause), icon) != 0) {
        lv_label_set_text(s_mini_pause, icon);
    }
    lv_obj_remove_flag(s_mini, LV_OBJ_FLAG_HIDDEN);
}

// ---- Screen sleep ----

static void enter_sleep(void)
{
    bl_set(0);
    if (s_panel) esp_lcd_panel_disp_on_off(s_panel, false);
    s_asleep = true;
    ESP_LOGI(TAG, "screen sleep");
}

static void exit_sleep(void)
{
    if (s_panel) esp_lcd_panel_disp_on_off(s_panel, true);
    bl_set(100);
    s_asleep = false;
    // Swallow the touch that woke the screen so it does not also click whatever
    // button is under the finger (buttons fire on release; this cancels it).
    if (s_touch_indev) lv_indev_wait_release(s_touch_indev);
    ESP_LOGI(TAG, "screen wake");
}

// ---- Alarm engine ----

// Look up a configured web radio by its stable id (survives web reorder or
// delete). Returns NULL if the id no longer exists (the alarm then beeps).
static const config_webradio_t *find_radio_by_id(const config_t *c, int id)
{
    for (size_t i = 0; i < c->webradio_count; i++)
        if (c->webradios[i].id == id) return &c->webradios[i];
    return NULL;
}

// Start the alarm beep fallback (called on the LVGL task from the alarm engine).
// s_alarm_beeping stops the late-failure check re-firing and switches the ringing
// screen source line to STR_ALARM_BEEP. We first take over any active source the
// same way ui_play does: without this the worker would still be blocked inside a
// source _play() call and REQ_BEEP would wait behind it (the alarm must always
// sound). When nothing is playing these stops are harmless no-ops.
static void beep_start(void)
{
    ESP_LOGI(TAG, "alarm: starting beep fallback");
    tuner_stop_sync();  // free the arbiter: the alarm must always sound
    s_memo_stop = true; // a memo record/playback yields too: the alarm always sounds
    s_alarm_beeping = true;
    s_beep_stop = false;
    s_stop_requested = true;
    audio_set_paused(false);
    // Same metadata reset as ui_play(), so GET /api/playback (and any other reader
    // of s_now_title/s_meta_*) reports the beep cleanly instead of the previous
    // source's stale title/artist.
    strlcpy(s_now_title, T(STR_ALARM_BEEP), sizeof(s_now_title));
    s_now_target[0] = '\0';  // the beep has no favorite identity (star button)
    s_now_is_file = false;   // and no progress row / seek
    s_meta_title[0] = '\0';
    s_meta_artist[0] = '\0';
    source_sd_stop();
    source_stream_stop();
    if (source_sendspin_session_active()) {
        source_sendspin_command(SENDSPIN_CMD_STOP);
    }
    play_req_t req = { .kind = REQ_BEEP };
    xQueueOverwrite(s_play_q, &req);
}

// Fire alarms[idx]: wake the screen, start the configured source (or the beep
// if it cannot play), set the ramp-start volume and show the ringing screen.
static void alarm_fire(int idx)
{
    const config_t *c = config_store_get();
    if (!c || idx < 0 || idx >= CFG_MAX_ALARMS) return;
    const config_alarm_t *ca = &c->alarms[idx];
    s_alarm_active_idx = idx;              // RINGING/SNOOZED code reads this alarm
    s_alarm_fired_min[idx] = time(NULL) / 60;   // latch this minute so it fires once
    s_alarm_saved_vol = audio_get_volume(); // restored at alarm end
    s_alarm_beeping = false;
    s_alarm_beep_confirmed = false;
    sleep_clear();  // the alarm always wins over a sleep timer (A2)

    if (s_asleep) exit_sleep();
    lv_display_trigger_activity(s_disp);

    // Resolve the source with a pre-flight check. If it cannot possibly play,
    // fall straight to the beep so the alarm always sounds.
    bool started = false;
    if (ca->source == 0) {
        const config_webradio_t *r = find_radio_by_id(c, ca->radio_id);
        if (r && net_state() == NET_STATE_CONNECTED) {
            s_play_ctx = PLAY_CTX_NONE;  // a single stream, no next/previous
            ui_play(false, r->url, r->name, 0);
            started = true;
        }
    } else {
        if (ca->sd_path[0] && source_sd_present()) {
            char path[16 + CFG_URL_MAX];
            snprintf(path, sizeof(path), "/sdcard/%s", ca->sd_path);
            struct stat st;
            if (stat(path, &st) == 0) {
                s_play_ctx = PLAY_CTX_NONE;  // a single track, no next/previous
                s_np_dur_override_ms = 0;
                ui_play(true, path,
                        ca->sd_title[0] ? ca->sd_title : ca->sd_path, 0);
                started = true;
            }
        }
    }
    if (!started) beep_start();

    // Ramp-start volume: 15% of the target, floor 5. The RINGING tick climbs it
    // to the target over 60 s. audio_set_volume clamps to volume_max.
    int target = ca->volume;
    int start = target * 15 / 100;
    if (start < 5) start = 5;
    audio_set_volume(start);

    s_alarm_ring_start_us = esp_timer_get_time();
    s_alarm_state = ALARM_RINGING;
    show(build_alarm_ringing);
}

// End the ring: stop playback the same way the Stop button does, restore the
// user's volume, then snooze (+10 min) or go idle, and return home.
static void alarm_finish(bool snooze)
{
    ui_stop();  // reuse the one stop path (A5 will also stop the beep here)
    audio_set_volume(s_alarm_saved_vol);
    if (snooze) {
        s_alarm_snooze_until = time(NULL) + 600;
        s_alarm_state = ALARM_SNOOZED;
    } else {
        s_alarm_state = ALARM_IDLE;
    }
    show(build_home);
}

// Latch the alarm occurrence the sunrise ramp was canceled for (index + fire
// epoch minute), so the entry check does not re-enter the same occurrence a
// few seconds later. The next day's occurrence has a different fire minute
// and enters normally.
static void sunrise_latch(const config_t *c, time_t wnow)
{
    struct tm tmv;
    localtime_r(&wnow, &tmv);
    int mu = 0;
    int nf = alarm_next_fire(c->alarms, CFG_MAX_ALARMS, &tmv, &mu);
    if (nf >= 0) {
        s_sunrise_block_idx = nf;
        s_sunrise_block_min = wnow / 60 + mu;
    }
}

// ---- Remote screenshot / navigation (for the web server) ----

// Take a screenshot of the current screen. Blocks until the LVGL task renders it
// (or timeout_ms elapses). On ESP_OK, *px points at a kept RGB565 buffer valid
// until ui_screenshot_release(). Serialized: one client at a time. The caller
// MUST call ui_screenshot_release() afterwards, on success or failure.
esp_err_t ui_screenshot(const uint8_t **px, int *w, int *h, uint32_t timeout_ms)
{
    if (!s_shot_lock || !s_shot_done) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_shot_lock, portMAX_DELAY);
    // Drain a stale completion left by a previously timed-out request, so this
    // client waits for its own snapshot and not the previous one.
    xSemaphoreTake(s_shot_done, 0);
    s_shot_ok = false;
    s_shot_req = true;
    if (xSemaphoreTake(s_shot_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        s_shot_req = false;  // the timer may still service it; drained next call
        return ESP_ERR_TIMEOUT;
    }
    if (!s_shot_ok || !s_shot_buf) return ESP_ERR_NO_MEM;
    *px = s_shot_buf;
    *w  = s_shot_w;
    *h  = s_shot_h;
    return ESP_OK;
}

void ui_screenshot_release(void)
{
    if (s_shot_lock) xSemaphoreGive(s_shot_lock);
}

// Screen name -> builder, for POST /api/debug/nav. All builders are defined
// above. Screens that need selection state their on_open handler normally sets
// (episodes, library_albums, game, game_play) are prepared by the applier in
// sleep_timer_cb. "game_play" jumps straight past the table picker into
// build_game (all tables enabled), for a remote screenshot of the game itself.
typedef struct { const char *name; screen_builder_t fn; } nav_screen_t;
static const nav_screen_t NAV_SCREENS[] = {
    { "home",              build_home },
    { "webradios",         build_webradios },
    { "podcasts",          build_podcasts },
    { "episodes",          build_episodes },
    { "sd",                build_sd },
    { "library",           build_library },
    { "library_artists",   build_library_artists },
    { "library_albums",    build_library_albums },
    { "game",              build_game_setup },
    { "game_play",         build_game },
    { "tuner",             build_tuner },
    { "memos",             build_memos },
    { "memo_record",       build_memo_record },
    { "settings",          build_settings },
    { "settings_theme",    build_settings_theme },
    { "settings_alarm",    build_settings_alarm },
    { "settings_web",      build_settings_web },
    { "settings_ap",       build_settings_ap },
    { "settings_library",  build_settings_library },
    { "settings_podcasts", build_settings_podcasts },
    { "setup",             build_setup },
    { "favorites",         build_favorites },
    { "now_playing",       build_now_playing },
    { "sendspin",          build_sendspin_playing },
    { "alarm_ringing",     build_alarm_ringing },
};
#define NAV_SCREEN_COUNT ((int)(sizeof(NAV_SCREENS) / sizeof(NAV_SCREENS[0])))

// Validate the screen name synchronously (safe string compare from the web task)
// and queue it for the UI task. Returns false on an unknown name so the web
// handler can answer 400. Deviates from a void signature (see ui.h) so the
// endpoint can reject unknown screens without a round-trip to the UI task.
bool ui_remote_nav(const char *screen)
{
    if (!screen) return false;
    for (int i = 0; i < NAV_SCREEN_COUNT; i++) {
        if (strcmp(screen, NAV_SCREENS[i].name) == 0) {
            strlcpy(s_nav_req, screen, sizeof(s_nav_req));  // applied by sleep_timer_cb
            return true;
        }
    }
    return false;
}

static void sleep_timer_cb(lv_timer_t *t)
{
    (void)t;

    // Serve a pending screenshot request (from the web server). Rendering must
    // stay on this task. Works while the screen sleeps: the snapshot renders the
    // widget tree off-screen, so do not wake the display or touch activity here.
    if (s_shot_req) {
        s_shot_ok = false;
        lv_draw_buf_t *db = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
        if (db) {
            int w = db->header.w, h = db->header.h;
            uint32_t stride = db->header.stride;
            if (!s_shot_buf) {
                s_shot_buf = heap_caps_malloc(LCD_HRES * LCD_VRES * 2, MALLOC_CAP_SPIRAM);
            }
            if (s_shot_buf && w > 0 && h > 0 && (w * h * 2) <= (LCD_HRES * LCD_VRES * 2)) {
                for (int y = 0; y < h; y++) {
                    memcpy(s_shot_buf + (size_t)y * w * 2,
                           db->data + (size_t)y * stride, (size_t)w * 2);
                }
                s_shot_w = w;
                s_shot_h = h;
                s_shot_ok = true;
            }
            lv_draw_buf_destroy(db);
        }
        // Composite the top layer (mini bar / toast) over the base image when it
        // has any visible child, so the screenshot matches the glass.
        if (s_shot_ok) {
            lv_obj_t *top = lv_layer_top();
            bool visible = false;
            uint32_t nc = lv_obj_get_child_count(top);
            for (uint32_t i = 0; i < nc; i++) {
                if (!lv_obj_has_flag(lv_obj_get_child(top, i), LV_OBJ_FLAG_HIDDEN)) {
                    visible = true;
                    break;
                }
            }
            if (visible) {
                lv_draw_buf_t *tb = lv_snapshot_take(top, LV_COLOR_FORMAT_ARGB8888);
                if (tb) {
                    int tw = tb->header.w < s_shot_w ? tb->header.w : s_shot_w;
                    int th = tb->header.h < s_shot_h ? tb->header.h : s_shot_h;
                    for (int y = 0; y < th; y++) {
                        const uint32_t *src = (const uint32_t *)(tb->data + (size_t)y * tb->header.stride);
                        uint16_t *dst = (uint16_t *)(s_shot_buf + (size_t)y * s_shot_w * 2);
                        for (int x = 0; x < tw; x++) {
                            uint32_t p = src[x];
                            uint8_t a = p >> 24;
                            if (a == 0) continue;  // fully transparent: keep base pixel
                            uint8_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
                            if (a != 255) {
                                uint16_t d = dst[x];
                                uint8_t dr = ((d >> 11) & 0x1F) << 3;
                                uint8_t dg = ((d >> 5)  & 0x3F) << 2;
                                uint8_t db8 = (d & 0x1F) << 3;
                                r = (r * a + dr  * (255 - a)) / 255;
                                g = (g * a + dg  * (255 - a)) / 255;
                                b = (b * a + db8 * (255 - a)) / 255;
                            }
                            dst[x] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                        }
                    }
                    lv_draw_buf_destroy(tb);
                }
            }
        }
        s_shot_req = false;
        xSemaphoreGive(s_shot_done);
    }

    // Apply a pending listening-stats reset (POST /api/stats/reset). Runs on the
    // LVGL task, sole owner of the stats RAM and file.
    if (s_stats_reset_req) {
        s_stats_reset_req = false;
        stats_reset();
    }

    // Apply a pending remote navigation request (POST /api/debug/nav).
    if (s_nav_req[0]) {
        char name[sizeof(s_nav_req)];
        strlcpy(name, s_nav_req, sizeof(name));
        s_nav_req[0] = '\0';
        // Prepare selection state that these screens' on_open handlers normally set.
        if (strcmp(name, "episodes") == 0) {
            const config_t *c = config_store_get();
            if (s_nav_podcast_rss[0] == '\0' && c && c->podcast_count > 0) {
                s_nav_podcast_id = c->podcasts[0].id;  // first configured podcast
                strlcpy(s_nav_podcast_rss, c->podcasts[0].rss_url, sizeof(s_nav_podcast_rss));
            }
        } else if (strcmp(name, "library_albums") == 0) {
            if (s_lib_artist[0] == '\0') strlcpy(name, "library_artists", sizeof(name));  // no artist chosen
        } else if (strcmp(name, "game") == 0 || strcmp(name, "game_play") == 0) {
            if (parental_blocked()) {
                strlcpy(name, "home", sizeof(name));   // quiet hours / daily limit: refuse, go home
            } else {
                if (s_game_timer) { lv_timer_delete(s_game_timer); s_game_timer = NULL; }
                s_game_score = 0;                      // mimic on_open_game: fresh session
                s_game_best = config_store_get_highscore();
                // "game" mimics on_open_game (picker starts with nothing checked); "game_play"
                // bypasses the picker straight into build_game, so it needs a non-empty set.
                s_game_tables = (strcmp(name, "game_play") == 0) ? GAME_TABLES_ALL : 0;
                s_game_a = 0;
            }
        } else if (strcmp(name, "tuner") == 0) {
            tuner_begin();  // mimic on_open_tuner: takeover + capture task
        } else if (strcmp(name, "memos") == 0 || strcmp(name, "memo_record") == 0) {
            if (parental_blocked()) {
                strlcpy(name, "home", sizeof(name));  // same refusal as the tile
            } else if (strcmp(name, "memo_record") == 0) {
                s_memo_state = MEMO_UI_IDLE;  // mimic on_memo_new: fresh record screen
                s_memo_rec_ms = 0;
                s_memo_state_shown = MEMO_UI_IDLE;
            }
        }
        for (int i = 0; i < NAV_SCREEN_COUNT; i++) {
            if (strcmp(name, NAV_SCREENS[i].name) == 0) { show(NAV_SCREENS[i].fn); break; }
        }
    }
    // Background download scheduler. Run the persisted job only when audio has been
    // idle for >5 min and Wi-Fi is up; pause it the instant audio plays. This also
    // drives resume-after-reboot: the job is loaded at boot with the idle clock set
    // far in the past, so it fires as soon as Wi-Fi is back.
    {
        int64_t now = esp_timer_get_time();
        if (audio_is_active()) { s_audio_idle_since_us = now; s_played_since_maint = true; }
        bool idle_5min = (now - s_audio_idle_since_us) >= DL_IDLE_RESUME_US;
        bool net_ok = !s_refreshing && source_sd_present() && net_state() == NET_STATE_CONNECTED;
        if (s_downloading) {
            if (audio_is_active()) s_dl_cancel = true;  // pause: worker saves cursor and exits
        } else if (s_dljob.active && net_ok && idle_5min) {
            s_downloading = true;
            s_dl_cancel = false;
            play_req_t req = { .kind = REQ_DOWNLOAD_JOB };
            xQueueOverwrite(s_play_q, &req);
        } else if ((s_played_since_maint || !s_maint_done_since_boot) && net_ok && idle_5min) {
            // Auto-maintenance (#61): refresh all feeds and download new episodes when
            // idle 5 min -- once after each listening session, and also once after a
            // boot even with no listening (so a device left on still updates). Only
            // with enough SD space free, so it never thrashes against a full card.
            uint64_t freeb = 0;
            if (source_sd_usage(NULL, &freeb) && freeb >= DL_MIN_FREE_BYTES) {
                memset(&s_dljob, 0, sizeof(s_dljob));
                s_dljob.active = true;
                s_dljob.scope_all = true;  // force stays false: refresh + download-new
                s_dljob.maint = true;      // also rescan the music library at the end
                dljob_persist();
                s_dl_done = 0;
                s_dl_total = 0;
                s_dl_phase = UI_DL_SCHEDULED;
                s_played_since_maint = false;
                s_maint_done_since_boot = true;
                ESP_LOGI(TAG, "auto-maintenance: refresh + download-new (all) scheduled");
            }
        }
    }

    // Apply a UI language change live (e.g. set from the web config). Fires once per
    // change: after lang_set_code the codes match again.
    if (strcmp(config_store_get()->ui.lang, lang_code()) != 0) {
        lang_set_code(config_store_get()->ui.lang);
        if (s_active_builder) show(s_active_builder);
    }

    // Apply a timezone change live (set from the web config), same
    // edge-triggered style. No rebuild needed: the 1 Hz clock tick below picks
    // up the new local time on its next run.
    if (strcmp(config_store_get()->ui.tz, s_tz_applied) != 0) {
        apply_tz(config_store_get()->ui.tz);
        strlcpy(s_tz_applied, config_store_get()->ui.tz, sizeof(s_tz_applied));
    }

    // Apply an orientation change live (set from the web config), same
    // edge-triggered style. The mini bar lives on the top layer and is not
    // rebuilt by show(), so recreate it (its geometry lives only in
    // create_mini_bar). No lock needed: this timer runs inside the LVGL task.
    // Fallback if hot rotation misbehaves on hardware: replace this block's
    // body with esp_restart(); (the boot path applies the stored orientation).
    bool want_ls = (config_store_get()->ui.orientation == 1);
    if (want_ls != s_landscape) {
        apply_orientation(want_ls);
        lv_obj_delete(s_mini);
        create_mini_bar();
        if (s_active_builder) show(s_active_builder);
    }

    // Apply a theme change live (picker tap or web config), edge-triggered.
    // apply_theme restyles every existing widget in place, so no rebuild is
    // needed, except the mini bar (accent bg is a local style: recreate it,
    // same as the orientation path) and the picker (rebuilt only to move its
    // checkmarks).
    if (config_store_get()->ui.dark != s_dark_applied ||
        config_store_get()->ui.accent != s_accent_applied) {
        apply_theme(config_store_get()->ui.dark == 1, config_store_get()->ui.accent);
        lv_obj_delete(s_mini);
        create_mini_bar();
        if (s_active_builder == build_settings_theme) show(build_settings_theme);
    }

    // Rebuild home when the game gets enabled/disabled from the web page.
    static int s_game_flag_applied = -1;
    if (config_store_get()->ui.game != s_game_flag_applied) {
        bool first = (s_game_flag_applied == -1);
        s_game_flag_applied = config_store_get()->ui.game;
        if (!first && s_active_builder == build_home) show(build_home);
    }

    // Same for the tuner tile.
    static int s_tuner_flag_applied = -1;
    if (config_store_get()->ui.tuner != s_tuner_flag_applied) {
        bool first = (s_tuner_flag_applied == -1);
        s_tuner_flag_applied = config_store_get()->ui.tuner;
        if (!first && s_active_builder == build_home) show(build_home);
    }

    // The tuner runs only while its screen is shown: navigating away by any
    // path (remote nav, alarm ringing screen, ...) ends the capture. The
    // async flag is enough here; playback takeovers use tuner_stop_sync().
    if (s_tuner_active && s_active_builder != build_tuner) {
        s_tuner_run = false;
    }

    // Refresh the tuner readout from the capture task's published state.
    if (s_active_builder == build_tuner && s_tuner_note_lbl) {
        char txt[40];
        int64_t now = esp_timer_get_time();
        bool fresh = s_tuner_hit_us && (now - s_tuner_hit_us) < 800000;
        if (fresh) {
            int midi = s_tuner_midi;
            float cents = s_tuner_cents;
            int pc = midi % 12, oct = midi / 12 - 1;
            if (strcmp(lang_code(), "fr") == 0) {
                snprintf(txt, sizeof(txt), "%s (%s%d)",
                         TUNER_NOTE_FR[pc], TUNER_NOTE_EN[pc], oct);
            } else {
                snprintf(txt, sizeof(txt), "%s%d", TUNER_NOTE_EN[pc], oct);
            }
            if (fabsf(cents) <= TUNER_IN_TUNE_CENTS) {
                lv_obj_set_style_text_color(s_tuner_note_lbl, col_accent(), 0);
            } else {
                lv_obj_remove_local_style_prop(s_tuner_note_lbl, LV_STYLE_TEXT_COLOR, 0);
            }
            if (strcmp(lv_label_get_text(s_tuner_note_lbl), txt) != 0) {
                lv_label_set_text(s_tuner_note_lbl, txt);
            }
            char fz[20];
            snprintf(fz, sizeof(fz), "%.1f Hz", (double)s_tuner_freq);
            if (s_tuner_freq_lbl && strcmp(lv_label_get_text(s_tuner_freq_lbl), fz) != 0) {
                lv_label_set_text(s_tuner_freq_lbl, fz);
            }
            int c = (int)lroundf(cents);
            if (c < -50) c = -50;
            if (c > 50) c = 50;
            if (s_tuner_bar && lv_bar_get_value(s_tuner_bar) != c) {
                lv_bar_set_value(s_tuner_bar, c, LV_ANIM_OFF);
            }
        } else {
            lv_obj_remove_local_style_prop(s_tuner_note_lbl, LV_STYLE_TEXT_COLOR, 0);
            if (strcmp(lv_label_get_text(s_tuner_note_lbl), T(STR_TUNER_PLAY_NOTE)) != 0) {
                lv_label_set_text(s_tuner_note_lbl, T(STR_TUNER_PLAY_NOTE));
            }
            if (s_tuner_freq_lbl && lv_label_get_text(s_tuner_freq_lbl)[0]) {
                lv_label_set_text(s_tuner_freq_lbl, "");
            }
            if (s_tuner_bar && lv_bar_get_value(s_tuner_bar) != 0) {
                lv_bar_set_value(s_tuner_bar, 0, LV_ANIM_OFF);
            }
        }
    }

    // ---- Voice memo UI glue ----
    // Rebuild the record screen on worker-driven state changes (capture
    // finalized to preview, peer browse finished), refresh the live capture
    // readout, stop a capture when the user navigates away, mirror the
    // play/stop icon, surface one-shot results, and consume the receive flag
    // set by the httpd task (toast + home badge / open list refresh).
    {
        if (s_active_builder == build_memo_record) {
            if (s_memo_state != s_memo_state_shown ||
                (s_memo_state == MEMO_UI_PICK_PEER &&
                 s_memo_peer_count != s_memo_peers_shown)) {
                memo_show_record();
            } else if (s_memo_state == MEMO_UI_RECORDING && s_memo_time_lbl) {
                char tt[20];
                int sdur = s_memo_rec_ms / 1000;
                snprintf(tt, sizeof(tt), "%d:%02d / %d:00", sdur / 60, sdur % 60,
                         MEMO_MAX_MS / 60000);
                if (strcmp(lv_label_get_text(s_memo_time_lbl), tt) != 0) {
                    lv_label_set_text(s_memo_time_lbl, tt);
                }
                if (s_memo_prog_bar) {
                    lv_bar_set_value(s_memo_prog_bar, s_memo_rec_ms, LV_ANIM_OFF);
                }
            }
        } else {
            if (s_memo_state == MEMO_UI_RECORDING) s_memo_stop = true;  // capture ends off-screen
            s_memo_state_shown = s_memo_state;  // track silently for a fresh re-entry
            s_memo_peers_shown = s_memo_peer_count;
        }
        if (s_memo_play_btn && s_active_builder == build_memo_play) {
            lv_obj_t *icon = lv_obj_get_child(s_memo_play_btn, 0);
            const char *want = s_memo_playing ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY;
            if (icon && strcmp(lv_label_get_text(icon), want) != 0) {
                lv_label_set_text(icon, want);
            }
        }
        if (s_memo_result != 0) {
            int r = s_memo_result;
            s_memo_result = 0;
            toast(r == 1 ? T(STR_MEMO_SENT)
                : r == -1 ? T(STR_MEMO_SEND_FAILED) : T(STR_MEMO_REC_FAILED));
            if (r == 1 && s_active_builder == build_memo_record) show(build_memos);
        }
        if (s_memo_rx_flag) {
            s_memo_rx_flag = false;
            char nice[MEMO_SENDER_MAX], msg[64];
            strlcpy(nice, s_memo_rx_from, sizeof(nice));
            for (char *p = nice; *p; p++) {
                if (*p == '-') *p = ' ';
            }
            snprintf(msg, sizeof(msg), T(STR_MEMO_NEW_FROM_FMT), nice);
            toast(msg);
            if (s_active_builder == build_home || s_active_builder == build_memos) {
                show(s_active_builder);  // badge dot / new list row appears
            }
        }
    }

    // Rebuild home when the SD card comes or goes: the memos tile (grey state
    // and unread badge) keys off it. The SD browser handles its own message.
    static int s_sd_present_applied = -1;
    if ((int)source_sd_present() != s_sd_present_applied) {
        bool first = (s_sd_present_applied == -1);
        s_sd_present_applied = (int)source_sd_present();
        if (!first && s_active_builder == build_home) show(build_home);
    }

    // Rebuild home (the Favorites tile appears/disappears) and an open
    // favorites screen when the list changes from the web page, same
    // edge-triggered pattern. Device star taps pass through here harmlessly
    // (now-playing is neither screen; home rebuilds on its own next show).
    static int s_fav_count_applied = -1;
    if ((int)config_store_get()->favorite_count != s_fav_count_applied) {
        bool first = (s_fav_count_applied == -1);
        s_fav_count_applied = (int)config_store_get()->favorite_count;
        if (!first) {
            if (s_active_builder == build_home) show(build_home);
            else if (s_active_builder == build_favorites) show(build_favorites);
        }
    }

    // Apply a volume-ceiling change live (set from the web config). The limit
    // clamps inside audio_set_volume; rebuild an open now-playing screen so
    // its volume slider range matches the new ceiling.
    if (config_store_get()->ui.volume_max != audio_get_volume_limit()) {
        audio_set_volume_limit(config_store_get()->ui.volume_max);
        if (s_active_builder == build_now_playing ||
            s_active_builder == build_sendspin_playing) {
            show(s_active_builder);
        }
    }

    // Follow the Sendspin stream, edge-triggered: open the now-playing screen when
    // Music Assistant STARTS a stream, but let the user navigate away afterwards
    // (the mini bar keeps it reachable). Return home when the stream ends only if
    // still on that screen. Refresh the title/progress while it is shown.
    bool ss = source_sendspin_session_active();
    if (ss && !s_ss_prev) {
        if (play_denied()) {  // parental block: refuse the Music Assistant stream
            source_sendspin_command(SENDSPIN_CMD_STOP);
        } else if (s_tuner_active) {
            // Someone is tuning an instrument in front of the device: the
            // local user wins over a remote Music Assistant push (which
            // could not play anyway, the tuner holds the arbiter).
            source_sendspin_command(SENDSPIN_CMD_STOP);
        } else {
            show(build_sendspin_playing);
        }
    } else if (!ss && s_ss_prev) {
        if (s_active_builder == build_sendspin_playing) show(build_home);
    } else if (ss && s_active_builder == build_sendspin_playing) {
        ss_refresh();
    }
    s_ss_prev = ss;

    // Playback died on its own (unreachable stream, lost connection, decode
    // error): tell the user on the now-playing screen instead of leaving a
    // normal-looking screen with permanent silence. Reuses the ICY line (radio)
    // or the time line (podcast/SD); cleared by the next play (ui_play). A
    // local-file failure gets its own wording: "check the connection" would
    // point the user at Wi-Fi for a corrupt SD file.
    if (s_play_failed && s_active_builder == build_now_playing) {
        lv_obj_t *lbl = s_np_icy_lbl ? s_np_icy_lbl : s_np_prog_time;
        const char *txt = T(s_play_failed_local ? STR_PLAYBACK_FAILED : STR_STREAM_FAILED);
        if (lbl && strcmp(lv_label_get_text(lbl), txt) != 0) {
            lv_obj_set_style_text_color(lbl, lv_palette_main(LV_PALETTE_RED), 0);
            lv_label_set_text(lbl, txt);
        }
    }

    // Stream reconnect in progress (play_task retry loop): explain the silence
    // on the same line, normal color. The red failure text only shows if the
    // retries give up.
    if (s_play_retrying && !s_play_failed && s_active_builder == build_now_playing) {
        lv_obj_t *lbl = s_np_icy_lbl ? s_np_icy_lbl : s_np_prog_time;
        const char *txt = T(STR_RECONNECTING);
        if (lbl && strcmp(lv_label_get_text(lbl), txt) != 0) {
            lv_label_set_text(lbl, txt);
        }
    }

    // Live-update the web radio ICY "now playing" line on the local screen.
    if (!s_play_failed && !s_play_retrying && s_active_builder == build_now_playing && s_np_icy_lbl) {
        char icy[128];
        source_stream_title(icy, sizeof(icy));
        if (strcmp(lv_label_get_text(s_np_icy_lbl), icy) != 0) {
            lv_label_set_text(s_np_icy_lbl, icy);
        }
    }

    // Live-update the SD file progress slider/time (unless the user is dragging).
    if (!s_play_failed && s_active_builder == build_now_playing && s_np_prog && !s_np_seeking) {
        uint32_t pos = 0, dur = 0;
        decode_progress(&pos, &dur);
        if (s_np_dur_override_ms > 0) {
            dur = s_np_dur_override_ms;  // exact RSS duration for podcasts
        }
        if (dur > 0) {
            lv_slider_set_value(s_np_prog, (int)((uint64_t)pos * 1000 / dur), LV_ANIM_OFF);
        }
        if (s_np_prog_time) {
            char tm[32], a[12], b[12];
            ss_fmt_time(a, sizeof(a), pos);
            ss_fmt_time(b, sizeof(b), dur);
            snprintf(tm, sizeof(tm), "%s / %s", a, b);
            if (strcmp(lv_label_get_text(s_np_prog_time), tm) != 0) {
                lv_label_set_text(s_np_prog_time, tm);
            }
        }
    }

    // Persist the podcast playback position periodically, regardless of which
    // screen is shown, so it survives a power loss (there is no shutdown hook
    // to save on instead). Throttled: only every RESUME_WRITE_INTERVAL_US, and
    // only once the position has actually moved.
    if (s_play_ctx == PLAY_CTX_PODCAST && s_resume.active && audio_is_active()) {
        uint32_t pos = 0, dur = 0;
        decode_progress(&pos, &dur);
        uint32_t abs_pos = s_resume.cached_trimmed_mp3
                                ? pos + (uint32_t)s_play_podcast_skip_s * 1000
                                : pos;
        int64_t now = esp_timer_get_time();
        if (abs_pos != s_resume_last_pos_ms && now - s_resume_last_write_us >= RESUME_WRITE_INTERVAL_US) {
            s_resume.pos_ms = abs_pos;
            resume_persist();
            s_resume_last_pos_ms = abs_pos;
            s_resume_last_write_us = now;

            // Auto-mark as played if we reach 3/4 of the total duration.
            uint32_t total_dur = (s_np_dur_override_ms > 0) ? s_np_dur_override_ms : dur;
            if (total_dur > 0 && abs_pos >= (total_dur / 4) * 3) {
                if (s_play_eps && s_play_index >= 0 && (size_t)s_play_index < s_play_ep_count) {
                    played_mark(s_play_eps[s_play_index].episode_url);
                }
            }
        }
    }

    // Track the SD file's title/artist once the decoder has parsed the tags
    // (ID3v2 / Vorbis comments). Kept current whenever SD is playing so the mini
    // bar shows the title too; the now-playing labels update only while shown.
    if (s_play_ctx == PLAY_CTX_SD) {
        char t[64], a[64];
        decode_metadata(t, sizeof(t), a, sizeof(a));
        if (strcmp(t, s_meta_title) != 0) {
            strlcpy(s_meta_title, t, sizeof(s_meta_title));
            if (s_active_builder == build_now_playing && s_np_name) {
                lv_label_set_text(s_np_name, s_meta_title[0] ? s_meta_title : s_now_title);
            }
        }
        if (strcmp(a, s_meta_artist) != 0) {
            strlcpy(s_meta_artist, a, sizeof(s_meta_artist));
            if (s_active_builder == build_now_playing && s_np_artist) {
                lv_label_set_text(s_np_artist, s_meta_artist);
            }
        }
    }

    // Apply a pending web remote command on the UI task (LVGL-safe).
    if (s_remote_cmd >= 0) {
        int cmd = s_remote_cmd;
        s_remote_cmd = -1;
        ui_remote_apply((ui_remote_t)cmd, s_remote_arg);
    }

    // A track finished on its own: play the next item in the same list (SD folder
    // or podcast episodes), or stop at the end of the list. Runs on the UI task so
    // play_ctx_at (LVGL) is safe.
    if (s_advance) {
        s_advance = false;
        // A3: the episode that just ended reached a real natural end (s_advance
        // is only set by play_task on source_sd_completed()/source_stream_completed(),
        // never on a user stop). Mark it played. This runs on the UI (LVGL) task,
        // same as played_contains() in build_episodes/ep_table_draw_cb, so
        // played.c needs no lock (see played.h).
        if (s_play_ctx == PLAY_CTX_PODCAST && s_play_eps &&
            s_play_index >= 0 && (size_t)s_play_index < s_play_ep_count) {
            played_mark(s_play_eps[s_play_index].episode_url);
            ESP_LOGI(TAG, "played: marked");
        }
        if (s_sleep_end_of_track) {
            // Sleep timer armed for "end of track": stop here instead of moving
            // to the next item. ui_stop() clears the timer (sleep_clear).
            ui_stop();
            toast(T(STR_SLEEP_STOPPED));
            ESP_LOGI(TAG, "sleep timer: fired");
            if (s_active_builder == build_now_playing) show(build_home);
        } else {
            int n = (s_play_ctx == PLAY_CTX_SD)      ? (int)s_play_sd_count
                  : (s_play_ctx == PLAY_CTX_PODCAST) ? (int)s_play_ep_count
                  : (s_play_ctx == PLAY_CTX_LIBRARY) ? (int)s_play_lib_count
                  : 0;
            if (n > 0) {
                if (s_play_index + 1 < n) {
                    play_ctx_at_ex(s_play_index + 1, s_active_builder == build_now_playing);
                } else {
                    // End of the list: explicitly stop. The natural decode end alone
                    // does not reliably silence the output when no next track reopens
                    // it (the symptom: sound lingers until Stop). ui_stop() runs the
                    // same path as the Stop button, which clears it. The toast tells
                    // the user why the sound ended, wherever they navigated to.
                    ui_stop();
                    toast(T(STR_END_OF_LIST));
                    if (s_active_builder == build_now_playing) show(build_home);
                }
            }
        }
    }

    // Live-update the library sync status while that screen is open.
    if (s_active_builder == build_settings_library && s_set_lib_lbl) {
        char t[64];
        if (library_scanning()) strlcpy(t, T(STR_SCANNING_SD), sizeof(t));
        else snprintf(t, sizeof(t), T(STR_TRACKS_INDEXED_FMT), (unsigned)library_track_count());
        if (strcmp(lv_label_get_text(s_set_lib_lbl), t) != 0) {
            lv_label_set_text(s_set_lib_lbl, t);
        }
    }

    // Refresh-all status: show progress while running, the feed count when done.
    // Leave any other message (e.g. T(STR_STOP_PLAYBACK_FIRST)) untouched.
    if (s_active_builder == build_settings_podcasts && s_set_pod_lbl) {
        if (s_refreshing) {
            if (strcmp(lv_label_get_text(s_set_pod_lbl), T(STR_REFRESHING_ALL)) != 0) {
                lv_label_set_text(s_set_pod_lbl, T(STR_REFRESHING_ALL));
            }
            s_pod_was_refreshing = true;
        } else if (s_pod_was_refreshing) {
            s_pod_was_refreshing = false;
            if (s_refresh_ok) {
                const config_t *c = config_store_get();
                char t[64];
                snprintf(t, sizeof(t), T(STR_REFRESH_DONE_FMT), (unsigned)(c ? c->podcast_count : 0));
                lv_label_set_text(s_set_pod_lbl, t);
            } else {
                lv_label_set_text(s_set_pod_lbl, T(STR_REFRESH_FAILED));
            }
        }
    }

    mini_bar_update();  // persistent now-playing bar across screens

    // 1 Hz wall-clock tick: home clock refresh, then the alarm engine.
    // Throttled so the labels and the alarm check only touch state once a
    // second, not every 50 ms tick.
    {
        static int64_t s_clock_last_us;
        int64_t now = esp_timer_get_time();
        if (now - s_clock_last_us >= 1000000) {
            s_clock_last_us = now;

            // ---- Listening stats (C3) ----
            // Count one second of real listening (not paused, not the alarm
            // beep) into today's bucket when the clock is valid. Persist on the
            // periodic/rollover signal from stats_tick and on the play->idle
            // edge, so a stopped session lands on flash promptly.
            {
                static bool s_stats_session_prev;
                stats_source_t ssrc = STATS_SRC_RADIO;
                const char *stitle = "";
                bool listening = stats_classify(&ssrc, &stitle);
                if (listening && time_valid()) {
                    time_t st = time(NULL);
                    struct tm stm;
                    localtime_r(&st, &stm);
                    int date = (stm.tm_year + 1900) * 10000 + (stm.tm_mon + 1) * 100 + stm.tm_mday;
                    if (stats_tick(date, ssrc, stitle)) stats_flush();
                }
                audio_source_t sa = audio_arbiter_active();
                bool session = (sa != AUDIO_SOURCE_NONE && sa != AUDIO_SOURCE_BEEP &&
                                sa != AUDIO_SOURCE_TUNER && sa != AUDIO_SOURCE_MEMO) ||
                               source_sendspin_session_active();
                if (s_stats_session_prev && !session) stats_flush();  // play -> idle edge
                s_stats_session_prev = session;

                // ---- Parental daily usage limit accumulation ----
                // One second of usage = audible listening (same classification
                // as the stats: beep and tuner never count) or time on the
                // game screen with the display awake. The alarm is exempt: its
                // ring must never burn the child's quota. Persisted to NVS on
                // usage_tick's ~1/min signal and on the play -> idle edge, so
                // a power cycle loses at most the last unsaved minute.
                static bool s_usage_counting_prev;
                bool counting = (listening ||
                                 (s_active_builder == build_game && !s_asleep)) &&
                                s_alarm_state != ALARM_RINGING && !s_alarm_beeping;
                int today = date_today();
                if (counting && today > 0) {
                    if (usage_tick(today))
                        config_store_set_usage(usage_date(), usage_seconds());
                    // One-shot warning per day when 5 minutes of quota remain.
                    const config_t *lc = config_store_get();
                    if (lc && lc->daily_limit.enabled && s_limit_warn_date != today) {
                        int rem = lc->daily_limit.minutes * 60 - usage_today(today);
                        if (rem > 0 && rem <= 300) {
                            s_limit_warn_date = today;
                            toast(T(STR_LIMIT_5MIN));
                        }
                    }
                }
                if (s_usage_counting_prev && !counting && usage_date() > 0)
                    config_store_set_usage(usage_date(), usage_seconds());
                s_usage_counting_prev = counting;
            }

            if (s_home_clock && s_active_builder == build_home) {
                bool visible = (np_current() == NP_NONE) && time_valid() && !s_asleep;
                if (visible) {
                    time_t nowt = time(NULL);
                    struct tm tmv;
                    localtime_r(&nowt, &tmv);
                    char buf[8];
                    snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
                    if (strcmp(lv_label_get_text(s_home_clock), buf) != 0) {
                        lv_label_set_text(s_home_clock, buf);
                    }
                    lv_obj_remove_flag(s_home_clock, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(s_home_clock, LV_OBJ_FLAG_HIDDEN);
                }
            }
            if (s_alarm_time_lbl) {
                time_t nowt = time(NULL);
                struct tm tmv;
                localtime_r(&nowt, &tmv);
                char buf[8];
                snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
                if (strcmp(lv_label_get_text(s_alarm_time_lbl), buf) != 0) {
                    lv_label_set_text(s_alarm_time_lbl, buf);
                }
            }

            // ---- Alarm engine (all on the LVGL task, no locking) ----
            const config_t *acfg = config_store_get();
            time_t wnow = time(NULL);
            long step = (long)(wnow - s_last_wall);
            s_last_wall = wnow;
            bool step_ok = (step >= 0 && step <= 5);  // skip SNTP correction ticks
            if (acfg) {
                struct tm tmv;
                localtime_r(&wnow, &tmv);

                // ---- Sunrise light (C1), BEFORE the alarm block on purpose:
                // on the fire tick the alarm takes over (alarm_fire runs
                // exit_sleep, backlight 100, ringing screen) and this block
                // sees the state change on its next tick (handoff). Quiet
                // hours never block it: this path drives the panel and
                // backlight directly, it passes through no user entry-point
                // gate (same exemption-by-construction as alarm_fire). ----
                if (!s_sunrise_active) {
                    if (s_alarm_state == ALARM_IDLE && time_valid() && step_ok &&
                        s_asleep && !audio_is_active() && !source_sendspin_session_active()) {
                        int mu = 0;
                        int nf = alarm_next_fire(acfg->alarms, CFG_MAX_ALARMS, &tmv, &mu);
                        if (nf >= 0 && acfg->alarms[nf].sunrise > 0 &&
                            mu >= 1 && mu <= acfg->alarms[nf].sunrise &&
                            !(nf == s_sunrise_block_idx &&
                              wnow / 60 + mu == s_sunrise_block_min)) {
                            s_sunrise_active = true;
                            s_sunrise_idx = nf;
                            s_sunrise_pct = 5;
                            s_sunrise_log_tick = 0;
                            // Panel on, but s_asleep stays true (see the
                            // statics comment: neutralizes the sleep
                            // countdown, touch rides the normal wake path).
                            if (s_panel) esp_lcd_panel_disp_on_off(s_panel, true);
                            show(build_sunrise);
                            bl_set(5);
                            ESP_LOGI(TAG, "sunrise: start (alarm %d, %d min)",
                                     nf + 1, acfg->alarms[nf].sunrise);
                        }
                    }
                } else if (s_alarm_state != ALARM_IDLE) {
                    // The alarm fired last tick: the ringing screen is up at
                    // backlight 100. Just end the ramp, no screen change.
                    s_sunrise_active = false;
                    ESP_LOGI(TAG, "sunrise: handoff to alarm");
                } else if (!s_asleep) {
                    // Touch: the 50 ms wake check already ran exit_sleep
                    // (backlight 100, touch swallowed). Normal wake = home.
                    s_sunrise_active = false;
                    sunrise_latch(acfg, wnow);
                    show(build_home);
                    ESP_LOGI(TAG, "sunrise: canceled (touch)");
                } else if (audio_is_active() || source_sendspin_session_active()) {
                    // Playback started (web remote / Music Assistant): full
                    // brightness, keep a now-playing screen if one took over
                    // (the Sendspin edge above may have shown it), else home.
                    s_sunrise_active = false;
                    sunrise_latch(acfg, wnow);
                    s_asleep = false;
                    bl_set(100);
                    lv_display_trigger_activity(s_disp);  // restart the sleep countdown
                    if (s_active_builder != build_now_playing &&
                        s_active_builder != build_sendspin_playing) show(build_home);
                    ESP_LOGI(TAG, "sunrise: canceled (play)");
                } else if (step_ok) {
                    // Ramp tick: recompute everything from the wall clock (an
                    // SNTP step tick is skipped entirely via step_ok).
                    int mu = 0;
                    int nf = alarm_next_fire(acfg->alarms, CFG_MAX_ALARMS, &tmv, &mu);
                    int sr = (nf >= 0) ? acfg->alarms[nf].sunrise : 0;
                    if (nf != s_sunrise_idx || sr <= 0 || mu > sr) {
                        // Disabled or retimed mid-ramp: back to the sleeping
                        // state (home behind a dark panel, s_asleep stays true).
                        s_sunrise_active = false;
                        show(build_home);
                        bl_set(0);
                        if (s_panel) esp_lcd_panel_disp_on_off(s_panel, false);
                        ESP_LOGI(TAG, "sunrise: canceled (disabled)");
                    } else {
                        int total = sr * 60;
                        int left = (int)((wnow / 60 + mu) * 60 - wnow);  // seconds to fire
                        int pct = 5 + 95 * (total - left) / total;
                        if (pct < 5) pct = 5;
                        if (pct > 100) pct = 100;
                        if (pct != s_sunrise_pct) {
                            s_sunrise_pct = pct;
                            bl_set(pct);
                        }
                        if (++s_sunrise_log_tick >= 30) {  // ramp evidence in /api/logs
                            s_sunrise_log_tick = 0;
                            ESP_LOGI(TAG, "sunrise: ramp %d%%", pct);
                        }
                    }
                }

                switch (s_alarm_state) {
                case ALARM_IDLE:
                    // Fire on the exact minute of an enabled day, once per alarm
                    // (latched). The step guard drops the tick that carries an
                    // SNTP jump. When several alarms match the same minute, the
                    // lowest index wins (first hit in the loop fires and breaks).
                    for (int ai = 0; ai < CFG_MAX_ALARMS; ai++) {
                        const config_alarm_t *aa = &acfg->alarms[ai];
                        if (aa->enabled && time_valid() && step_ok &&
                            tmv.tm_hour == aa->hour && tmv.tm_min == aa->minute &&
                            (aa->days & (1 << ((tmv.tm_wday + 6) % 7))) &&
                            (wnow / 60 != s_alarm_fired_min[ai])) {
                            alarm_fire(ai);
                            break;
                        }
                    }
                    break;
                case ALARM_SNOOZED:
                    if (!acfg->alarms[s_alarm_active_idx].enabled) {
                        s_alarm_state = ALARM_IDLE;  // disabled while snoozed
                    } else if (wnow >= s_alarm_snooze_until) {
                        alarm_fire(s_alarm_active_idx);  // epoch math, no step guard needed
                    }
                    break;
                case ALARM_RINGING: {
                    lv_display_trigger_activity(s_disp);  // never sleep mid-ring
                    long elapsed_s = (long)((esp_timer_get_time() - s_alarm_ring_start_us) / 1000000);
                    // Volume ramp: the ramp owns the volume for the first 60 s,
                    // v = start + (target-start)*elapsed/60, then it settles
                    // exactly on the target. audio_set_volume clamps to volume_max.
                    int target = acfg->alarms[s_alarm_active_idx].volume;
                    int rstart = target * 15 / 100;
                    if (rstart < 5) rstart = 5;
                    if (elapsed_s < 60) {
                        int v = rstart + (int)((target - rstart) * elapsed_s / 60);
                        if (v != audio_get_volume()) audio_set_volume(v);
                    } else if (audio_get_volume() != target) {
                        audio_set_volume(target);
                    }
                    // Late failure: the source never produced audio (unreachable
                    // stream, or a track shorter than the ring). Beep instead so
                    // the alarm keeps sounding. s_play_failed is read-only here;
                    // the now-playing consumer never runs on the ringing screen.
                    // Gate on the beep actually SOUNDING (arbiter == BEEP), not on
                    // s_alarm_beeping: if a REQ_BEEP was clobbered in the queue or
                    // beep_run could not acquire the arbiter, s_alarm_beeping would
                    // wedge the retry and the alarm would stay silent. Re-firing an
                    // already-pending beep is harmless (single-slot queue).
                    if (audio_arbiter_active() == AUDIO_SOURCE_BEEP && audio_is_active()) {
                        s_alarm_beep_confirmed = true;
                    }
                    if (!s_alarm_beep_confirmed &&
                        (s_play_failed || (!audio_is_active() && elapsed_s > 20))) {
                        beep_start();
                    }
                    // Reflect the beep takeover on the ringing screen's source line.
                    if (s_alarm_beeping && s_alarm_src_lbl &&
                        strcmp(lv_label_get_text(s_alarm_src_lbl), T(STR_ALARM_BEEP)) != 0) {
                        lv_label_set_text(s_alarm_src_lbl, T(STR_ALARM_BEEP));
                    }
                    // Auto-stop at 30 min, or the instant this alarm is disabled
                    // (the kill switch): it targets the active alarm only.
                    if (elapsed_s >= 30 * 60 || !acfg->alarms[s_alarm_active_idx].enabled) {
                        alarm_finish(false);
                    }
                    break;
                }
                }
            }
            // Edge-triggered home rebuild on an alarm-state change, so the snooze
            // reminder line appears/disappears (net-state rebuild pattern).
            if (s_alarm_state != s_alarm_state_shown) {
                s_alarm_state_shown = s_alarm_state;
                if (s_active_builder == build_home) show(build_home);
            }

            // ---- Sleep timer engine (A2) ----
            // esp_timer based, so an SNTP wall-clock jump cannot affect it. Runs
            // after the alarm block on purpose: alarm_fire() (above) already
            // clears the sleep timer, so if both would land on the same tick the
            // alarm wins and s_sleep_stop_at_us is already 0 here.
            // End-of-track only fires from the s_advance hook, which the current
            // source may not have anymore (armed on an SD/podcast/library list,
            // then the user switched to a radio or Sendspin took over). Convert
            // to the 60-minute deadline, same rule as arming on such a source.
            if (s_sleep_end_of_track && !sleep_has_track_end()) {
                ESP_LOGI(TAG, "sleep timer: no track end anymore, falling back to 60 min");
                sleep_arm(60);
            }
            if (s_sleep_stop_at_us != 0 && now >= s_sleep_stop_at_us) {
                ESP_LOGI(TAG, "sleep timer: fired");
                if (source_sendspin_session_active()) source_sendspin_command(SENDSPIN_CMD_STOP);
                if (audio_is_active()) ui_stop();
                if (s_active_builder == build_now_playing ||
                    s_active_builder == build_sendspin_playing) show(build_home);
                toast(T(STR_SLEEP_STOPPED));
                sleep_clear();  // ui_stop() already does this when local audio was active
            }
            // Idle safety net: a source can die on its own (dead stream, decode
            // error) without ever calling ui_stop, which would otherwise leave
            // the timer armed with nothing left to stop. Debounced 5 s: right
            // after arming, audio_is_active() (s_open in audio.c) can still read
            // false for a moment while a radio/podcast stream is connecting
            // (TLS handshake, CDN redirect, prebuffer), and a bare one-tick check
            // would wrongly disarm a timer set during that window.
            static int s_sleep_idle_ticks;
            if (s_sleep_choice != 0 && !audio_is_active() && !source_sendspin_session_active()) {
                if (++s_sleep_idle_ticks >= 5) sleep_clear();
            } else {
                s_sleep_idle_ticks = 0;
            }
            // Countdown label, only while a screen showing it is active (same
            // gating as the alarm/home clock labels above).
            if (s_active_builder == build_now_playing || s_active_builder == build_sendspin_playing) {
                sleep_label_refresh();
            }

            // Parental block (quiet hours or exhausted daily limit),
            // edge-triggered. Runs after the alarm block on purpose: if the
            // alarm minute equals the block start, the alarm fires first and
            // the enter-edge skips the stop (the alarm always wins).
            bool q = quiet_active();
            bool blocked = q || limit_hit();
            if ((int)blocked != s_block_applied) {
                bool first = (s_block_applied == -1);
                s_block_applied = (int)blocked;
                if (blocked && !first && s_alarm_state == ALARM_IDLE && !s_alarm_beeping) {
                    if (source_sendspin_session_active())
                        source_sendspin_command(SENDSPIN_CMD_STOP);
                    // Also stop a stream reconnect in progress (audio is inactive
                    // between attempts, but the retry loop would otherwise bring
                    // the stream back during the window). ui_stop sets
                    // s_stop_requested, which ends the retries.
                    if (audio_is_active() || s_play_retrying) ui_stop();
                    if (s_active_builder == build_now_playing ||
                        s_active_builder == build_sendspin_playing ||
                        s_active_builder == build_game) show(build_home);
                    toast(q ? T(STR_QUIET_HOURS) : T(STR_LIMIT_REACHED));
                    ESP_LOGI(TAG, "%s, playback stopped",
                             q ? "quiet hours: window opened" : "daily limit: quota reached");
                }
                if (!blocked && !first) ESP_LOGI(TAG, "parental block lifted");
                if (s_active_builder == build_home) show(build_home);  // grey/un-grey tiles
            }
        }
    }

    // Reload the episodes screen once a podcast refresh finishes on the worker,
    // and surface a failed refresh instead of silently showing the stale list.
    if (s_refresh_done) {
        s_refresh_done = false;
        if (s_active_builder == build_episodes) {
            show(build_episodes);
            if (!s_refresh_ok && s_ep_msg) {
                lv_label_set_text(s_ep_msg, T(STR_REFRESH_FAILED));
            }
        }
    }

    // Rebuild the home screen when connectivity changes, so the network sources
    // grey out or re-enable themselves without the user leaving the screen. Also
    // leave the provisioning screen as soon as Wi-Fi connects (net keeps retrying
    // in the background while the setup AP is up): without this the user is
    // stranded on the QR screen even though the device is already online.
    net_state_t ns = net_state();
    if (ns != s_last_net) {
        s_last_net = ns;
        if (s_active_builder == build_setup && ns == NET_STATE_CONNECTED) {
            show(build_home);
        } else if (ns == NET_STATE_PROVISIONING && esp_timer_get_time() < 15 * 1000000LL) {
            // A device with no credentials reaches PROVISIONING moments after
            // boot (the UI starts before the network now): show the setup QR
            // screen, whatever is on screen (this early the user cannot be deep
            // into anything, and without Wi-Fi nothing else works). Only early
            // in boot: the late (~30 s) fallback AP must not hijack the screen
            // from a user already navigating or playing SD.
            show(build_setup);
        } else if (s_active_builder == build_home) {
            show(build_home);
        } else if (s_active_builder == build_episodes) {
            // Re-grey / un-grey the non-downloaded episodes on Wi-Fi changes.
            show(build_episodes);
        }
    }
    if (gpio_get_level(BOARD_BOOT_BTN_GPIO) == 0) {  // BOOT pressed (active low)
        lv_display_trigger_activity(s_disp);
    }
    uint32_t inactive = lv_display_get_inactive_time(s_disp);
    if (s_asleep) {
        if (inactive < 400) exit_sleep();
    } else if (s_sleep_ms > 0 && inactive >= s_sleep_ms &&
               s_active_builder != build_tuner &&
               s_memo_state != MEMO_UI_RECORDING) {
        // No screen sleep while tuning or recording a memo: the user watches
        // without touching (and a capture shows no audio activity).
        enter_sleep();
    }
}

esp_err_t ui_start(i2c_master_bus_handle_t i2c_bus)
{
    s_play_q = xQueueCreate(1, sizeof(play_req_t));
    ESP_RETURN_ON_FALSE(s_play_q, ESP_ERR_NO_MEM, TAG, "play queue alloc failed");

    // Resume any persisted background download job after a reboot. Set the idle
    // clock far in the past so the scheduler can start it as soon as Wi-Fi is up.
    if (config_store_get_dljob(&s_dljob) != ESP_OK) memset(&s_dljob, 0, sizeof(s_dljob));
    // Load a persisted podcast resume point, if any. Not acted on here: it is
    // used lazily when the user re-selects that episode (play_ctx_at_ex).
    if (config_store_get_resume(&s_resume) != ESP_OK) memset(&s_resume, 0, sizeof(s_resume));
    // Seed the daily usage counter so a power cycle cannot reset the child's
    // consumed time (a stale day is discarded by usage.c on the first tick).
    uint32_t use_day = 0, use_sec = 0;
    if (config_store_get_usage(&use_day, &use_sec) == ESP_OK)
        usage_seed((int)use_day, (int)use_sec);
    s_audio_idle_since_us = esp_timer_get_time();  // idle measured from boot: jobs and
                                                   // auto-maintenance run after 5 min idle
    lang_set_code(config_store_get()->ui.lang);    // apply the saved UI language
    apply_tz(config_store_get()->ui.tz);            // apply the saved timezone
    strlcpy(s_tz_applied, config_store_get()->ui.tz, sizeof(s_tz_applied));
    // 16 KB stack: enough now that the decoders are heap-allocated. (32 KB could
    // not be allocated contiguously this late in boot and failed silently.)
    // Pin to core 1 so the decoder always has CPU even while WiFi (core 0) and
    // the LVGL UI render a fast scroll: otherwise the decode wake-up can slip
    // past the I2S DMA drain and the audio drops out.
    // The stack MUST stay in internal RAM (no xTaskCreateWithCaps/PSRAM here):
    // this task writes to flash (dljob/resume persist in NVS, RSS manifests on
    // LittleFS), and flash writes disable the cache, making a PSRAM stack
    // unreachable mid-operation (crash).
    if (xTaskCreatePinnedToCore(play_task, "ui_play", 16384, NULL, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "failed to create play task");
    }

    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(init_display(&io, &s_panel), TAG, "display init failed");

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_affinity = 0;  // keep LVGL on core 0; core 1 is reserved for audio
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port init failed");

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = s_panel,
        .buffer_size = LCD_HRES * 40,
        // Single internal-RAM DMA buffer. A PSRAM double buffer made scrolling
        // smoother but pushed render+flush traffic onto the PSRAM bus, which
        // contended with the audio pipeline and caused dropouts while scrolling
        // large lists. Smooth scrolling comes from the static row titles instead.
        .double_buffer = false,
        .hres = LCD_HRES,
        .vres = LCD_VRES,
        .monochrome = false,
        .rotation = { .mirror_x = true },  // panel is mirrored on X by default
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "add disp failed");

    esp_lcd_touch_handle_t touch = NULL;
    if (init_touch(i2c_bus, &touch) == ESP_OK) {
        lvgl_port_touch_cfg_t touch_cfg = { .disp = s_disp, .handle = touch };
        s_touch_indev = lvgl_port_add_touch(&touch_cfg);
    } else {
        ESP_LOGW(TAG, "touch init failed, continuing without touch");
    }

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOARD_BOOT_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    const config_t *cfg = config_store_get();
    s_sleep_ms = (cfg && cfg->ui.screen_sleep_seconds > 0)
                 ? (uint32_t)cfg->ui.screen_sleep_seconds * 1000 : 0;
    if (cfg) audio_set_volume_limit(cfg->ui.volume_max);  // before the volume
    if (cfg && cfg->ui.volume >= 0 && cfg->ui.volume <= 100) {
        audio_set_volume(cfg->ui.volume);  // apply the configured default volume
    }

    // Screenshot handoff primitives (web server -> LVGL task). Created before the
    // timer so a request can never race an unbuilt semaphore.
    s_shot_lock = xSemaphoreCreateMutex();
    s_shot_done = xSemaphoreCreateBinary();

    if (lvgl_port_lock(0)) {
        // Register our child theme once: it inherits the LVGL default theme
        // (parent applies first) and its apply_cb attaches the per-theme style
        // layer to every new screen and button (see bugne_theme_apply).
        for (int i = 0; i < THEME_STYLE_COUNT; i++) lv_style_init(THEME_STYLES[i]);
        lv_theme_t *base = lv_display_get_theme(s_disp);
        s_bugne_theme = *base;
        lv_theme_set_parent(&s_bugne_theme, base);
        lv_theme_set_apply_cb(&s_bugne_theme, bugne_theme_apply);
        lv_display_set_theme(s_disp, &s_bugne_theme);
        // Apply the stored orientation and theme before anything is built, so
        // the first screen and the mini bar come up right.
        if (cfg && cfg->ui.orientation == 1) apply_orientation(true);
        if (cfg) apply_theme(cfg->ui.dark == 1, cfg->ui.accent);
        create_mini_bar();  // on the top layer, persists across screen changes
        show(net_state() == NET_STATE_PROVISIONING ? build_setup : build_home);
        // Poll often (50 ms) so a wake-up touch is caught and cancelled before
        // the finger lifts, otherwise it would click the button underneath.
        lv_timer_create(sleep_timer_cb, 50, NULL);
        lvgl_port_unlock();
    }
    // Backlight stayed off through init (no boot flash); the LVGL task renders
    // the first frame within one refresh period, so a tiny delay is enough to
    // light up on a drawn home screen instead of calling lv_refr_now from this
    // task (rendering from outside the LVGL task proved unsafe at boot).
    vTaskDelay(pdMS_TO_TICKS(120));
    bl_set(100);

    ESP_LOGI(TAG, "UI started");
    return ESP_OK;
}
