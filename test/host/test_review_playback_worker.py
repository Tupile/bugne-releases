from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
UI = (ROOT / "components/ui/ui.c").read_text()


def function(text, name):
    match = re.search(r"^.*\b" + name + r"\([^;]*?\)\n\{", text, re.M)
    assert match, name
    start = match.start()
    opening = text.index("{", match.end() - 1)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


request = UI[UI.index("typedef enum { REQ_PLAY"):UI.index("static QueueHandle_t s_play_q;")]
prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#define PODCAST_URL_MAX 512
#define PODCAST_PATH_MAX 256
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static int lock_depth;
#define taskENTER_CRITICAL(m) do { assert(lock_depth == 0); lock_depth++; } while (0)
#define taskEXIT_CRITICAL(m) do { assert(lock_depth == 1); lock_depth--; } while (0)
#define pdTRUE 1
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(n) (n)
#define PLAY_CTX_NONE 0
#define PLAY_CTX_SD 1
#define PLAY_CTX_PODCAST 2
#define PLAY_CTX_LIBRARY 3
#define MEMO_UI_IDLE 0
#define MEMO_UI_PREVIEW 2
#define MEMO_UI_PICK_PEER 3
#define MEMO_PEERS_MAX 8
#define MALLOC_CAP_SPIRAM 0
#define ART_MAX_JPEG (512 * 1024)
typedef int esp_err_t;
static uint32_t s_play_generation = 1, s_advance_generation, s_worker_generation;
static uint32_t s_refresh_generation, s_refresh_owner_generation;
static bool s_stop_requested, s_advance, s_play_retrying, s_dl_cancel;
static bool s_play_failed, s_play_failed_local, s_refresh_ok, s_refreshing, s_refresh_done;
static volatile bool s_refresh_cancel;
static bool s_play_worker_busy, s_downloading, s_dl_queued;
static int s_play_ctx, s_memo_peer_count, s_memo_state, s_memo_peers[8], s_play_q;
static char s_art_pending_path[PODCAST_PATH_MAX];
static uint32_t sd_generation, stream_generation, observed_generation;
static int calls, refresh_calls, queued, hook, source_result, art_count, delay_calls, session_clear_at;
static bool source_completed, session;
static int arbiter_clear_at;
static int arbiter = -1;
#define AUDIO_SOURCE_NONE -1
#define AUDIO_SOURCE_SENDSPIN 3
static int64_t clock_us;
static jmp_buf finished;
static void play_cancel(void);
static void refresh_cancel(void);
static void cancel_play(void) { play_cancel(); sd_generation++; stream_generation++; s_dl_cancel = true; }
static size_t strlcpy(char *d, const char *s, size_t n) {
    size_t len = strlen(s);
    if (n) { size_t copy = len < n - 1 ? len : n - 1; memcpy(d, s, copy); d[copy] = 0; }
    return len;
}
static int uxQueueMessagesWaiting(int q) { (void)q; return queued; }
static uint32_t source_sd_generation(void) { assert(!lock_depth); return sd_generation; }
static uint32_t source_stream_generation(void) { assert(!lock_depth); return stream_generation; }
static bool source_sendspin_session_active(void) { return session; }
static bool source_sd_completed(void) { return source_completed; }
static bool source_stream_completed(void) { return source_completed; }
static esp_err_t source_play(const char *target, uint32_t generation) {
    (void)target;
    assert(!lock_depth);
    assert(!session && arbiter == AUDIO_SOURCE_NONE);
    calls++;
    if (calls == 1) observed_generation = generation;
    else assert(generation == observed_generation);
    if (hook == 2) cancel_play();
    if (hook == 4 && calls == 2) { source_completed = true; return ESP_OK; }
    return source_result;
}
static esp_err_t source_sd_play_generation(const char *p, uint32_t g) { return source_play(p, g); }
static esp_err_t source_stream_play_generation(const char *p, uint32_t g) { return source_play(p, g); }
static void source_stream_set_preroll_decoy(bool b) { (void)b; }
static int64_t esp_timer_get_time(void) { return clock_us; }
static void vTaskDelay(int ms) {
    clock_us += (int64_t)ms * 1000;
    delay_calls++;
    if (session_clear_at >= 0 && delay_calls >= session_clear_at) {
        session = false;
    }
    if (arbiter_clear_at >= 0 && delay_calls >= arbiter_clear_at) {
        arbiter = AUDIO_SOURCE_NONE;
    }
    if (hook == 3) cancel_play();
    if (hook == 10 && calls == 1) session = true;
}
static void decode_set_start_skip_ms(uint32_t ms) { (void)ms; }
static void decode_progress(uint32_t *p, uint32_t *d) { *p = 3000; *d = 9000; }
static void worker_run_job(void) {}
static void beep_run(void) {}
static void memo_record_run(void) {}
static void memo_play_run(const char *p) { (void)p; }
static int net_memo_peers(int *p, int n) { (void)p; (void)n; return 0; }
static void memo_send_run(int p) { (void)p; }
static void talkie_send_run(void) {}
static bool audio_is_active(void) { return false; }
static int audio_arbiter_active(void) { return arbiter; }
static void audio_unused(bool (*fn)(void)) { (void)fn; }
static void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static void heap_caps_free(void *p) { free(p); }
static void art_clear(void) { art_count = 0; }
static void art_set_jpeg(const void *p, size_t n) { (void)p; (void)n; art_count++; }
static size_t cover_read(void *p, size_t s, size_t n, FILE *f) {
    size_t got = fread(p, s, n, f);
    if (hook == 1) cancel_play();
    return got;
}
#define fread cover_read
typedef struct { int id; char title[16]; char rss_url[32]; } config_podcast_t;
typedef struct {
    size_t podcast_count, webradio_count;
    config_podcast_t podcasts[2];
    struct { char url[32]; int skip_preroll; } webradios[1];
} config_t;
static config_t config = {.podcast_count = 2};
static const config_t *config_store_get(void) { return &config; }
static bool job_in_scope(const config_podcast_t *p) { (void)p; return true; }
static int s_dl_phase;
#define UI_DL_REFRESHING 1
static esp_err_t podcast_refresh_cancelable(int id, const char *name, const char *url, volatile bool *cancel) {
    (void)id; (void)name; (void)url;
    if (*cancel) return ESP_FAIL;
    refresh_calls++;
    if (hook == 5) refresh_cancel();
    if (hook == 6) queued = 1;
    if (hook == 7) s_dl_cancel = true;
    return *cancel ? ESP_FAIL : ESP_OK;
}
'''
queue = r'''
static play_req_t slot;
static int xQueuePeek(int q, play_req_t *r, int ticks) {
    (void)q; (void)ticks;
    if (!queued) return 0;
    *r = slot;
    return pdTRUE;
}
static void xQueueOverwrite(int q, const play_req_t *r) { (void)q; slot = *r; queued = 1; }
static int xQueueReceive(int q, play_req_t *r, int ticks) {
    (void)q; (void)ticks;
    if (!queued) longjmp(finished, 1);
    *r = slot;
    queued = 0;
    return pdTRUE;
}
'''
names = ("play_current", "refresh_cancel", "refresh_begin", "refresh_finished", "play_cancel",
         "play_ended", "play_retrying", "load_cover_file", "job_refresh", "play_task", "req_abandoned", "req_post")
body = "\n".join(function(UI, name) for name in names)

cases = r'''
static void run(void) { if (!setjmp(finished)) play_task(NULL); }
static void reset(void) {
    cancel_play(); s_stop_requested = false;
    queued = calls = refresh_calls = hook = art_count = 0;
    s_advance = s_play_failed = s_refresh_done = s_refreshing = false;
    source_completed = true; source_result = ESP_OK;
    session = false; clock_us = 0; delay_calls = 0; session_clear_at = -1;
    arbiter_clear_at = -1;
    arbiter = AUDIO_SOURCE_NONE;
    s_play_ctx = PLAY_CTX_LIBRARY;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    play_req_t req = {.kind = REQ_PLAY, .is_file = true};
    strlcpy(req.target, "track.mp3", sizeof(req.target));
    reset(); req_post(&req); cancel_play(); run();
    assert(calls == 0 && !s_advance && !s_play_failed);
    reset(); hook = 1; strlcpy(s_art_pending_path, argv[1], sizeof(s_art_pending_path));
    req_post(&req); assert(slot.cover[0] && !s_art_pending_path[0]); run();
    assert(calls == 0 && !s_advance && !s_play_failed && !art_count);
    reset(); hook = 2; req_post(&req); run();
    assert(calls == 1 && !s_advance && !s_play_failed);
    reset(); req_post(&req); run();
    assert(calls == 1 && s_advance && !s_play_failed);
    reset(); source_result = ESP_FAIL; req_post(&req); run();
    assert(calls == 1 && !s_advance && s_play_failed);
    reset(); req.is_file = false; s_play_ctx = PLAY_CTX_PODCAST;
    hook = 3; source_completed = false; source_result = ESP_FAIL; req_post(&req); run();
    assert(calls == 1 && !s_advance && !s_play_failed && !s_play_retrying);
    reset(); req.is_file = false; s_play_ctx = PLAY_CTX_PODCAST; hook = 4;
    source_completed = false; source_result = ESP_FAIL; req_post(&req); run();
    assert(calls == 2 && s_advance && !s_play_failed && !s_play_retrying);
    reset(); req.is_file = true; req.kind = REQ_REFRESH_ALL; req_post(&req); run();
    assert(refresh_calls == 2 && !s_refreshing && s_refresh_done && s_refresh_ok);
    reset(); req.kind = REQ_REFRESH_ALL; req_post(&req); cancel_play(); run();
    assert(refresh_calls == 2 && !s_refreshing && s_refresh_done && s_refresh_ok);
    reset(); req.kind = REQ_REFRESH_ALL; req_post(&req);
    play_req_t newer = {.kind = REQ_PLAY, .is_file = true};
    req_post(&newer);
    assert(!s_refreshing && s_refresh_done && !s_refresh_ok);
    run();
    assert(refresh_calls == 0 && calls == 1 && !s_refreshing && s_refresh_done && !s_refresh_ok);
    reset(); req.kind = REQ_REFRESH_ALL; hook = 5; req_post(&req); run();
    assert(refresh_calls == 1 && !s_refreshing && s_refresh_done && !s_refresh_ok);
    reset(); queued = 0; hook = 7; s_dl_cancel = false;
    strlcpy(config.podcasts[0].rss_url, "feed-a.example/rss", sizeof(config.podcasts[0].rss_url));
    assert(!job_refresh() && refresh_calls == 1);
    reset(); queued = 0; hook = 6; s_dl_cancel = false;
    assert(!job_refresh() && refresh_calls == 1);
    reset(); hook = 6; s_dl_cancel = false; queued = 0;
    assert(!job_refresh() && refresh_calls == 1);
    req.kind = REQ_PLAY;
    reset(); req.is_file = false; s_play_ctx = PLAY_CTX_PODCAST; source_completed = true;
    session = true; session_clear_at = 3; arbiter = AUDIO_SOURCE_SENDSPIN;
    arbiter_clear_at = 6;
    req_post(&req); run();
    assert(calls == 1 && s_advance && !s_play_failed && !s_play_retrying);
    req.kind = REQ_PLAY;
    reset(); req.is_file = false; s_play_ctx = PLAY_CTX_PODCAST; source_completed = false;
    session = true; arbiter = AUDIO_SOURCE_NONE; req_post(&req); run();
    assert(calls == 0 && !s_advance && s_play_failed && !s_play_retrying);
    reset(); req.is_file = false; s_play_ctx = PLAY_CTX_PODCAST; source_completed = false;
    session = false; arbiter = AUDIO_SOURCE_SENDSPIN; req_post(&req); run();
    assert(calls == 0 && !s_advance && s_play_failed && !s_play_retrying);
    reset(); req.is_file = false; s_play_ctx = PLAY_CTX_PODCAST;
    source_completed = false; source_result = ESP_FAIL; hook = 10;
    req_post(&req); run();
    assert(calls == 1 && !s_advance && !s_play_failed && !s_play_retrying);
    reset(); req.is_file = false; s_play_ctx = PLAY_CTX_PODCAST; source_completed = true;
    session = true; arbiter = AUDIO_SOURCE_SENDSPIN;
    req_post(&req); cancel_play(); run();
    assert(calls == 0 && !s_advance && !s_play_failed && !s_play_retrying);
    audio_unused(audio_is_active);
    puts("review queue/worker cancellation: 17 cases passed");
}
'''
def test(ui_text):
    body = "\n".join(function(ui_text, name) for name in names)
    with tempfile.TemporaryDirectory(prefix="bugne-review-worker-") as directory:
        source = Path(directory) / "test.c"
        binary = Path(directory) / "test"
        cover = Path(directory) / "cover.jpg"
        cover.write_bytes(b"jpeg-test")
        source.write_text(prelude + request + queue + body + cases)
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary), str(cover)], check=True)


test(UI)
