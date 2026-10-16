from pathlib import Path
import re
import subprocess
import tempfile

# Web remote control and status snapshot (ui.c): ui_remote_apply clamps and
# gates, ui_download_status phase mapping, ui_status source classification.
# The functions are extracted from ui.c and compiled against the real ui.h, so
# the enums and structs cannot drift from the firmware.

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
#include "ui.h"

#define LIB_PATH_MAX 256
#define LV_ANIM_OFF 0
enum { PLAY_CTX_NONE, PLAY_CTX_SD, PLAY_CTX_PODCAST };
enum { REQ_REFRESH_ALL = 1 };
enum { SENDSPIN_CMD_PAUSE = 1, SENDSPIN_CMD_PLAY, SENDSPIN_CMD_STOP,
       SENDSPIN_CMD_NEXT, SENDSPIN_CMD_PREVIOUS };

typedef struct { int kind; } play_req_t;
typedef struct { char url[128]; char name[64]; } config_webradio_t;
typedef struct { size_t webradio_count; config_webradio_t webradios[2]; } config_t;
typedef struct { bool active, scope_all, force; int pod_id; } dljob_t;

static size_t strlcpy(char *dst, const char *src, size_t cap)
{
    size_t len = strlen(src);
    if (cap) {
        size_t n = len < cap - 1 ? len : cap - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}

// UI state read or written by the extracted functions.
static dljob_t s_dljob;
static int s_play_ctx, s_play_index, s_np_dur_override_ms;
static bool s_user_stopped, s_refreshing, s_downloading, s_dl_queued, s_dl_cancel;
static int s_dl_done, s_dl_total;
static ui_dl_phase_t s_dl_phase;
static char s_remote_path[256], s_remote_title[64];
static int s_remote_arg, s_remote_cmd;
static int slider;
static void *s_np_vol = &slider;
static void (*s_active_builder)(void *);
static int s_sleep_choice;
static bool s_sleep_end_of_track;
static int64_t s_sleep_stop_at_us;
static char s_meta_title[64], s_now_title[64], s_meta_artist[64];
static config_t config = { 1, { { "http://radio", "Radio 1" } } };

// Recorded side effects and scripted inputs.
static bool ss_session, ss_active, denied, paused, active, seekable = true;
static int volume_set = -1, slider_set = -1, sleep_armed = -2, ss_cmd, played, persisted;
static uint32_t seek_to = 0xFFFFFFFF, dec_pos, dec_dur;
static char played_path[300], played_title[64];

static bool source_sendspin_session_active(void) { return ss_session; }
static bool source_sendspin_active(void) { return ss_active; }
static void source_sendspin_command(int cmd) { ss_cmd = cmd; }
static void source_sendspin_title(char *o, size_t n) { strlcpy(o, "SsTitle", n); }
static void source_sendspin_artist(char *o, size_t n) { strlcpy(o, "SsArtist", n); }
static void source_sendspin_progress(uint32_t *p, uint32_t *d) { *p = 1; *d = 2; }
static void source_stream_title(char *o, size_t n) { strlcpy(o, "IcyNow", n); }
static bool play_denied(void) { return denied; }
static bool audio_is_paused(void) { return paused; }
static void audio_set_paused(bool p) { paused = p; }
static bool audio_is_active(void) { return active; }
static int audio_get_volume(void) { return 42; }
static void audio_set_volume(int v) { volume_set = v; }
static void lv_slider_set_value(void *s, int v, int anim) { (void)s; (void)anim; slider_set = v; }
static void podcast_resume_save_current_position(void) {}
static void ui_stop(void) {}
static void build_now_playing(void *x) { (void)x; }
static void build_sendspin_playing(void *x) { (void)x; }
static void build_home(void *x) { (void)x; }
static void show(void (*b)(void *)) { s_active_builder = b; }
static void play_ctx_at(int i) { s_play_index = i; }
static const config_t *config_store_get(void) { return &config; }
static void ui_play(bool sd, const char *path, const char *title, int x)
{
    (void)sd; (void)x; played++;
    strlcpy(played_path, path, sizeof(played_path));
    strlcpy(played_title, title, sizeof(played_title));
}
static void req_post(play_req_t *r) { (void)r; }
static void dljob_persist(void) { persisted++; }
static void sleep_arm(int m) { sleep_armed = m; }
static void sleep_label_refresh(void) {}
static bool now_playing_seekable(void) { return seekable; }
static void decode_progress(uint32_t *p, uint32_t *d) { *p = dec_pos; *d = dec_dur; }
static void decode_seek(uint32_t t) { seek_to = t; }
static int64_t esp_timer_get_time(void) { return 0; }
'''

tests = r'''
int main(void)
{
    // Volume clamps to 0..100 and mirrors on the now-playing slider.
    ui_remote_apply(UI_REMOTE_VOLUME, 150);
    assert(volume_set == 100 && slider_set == 100);
    ui_remote_apply(UI_REMOTE_VOLUME, -5);
    assert(volume_set == 0 && slider_set == 0);

    // Sleep: above 180 clamps to 180, any negative means end-of-track.
    ui_remote_apply(UI_REMOTE_SLEEP, 200);
    assert(sleep_armed == 180);
    ui_remote_apply(UI_REMOTE_SLEEP, -7);
    assert(sleep_armed == -1);
    ui_remote_apply(UI_REMOTE_SLEEP, 0);
    assert(sleep_armed == 0);

    // Toggle: pausing always works, resuming is refused while denied.
    paused = false; denied = true;
    ui_remote_apply(UI_REMOTE_TOGGLE, 0);
    assert(paused);
    ui_remote_apply(UI_REMOTE_TOGGLE, 0);
    assert(paused);
    denied = false;
    ui_remote_apply(UI_REMOTE_TOGGLE, 0);
    assert(!paused);

    // Sendspin toggle: pause when active, refuse play when denied.
    ss_session = true; ss_active = true;
    ui_remote_apply(UI_REMOTE_TOGGLE, 0);
    assert(ss_cmd == SENDSPIN_CMD_PAUSE);
    ss_active = false; denied = true; ss_cmd = 0;
    ui_remote_apply(UI_REMOTE_TOGGLE, 0);
    assert(ss_cmd == 0);
    ss_session = false; denied = false;

    // Seek clamps to the decoder duration, negative to 0, ignored when unknown.
    dec_dur = 60000;
    ui_remote_apply(UI_REMOTE_SEEK, 90000);
    assert(seek_to == 60000);
    ui_remote_apply(UI_REMOTE_SEEK, -1);
    assert(seek_to == 0);
    seek_to = 0xFFFFFFFF; dec_dur = 0;
    ui_remote_apply(UI_REMOTE_SEEK, 1000);
    assert(seek_to == 0xFFFFFFFF);
    seekable = false; dec_dur = 60000;
    ui_remote_apply(UI_REMOTE_SEEK, 1000);
    assert(seek_to == 0xFFFFFFFF);
    seekable = true;

    // Play radio: out-of-range index ignored.
    ui_remote_apply(UI_REMOTE_PLAY_RADIO, 1);
    assert(played == 0);
    ui_remote_apply(UI_REMOTE_PLAY_RADIO, 0);
    assert(played == 1 && strcmp(played_path, "http://radio") == 0);

    // Play path: prefixed with /sdcard/, title falls back to the file name.
    ui_remote_play_path("music/a/song.mp3", NULL);
    assert(s_remote_cmd == UI_REMOTE_PLAY_PATH);
    ui_remote_apply(UI_REMOTE_PLAY_PATH, 0);
    assert(strcmp(played_path, "/sdcard/music/a/song.mp3") == 0);
    assert(strcmp(played_title, "song.mp3") == 0);
    ui_remote_play_path("x.mp3", "Given");
    ui_remote_apply(UI_REMOTE_PLAY_PATH, 0);
    assert(strcmp(played_title, "Given") == 0);
    denied = true; played = 0;
    ui_remote_apply(UI_REMOTE_PLAY_PATH, 0);
    assert(played == 0);
    denied = false;

    // Download jobs: scope and force per command, a running worker is cancelled.
    ui_remote_apply(UI_REMOTE_REDOWNLOAD_PODCAST, 7);
    assert(s_dljob.active && !s_dljob.scope_all && s_dljob.force && s_dljob.pod_id == 7);
    assert(s_dl_phase == UI_DL_SCHEDULED && !s_dl_cancel);
    s_downloading = true;
    ui_remote_apply(UI_REMOTE_DOWNLOAD_ALL, 7);
    assert(s_dljob.scope_all && !s_dljob.force && s_dljob.pod_id == 0 && s_dl_cancel);
    s_downloading = false; s_dl_cancel = false;
    ui_remote_apply(UI_REMOTE_CANCEL_DOWNLOAD, 0);
    assert(!s_dljob.active && s_dl_cancel && s_dl_phase == UI_DL_IDLE);

    // Download status phase mapping.
    ui_dl_status_t d;
    s_dljob.active = true; s_downloading = true; s_dl_phase = UI_DL_DOWNLOADING;
    s_dl_done = 5; s_dl_total = 10;
    ui_download_status(&d);
    assert(d.pending && d.active && d.phase == UI_DL_DOWNLOADING && d.done == 5 && d.total == 10);
    s_downloading = false; active = true;
    ui_download_status(&d);
    assert(d.phase == UI_DL_PAUSED && !d.active);
    active = false;
    ui_download_status(&d);
    assert(d.phase == UI_DL_SCHEDULED);
    s_dljob.active = false; s_dl_phase = UI_DL_SDFULL;
    ui_download_status(&d);
    assert(d.phase == UI_DL_SDFULL && !d.pending);
    s_dl_phase = UI_DL_REFRESHING;
    ui_download_status(&d);
    assert(d.phase == UI_DL_IDLE);

    // Status source classification.
    ui_status_t st;
    ss_session = true; ss_active = false;
    ui_status(&st);
    assert(st.active && st.paused && strcmp(st.source, "sendspin") == 0 && !st.seekable);
    ss_session = false; active = false;
    ui_status(&st);
    assert(!st.active && strcmp(st.source, "none") == 0 && st.volume == 42);
    active = true; s_play_ctx = PLAY_CTX_SD;
    strcpy(s_now_title, "file.mp3"); s_meta_title[0] = '\0';
    ui_status(&st);
    assert(strcmp(st.source, "sd") == 0 && strcmp(st.title, "file.mp3") == 0);
    strcpy(s_meta_title, "Tagged");
    ui_status(&st);
    assert(strcmp(st.title, "Tagged") == 0);
    s_play_ctx = PLAY_CTX_NONE;
    ui_status(&st);
    assert(strcmp(st.source, "radio") == 0 && strcmp(st.artist, "IcyNow") == 0);
    s_play_ctx = PLAY_CTX_PODCAST; dec_dur = 1000; s_np_dur_override_ms = 5000;
    ui_status(&st);
    assert(strcmp(st.source, "podcast") == 0 && st.dur_ms == 5000);

    // Sleep minutes: end-of-track is -1, otherwise rounded up.
    s_sleep_choice = 30; s_sleep_end_of_track = true;
    ui_status(&st);
    assert(st.sleep_min == -1);
    s_sleep_end_of_track = false; s_sleep_stop_at_us = 61LL * 1000000;
    ui_status(&st);
    assert(st.sleep_min == 2);

    puts("review ui remote tests: passed");
    return 0;
}
'''

body = "\n".join(function(name) for name in (
    "ui_remote_apply", "ui_remote", "ui_remote_play_path", "ui_download_status", "ui_status"
))

with tempfile.TemporaryDirectory(prefix="bugne-ui-remote-") as directory:
    source = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    source.write_text(prelude + body + tests)
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "test/host/stubs"),
                    "-I", str(ROOT / "components/ui/include"),
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
