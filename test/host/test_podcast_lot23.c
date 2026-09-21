#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_http_client.h"

static char root[128];
static volatile bool cancelled;
static int fail_read, cancel_read, fail_seek, fail_tell, read_calls, seek_calls;
static int fail_dir_read, dir_calls, fail_dir_close, stat_calls;
static const char *fail_close_path, *cancel_close_path, *fail_write_path;
static bool fail_remove_final, fail_rename_final, fail_input_close;
static bool sd_absent;
int lot23_alloc_calls, lot23_alloc_fail_nth;
static struct {
    FILE *f;
    char path[512];
    bool error;
    bool input;
} files[16];

static void mapped(const char *path, char *out, size_t cap)
{
    assert(path[0] == '/');
    assert(snprintf(out, cap, "%s%s", root, path) < (int)cap);
}

static int slot(FILE *f)
{
    for (int i = 0; i < 16; i++) if (files[i].f == f) return i;
    abort();
}

static FILE *test_fopen(const char *path, const char *mode)
{
    char p[512];
    mapped(path, p, sizeof(p));
    FILE *f = fopen(p, mode);
    if (!f) return NULL;
    for (int i = 0; i < 16; i++) {
        if (files[i].f) continue;
        files[i].f = f;
        files[i].error = false;
        files[i].input = strchr(mode, 'r') != NULL;
        strcpy(files[i].path, path);
        return f;
    }
    abort();
}

static int test_fclose(FILE *f)
{
    int i = slot(f);
    bool fail = (fail_close_path && strstr(files[i].path, fail_close_path)) ||
                (fail_input_close && files[i].input);
    if (cancel_close_path && strstr(files[i].path, cancel_close_path)) cancelled = true;
    files[i].f = NULL;
    int r = fclose(f);
    return fail ? EOF : r;
}

static size_t test_fread(void *buf, size_t size, size_t n, FILE *f)
{
    read_calls++;
    if (cancel_read == read_calls) cancelled = true;
    if (fail_read == read_calls) {
        files[slot(f)].error = true;
        errno = EIO;
        return 0;
    }
    return fread(buf, size, n, f);
}

static int test_ferror(FILE *f)
{
    return files[slot(f)].error || ferror(f);
}

static size_t test_fwrite(const void *buf, size_t size, size_t n, FILE *f)
{
    if (fail_write_path && strstr(files[slot(f)].path, fail_write_path)) return 0;
    return fwrite(buf, size, n, f);
}

static int test_fseek(FILE *f, long pos, int whence)
{
    if (++seek_calls == fail_seek) { errno = EIO; return -1; }
    return fseek(f, pos, whence);
}

static long test_ftell(FILE *f)
{
    if (fail_tell) { errno = EIO; return -1; }
    return ftell(f);
}

static int test_remove(const char *path)
{
    if (fail_remove_final && strcmp(path, "/sdcard/audio.mp3") == 0) {
        errno = EACCES;
        return -1;
    }
    char p[512];
    mapped(path, p, sizeof(p));
    return remove(p);
}

static int test_rename(const char *from, const char *to)
{
    char a[512], b[512];
    mapped(from, a, sizeof(a));
    mapped(to, b, sizeof(b));
    if (fail_rename_final) { errno = EIO; return -1; }
    if (strncmp(to, "/sdcard/", 8) == 0 && access(b, F_OK) == 0) {
        errno = EEXIST;
        return -1;
    }
    return rename(a, b);
}

static int test_mkdir(const char *path, mode_t mode)
{
    char p[512];
    mapped(path, p, sizeof(p));
    return mkdir(p, mode);
}

static int test_stat(const char *path, struct stat *st)
{
    char p[512];
    mapped(path, p, sizeof(p));
    stat_calls++;
    return stat(p, st);
}

static DIR *test_opendir(const char *path)
{
    char p[512];
    mapped(path, p, sizeof(p));
    return opendir(p);
}

static struct dirent *test_readdir(DIR *d)
{
    if (++dir_calls == fail_dir_read) { errno = EIO; return NULL; }
    return readdir(d);
}

static int test_closedir(DIR *d)
{
    int r = closedir(d);
    return fail_dir_close ? -1 : r;
}

#define fopen test_fopen
#define fclose test_fclose
#define fread test_fread
#define ferror test_ferror
#define fwrite test_fwrite
#define fseek test_fseek
#define ftell test_ftell
#define remove test_remove
#define rename test_rename
#define mkdir test_mkdir
#define opendir test_opendir
#define readdir test_readdir
#define closedir test_closedir
#define stat(path, st) test_stat(path, st)
#include "../../components/podcast/podcast.c"
#undef fopen
#undef fclose
#undef fread
#undef ferror
#undef fwrite
#undef fseek
#undef ftell
#undef remove
#undef rename
#undef mkdir
#undef stat
#undef opendir
#undef readdir
#undef closedir

static const char rss[] = "<rss><channel><title>Feed</title>"
    "<image><url>http://fixture/cover</url></image>"
    "<item><title>Episode</title><enclosure url=\"http://fixture/audio.mp3\"/>"
    "</item></channel></rss>";
static unsigned char audio[417 * 120];
static struct lot23_http {
    int kind;
    size_t offset;
    int redirects;
} http;
static struct {
    const char *body;
    size_t len;
    int cancel_phase, cancel_kind, redirect_count, opens, inits, cleanups;
    int fail_headers, fail_redirect, incomplete, read_error, cover_error;
    int64_t clock, advance;
} net;

enum { OPEN = 1, HEADERS, REDIRECT, BODY, CLEANUP };

static void http_event(int phase)
{
    net.clock += net.advance;
    if (phase == net.cancel_phase && http.kind == net.cancel_kind) cancelled = true;
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    assert(cfg->timeout_ms == 3000);
    memset(&http, 0, sizeof(http));
    http.kind = strstr(cfg->url, "cover") != NULL;
    net.inits++;
    return &http;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t c, int len)
{
    (void)c; (void)len;
    net.opens++;
    http_event(OPEN);
    return ESP_OK;
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c)
{
    (void)c;
    http_event(HEADERS);
    return net.fail_headers ? -1 : 0;
}

int esp_http_client_get_status_code(esp_http_client_handle_t c)
{
    if (c->kind && net.cover_error) return 503;
    return c->redirects < net.redirect_count ? 302 : 200;
}

esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t c)
{
    http_event(REDIRECT);
    c->redirects++;
    return net.fail_redirect ? ESP_FAIL : ESP_OK;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t c)
{
    (void)c;
    return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c)
{
    (void)c;
    net.cleanups++;
    http_event(CLEANUP);
    return ESP_OK;
}

int esp_http_client_read(esp_http_client_handle_t c, char *buf, int len)
{
    http_event(BODY);
    if (net.read_error) return -1;
    const char *body = c->kind ? "cover-data" : net.body;
    size_t size = c->kind ? strlen(body) : net.len;
    size_t n = size - c->offset;
    if (n > (size_t)len) n = len;
    memcpy(buf, body + c->offset, n);
    c->offset += n;
    return (int)n;
}

bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c)
{
    (void)c;
    return !net.incomplete;
}

int64_t esp_timer_get_time(void)
{
    return net.clock;
}

bool source_sd_present(void)
{
    return !sd_absent;
}

FILE *source_sd_create(const char *rel)
{
    char path[512];
    assert(snprintf(path, sizeof(path), "/sdcard/%s", rel) < (int)sizeof(path));
    return test_fopen(path, "wb");
}

static void put(const char *path, const void *data, size_t size)
{
    FILE *f = test_fopen(path, "wb");
    assert(f);
    assert(fwrite(data, 1, size, f) == size);
    assert(test_fclose(f) == 0);
}

static bool exists(const char *path)
{
    char p[512];
    mapped(path, p, sizeof(p));
    return access(p, F_OK) == 0;
}

static void matches(const char *path, const void *data, size_t size)
{
    FILE *f = test_fopen(path, "rb");
    assert(f);
    unsigned char *buf = malloc(size + 1);
    assert(buf);
    assert(fread(buf, 1, size + 1, f) == size);
    assert(memcmp(buf, data, size) == 0);
    free(buf);
    assert(test_fclose(f) == 0);
}

static void reset(void)
{
    for (int i = 0; i < 16; i++) assert(!files[i].f);
    cancelled = false;
    fail_read = cancel_read = fail_seek = fail_tell = read_calls = seek_calls = 0;
    fail_dir_read = dir_calls = fail_dir_close = stat_calls = 0;
    fail_close_path = cancel_close_path = fail_write_path = NULL;
    fail_input_close = false;
    sd_absent = false;
    lot23_alloc_calls = lot23_alloc_fail_nth = 0;
    fail_remove_final = fail_rename_final = false;
    memset(&net, 0, sizeof(net));
    net.body = (const char *)audio;
    net.len = sizeof(audio);
    test_remove("/sdcard/audio.mp3.part");
    test_remove("/sdcard/audio.mp3.trim.part");
    test_remove("/sdcard/podcasts/Feed/cover.jpg");
    test_remove("/sdcard/podcasts/Feed/cover.jpg.part");
    put("/sdcard/audio.mp3", "old", 3);
    put("/littlefs/podcasts/7.json", "old-manifest", 12);
}

static const podcast_episode_t episode = {
    .episode_url = "http://fixture/audio.mp3",
    .cache_path = "/sdcard/audio.mp3"
};

static void failed_download(esp_err_t expected)
{
    assert(podcast_download_episode(&episode, 1, &cancelled) == expected);
    fail_close_path = cancel_close_path = NULL;
    fail_input_close = false;
    matches(episode.cache_path, "old", 3);
    assert(!exists("/sdcard/audio.mp3.part"));
    assert(!exists("/sdcard/audio.mp3.trim.part"));
    assert(net.inits == net.cleanups);
}

static void test_trim_failures(void)
{
    reset(); fail_close_path = ".trim.part"; failed_download(ESP_FAIL);
    reset(); fail_close_path = "audio.mp3.part"; failed_download(ESP_FAIL);
    reset(); fail_write_path = ".trim.part"; failed_download(ESP_FAIL);
    reset(); fail_write_path = "audio.mp3.part"; failed_download(ESP_FAIL);
    reset(); fail_read = 1; failed_download(ESP_FAIL);
    reset(); fail_read = 5; failed_download(ESP_FAIL);
    reset(); fail_read = 42; failed_download(ESP_FAIL);
    reset(); fail_read = 43; failed_download(ESP_FAIL);
    reset(); fail_seek = 1; failed_download(ESP_FAIL);
    reset(); fail_seek = 5; failed_download(ESP_FAIL);
    reset(); fail_seek = 43; failed_download(ESP_FAIL);
    reset(); fail_tell = 1; failed_download(ESP_FAIL);
    reset(); fail_input_close = true; failed_download(ESP_FAIL);
    reset(); cancel_read = 5; failed_download(ESP_ERR_INVALID_STATE);
    reset(); cancel_read = 42; failed_download(ESP_ERR_INVALID_STATE);
    reset(); cancel_close_path = ".trim.part"; failed_download(ESP_ERR_INVALID_STATE);
    reset(); cancelled = true; failed_download(ESP_ERR_INVALID_STATE);
    reset(); net.incomplete = 1; failed_download(ESP_FAIL);
    reset(); net.read_error = 1; failed_download(ESP_FAIL);
}

static void test_trim_success(void)
{
    reset();
    assert(podcast_download_episode(&episode, 1, &cancelled) == ESP_OK);
    matches(episode.cache_path, audio + 39 * 417, sizeof(audio) - 39 * 417);
    assert(!exists("/sdcard/audio.mp3.part"));
    assert(!exists("/sdcard/audio.mp3.trim.part"));
    reset();
    net.body = "unsupported-format";
    net.len = strlen(net.body);
    assert(podcast_download_episode(&episode, 1, &cancelled) == ESP_OK);
    matches(episode.cache_path, net.body, net.len);
    reset();
    net.len = 417;
    assert(podcast_download_episode(&episode, 1, &cancelled) == ESP_OK);
    matches(episode.cache_path, audio, 417);
    reset(); fail_remove_final = true;
    assert(podcast_download_episode(&episode, 1, &cancelled) == ESP_FAIL);
    matches(episode.cache_path, "old", 3);
    matches("/sdcard/audio.mp3.trim.part", audio + 39 * 417, sizeof(audio) - 39 * 417);
    reset(); fail_rename_final = true;
    assert(podcast_download_episode(&episode, 1, &cancelled) == ESP_FAIL);
    assert(!exists(episode.cache_path));
    matches("/sdcard/audio.mp3.trim.part", audio + 39 * 417, sizeof(audio) - 39 * 417);
}

static void refresh_setup(void)
{
    reset();
    net.body = rss;
    net.len = strlen(rss);
}

static void failed_refresh(esp_err_t expected)
{
    assert(podcast_refresh_cancelable(7, "Feed", "http://fixture/rss", &cancelled) == expected);
    fail_close_path = cancel_close_path = NULL;
    matches("/littlefs/podcasts/7.json", "old-manifest", 12);
    assert(!exists("/littlefs/podcasts/7.json.tmp"));
    assert(!exists("/sdcard/podcasts/Feed/cover.jpg.part"));
    assert(net.inits == net.cleanups);
}

// The cover is fetched only once the manifest is safely renamed into place, so
// anything going wrong during the cover fetch (a cancel, an HTTP error, a failed
// close) costs the artwork and NOTHING else: the episode list is already stored
// and the refresh reports success.
static void cover_lost_manifest_kept(void)
{
    assert(podcast_refresh_cancelable(7, "Feed", "http://fixture/rss", &cancelled) == ESP_OK);
    fail_close_path = cancel_close_path = NULL;
    assert(!exists("/littlefs/podcasts/7.json.tmp"));
    assert(!exists("/sdcard/podcasts/Feed/cover.jpg"));
    assert(!exists("/sdcard/podcasts/Feed/cover.jpg.part"));
    assert(net.inits == net.cleanups);
    podcast_episode_t *eps = NULL;
    size_t cap = 0, count = 0;
    assert(podcast_read_manifest(7, &eps, &cap, &count) == ESP_OK);
    assert(count == 1 && strcmp(eps[0].title, "Episode") == 0);
    free(eps);
}

static void test_refresh(void)
{
    refresh_setup(); cancelled = true; failed_refresh(ESP_ERR_INVALID_STATE);
    assert(net.inits == 0);
    // A cancel during the RSS fetch keeps the previous manifest untouched.
    for (int phase = OPEN; phase <= CLEANUP; phase++) {
        refresh_setup();
        net.cancel_kind = 0;
        net.cancel_phase = phase;
        net.redirect_count = phase == REDIRECT ? 1 : 0;
        failed_refresh(ESP_ERR_INVALID_STATE);
        assert(!exists("/sdcard/podcasts/Feed/cover.jpg"));
    }
    // A cancel during the COVER fetch must not cost the episode list.
    for (int phase = OPEN; phase <= CLEANUP; phase++) {
        refresh_setup();
        net.cancel_kind = 1;
        net.cancel_phase = phase;
        net.redirect_count = phase == REDIRECT ? 1 : 0;
        cover_lost_manifest_kept();
    }
    refresh_setup(); cancel_close_path = "7.json.tmp"; failed_refresh(ESP_ERR_INVALID_STATE);
    refresh_setup(); cancel_close_path = "cover.jpg.part"; cover_lost_manifest_kept();
    refresh_setup(); fail_close_path = "7.json.tmp"; failed_refresh(ESP_FAIL);
    refresh_setup(); fail_rename_final = true; failed_refresh(ESP_FAIL);
    refresh_setup(); net.fail_headers = 1; failed_refresh(ESP_FAIL);
    refresh_setup(); net.redirect_count = 1; net.fail_redirect = 1; failed_refresh(ESP_FAIL);
    refresh_setup(); net.redirect_count = 9; failed_refresh(ESP_FAIL); assert(net.opens == 6);
    refresh_setup(); net.incomplete = 1; failed_refresh(ESP_FAIL);
    refresh_setup(); net.len -= 6; failed_refresh(ESP_FAIL);
    refresh_setup(); net.advance = 16000000; failed_refresh(ESP_FAIL);
    refresh_setup(); net.cover_error = 1; cover_lost_manifest_kept();
    refresh_setup(); net.redirect_count = 1;
    assert(podcast_refresh_cancelable(7, "Feed", "http://fixture/rss", &cancelled) == ESP_OK);
    matches("/sdcard/podcasts/Feed/cover.jpg", "cover-data", 10);
    assert(!exists("/littlefs/podcasts/7.json.tmp"));
}

static void test_cache_scan(void)
{
    reset();
    assert(test_mkdir("/sdcard/scan", 0700) == 0);
    for (int i = 0; i < 600; i++) {
        char path[80];
        snprintf(path, sizeof(path), "/sdcard/scan/%03d.mp3", i);
        put(path, "x", 1);
    }
    cache_scan_t cs;
    cache_scan_start(&cs, "/sdcard/scan");
    assert(cs.n == 512 && !cs.complete);
    int misses = 0;
    for (int i = 0; i < 600; i++) {
        podcast_episode_t ep = {0};
        char name[32];
        snprintf(name, sizeof(name), "%03d.mp3", i);
        snprintf(ep.cache_path, sizeof(ep.cache_path), "/sdcard/scan/%s", name);
        if (!cache_scan_has(&cs, name)) misses++;
        assert(ep_is_cached(&cs, &ep));
    }
    assert(misses > 0);
    cache_scan_done(&cs);
    fail_dir_read = 1; dir_calls = 0;
    cache_scan_start(&cs, "/sdcard/scan");
    assert(!cs.complete);
    podcast_episode_t ep = {.cache_path = "/sdcard/scan/599.mp3"};
    assert(ep_is_cached(&cs, &ep));
    cache_scan_done(&cs);
    fail_dir_read = 0;
    assert(test_mkdir("/sdcard/small", 0700) == 0);
    put("/sdcard/small/known.mp3", "x", 1);
    fail_dir_close = 1;
    cache_scan_start(&cs, "/sdcard/small");
    assert(!cs.complete);
    cache_scan_done(&cs);
    fail_dir_close = 0;
    cache_scan_start(&cs, "/sdcard/small");
    assert(cs.complete);
    strcpy(ep.cache_path, "/sdcard/small/missing.mp3");
    stat_calls = 0;
    assert(!ep_is_cached(&cs, &ep) && stat_calls == 0);
    assert(test_mkdir("/sdcard/small/nested", 0700) == 0);
    put("/sdcard/small/nested/new.mp3", "x", 1);
    strcpy(ep.cache_path, "/sdcard/small/nested/new.mp3");
    assert(ep_is_cached(&cs, &ep));
    cache_scan_done(&cs);
}

// Episodes the feed no longer lists --------------------------------------
//
// A refresh rewrites the manifest from the RSS alone, so an episode dropped by
// the feed used to vanish from the list while its downloaded audio stayed on
// the card forever, unreachable. The retention pass carries those entries over
// verbatim as long as the file is really there.

#define FEED_DIR "/sdcard/podcasts/Feed"
#define EP_FIELDS(t) "{\"title\":\"" t "\",\"date\":\"Mon, 01 Jan 2026 00:00:00 GMT\"," \
    "\"duration_seconds\":42,\"episode_url\":\"http://fixture/" t ".mp3\"," \
    "\"cache_path\":\"" FEED_DIR "/" t ".mp3\",\"cached\":false"
#define OLD_EP(t)          EP_FIELDS(t) "}"
#define OLD_EP_RETAINED(t) EP_FIELDS(t) ",\"retained\":true}"

static char mbuf[160000];

static void old_manifest(const char *episodes)
{
    int n = snprintf(mbuf, sizeof(mbuf), "{\"schema_version\":1,\"podcast_title\":\"Feed\","
                     "\"rss_url\":\"http://fixture/rss\",\"generated_at\":\"T\","
                     "\"episodes\":[%s]}", episodes);
    assert(n > 0 && n < (int)sizeof(mbuf));
    put("/littlefs/podcasts/7.json", mbuf, (size_t)n);
}

// The manifest as written: the parsed form cannot show the retained marker
// (the reader ignores it, like cached) nor the order of the entries.
static const char *manifest_text(void)
{
    FILE *f = test_fopen("/littlefs/podcasts/7.json", "rb");
    assert(f);
    size_t n = test_fread(mbuf, 1, sizeof(mbuf) - 1, f);
    mbuf[n] = '\0';
    assert(test_fclose(f) == 0);
    return mbuf;
}

static size_t read_eps(podcast_episode_t **eps)
{
    size_t cap = 0, count = 0;
    *eps = NULL;
    assert(podcast_read_manifest(7, eps, &cap, &count) == ESP_OK);
    return count;
}

static void refresh_ok(void)
{
    assert(podcast_refresh_cancelable(7, "Feed", "http://fixture/rss", &cancelled) == ESP_OK);
    assert(!exists("/littlefs/podcasts/7.json.tmp"));
}

static void test_retention(void)
{
    podcast_episode_t *eps;
    size_t n;

    // Dropped by the feed with its audio on the card: kept, after the feed's own
    // episodes, every field byte-identical (recomputing cache_path would orphan
    // the file, the collision suffix depends on the position in the feed).
    refresh_setup();
    old_manifest(OLD_EP("Gone"));
    put(FEED_DIR "/Gone.mp3", "x", 1);
    refresh_ok();
    n = read_eps(&eps);
    assert(n == 2);
    assert(strcmp(eps[0].title, "Episode") == 0 && !eps[0].cached);
    assert(strcmp(eps[1].title, "Gone") == 0 && eps[1].cached);
    assert(strcmp(eps[1].episode_url, "http://fixture/Gone.mp3") == 0);
    assert(strcmp(eps[1].cache_path, FEED_DIR "/Gone.mp3") == 0);
    assert(strcmp(eps[1].date, "Mon, 01 Jan 2026 00:00:00 GMT") == 0);
    assert(eps[1].duration_seconds == 42);
    free(eps);
    assert(strstr(manifest_text(), "\"retained\":true"));

    // Never downloaded (or deleted since): it goes with the feed.
    refresh_setup();
    old_manifest(OLD_EP("Gone"));
    assert(test_remove(FEED_DIR "/Gone.mp3") == 0);
    refresh_ok();
    n = read_eps(&eps);
    assert(n == 1);
    free(eps);
    assert(!strstr(manifest_text(), "Gone"));

    // Still listed by the feed: one row, no duplicate, no marker.
    refresh_setup();
    old_manifest(OLD_EP("Episode"));
    put(FEED_DIR "/Episode.mp3", "x", 1);
    refresh_ok();
    n = read_eps(&eps);
    assert(n == 1 && eps[0].cached);
    free(eps);
    assert(!strstr(manifest_text(), "retained"));
    assert(test_remove(FEED_DIR "/Episode.mp3") == 0);

    // No card: presence cannot be checked, so only an entry a previous refresh
    // already marked survives. One refresh with the card pulled out must not
    // cost the kept episodes, and a device that never had a card never
    // accumulates dead rows.
    refresh_setup();
    old_manifest(OLD_EP_RETAINED("Kept") "," OLD_EP("Fresh"));
    put(FEED_DIR "/Kept.mp3", "x", 1);
    put(FEED_DIR "/Fresh.mp3", "x", 1);
    sd_absent = true;
    refresh_ok();
    n = read_eps(&eps);
    assert(n == 2 && strcmp(eps[1].title, "Kept") == 0);
    free(eps);
    assert(!strstr(manifest_text(), "Fresh"));

    // Idempotent: a second refresh keeps the entry and does not stack markers.
    refresh_setup();
    old_manifest(OLD_EP_RETAINED("Kept"));
    refresh_ok();
    const char *first = strstr(manifest_text(), "\"retained\":true");
    assert(first && !strstr(first + strlen("\"retained\":true"), "retained"));
    n = read_eps(&eps);
    assert(n == 2 && strcmp(eps[1].title, "Kept") == 0 && eps[1].cached);
    free(eps);
    assert(test_remove(FEED_DIR "/Kept.mp3") == 0);
    assert(test_remove(FEED_DIR "/Fresh.mp3") == 0);

    // Best effort, three ways: reset() leaves an unusable old manifest, then the
    // dedup table and the retention scratch fail to allocate (they are the 1st
    // and the 3rd PSRAM allocation of a refresh, the HTTP chunk is the 2nd).
    // Each costs the carry-over and nothing else.
    refresh_setup();
    refresh_ok();
    n = read_eps(&eps);
    assert(n == 1);
    free(eps);

    put(FEED_DIR "/Gone.mp3", "x", 1);
    for (int nth = 1; nth <= 3; nth += 2) {
        refresh_setup();
        old_manifest(OLD_EP("Gone"));
        lot23_alloc_fail_nth = nth;
        refresh_ok();
        assert(lot23_alloc_calls > nth);
        n = read_eps(&eps);
        assert(n == 1);
        free(eps);
    }

    // A worst-case entry (longest title, longest URL) grows by the marker on its
    // first carry-over and must still fit PODCAST_OBJ_MAX when the NEXT refresh
    // reads it back: an object over the limit makes manifest_next_object return
    // -1, which would silently end the pass and drop every later entry too.
    static char big[PODCAST_OBJ_MAX * 2];
    char longt[PODCAST_TITLE_MAX], longu[PODCAST_URL_MAX];
    memset(longt, 'T', sizeof(longt) - 1); longt[sizeof(longt) - 1] = '\0';
    memset(longu, 'u', sizeof(longu) - 1); longu[sizeof(longu) - 1] = '\0';
    snprintf(big, sizeof(big),
             "{\"title\":\"%s\",\"date\":\"Mon, 01 Jan 2026 00:00:00 GMT\","
             "\"duration_seconds\":42,\"episode_url\":\"http://fixture/%s\","
             "\"cache_path\":\"" FEED_DIR "/%s.mp3\",\"cached\":false},"
             OLD_EP("After"), longt, longu, longt);
    char longp[PODCAST_PATH_MAX];
    snprintf(longp, sizeof(longp), FEED_DIR "/%s.mp3", longt);
    put(longp, "x", 1);
    put(FEED_DIR "/After.mp3", "x", 1);
    static char carried[sizeof(mbuf)];
    for (int pass = 0; pass < 2; pass++) {
        refresh_setup();  // it puts the "old-manifest" garbage back, so write after it
        if (pass == 0) old_manifest(big);
        else put("/littlefs/podcasts/7.json", carried, strlen(carried));
        refresh_ok();
        n = read_eps(&eps);
        assert(n == 3);
        assert(strcmp(eps[1].title, longt) == 0 && eps[1].cached);
        assert(strcmp(eps[2].title, "After") == 0 && eps[2].cached);
        free(eps);
        strcpy(carried, manifest_text());  // the marked entry feeds the next refresh
    }
    assert(test_remove(longp) == 0);
    assert(test_remove(FEED_DIR "/After.mp3") == 0);

    // The safety cap still holds: the feed's episodes first, carry-over fills
    // whatever is left.
    static char many[PODCAST_MAX_EPISODES * 220];
    size_t len = 0;
    for (int i = 0; i < PODCAST_MAX_EPISODES; i++) {
        char name[16], path[80];
        snprintf(name, sizeof(name), "Old%03d", i);
        snprintf(path, sizeof(path), FEED_DIR "/%s.mp3", name);
        put(path, "x", 1);
        int w = snprintf(many + len, sizeof(many) - len,
                         "%s{\"title\":\"%s\",\"date\":\"D\",\"duration_seconds\":1,"
                         "\"episode_url\":\"http://fixture/%s.mp3\","
                         "\"cache_path\":\"%s\",\"cached\":false}",
                         i ? "," : "", name, name, path);
        assert(w > 0 && (len += (size_t)w) < sizeof(many));
    }
    refresh_setup();
    old_manifest(many);
    refresh_ok();
    n = read_eps(&eps);
    assert(n == PODCAST_MAX_EPISODES);
    assert(strcmp(eps[0].title, "Episode") == 0);
    assert(strcmp(eps[1].title, "Old000") == 0);
    assert(strcmp(eps[PODCAST_MAX_EPISODES - 1].title, "Old298") == 0);
    free(eps);
}

static void clean_tree(const char *path)
{
    DIR *d = opendir(path);
    assert(d);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1024];
        assert(snprintf(p, sizeof(p), "%s/%s", path, e->d_name) < (int)sizeof(p));
        struct stat st;
        assert(lstat(p, &st) == 0);
        if (S_ISDIR(st.st_mode)) clean_tree(p);
        else assert(unlink(p) == 0);
    }
    assert(closedir(d) == 0);
    assert(rmdir(path) == 0);
}

int main(void)
{
    strcpy(root, "/tmp/opencode/podcast-lot23-XXXXXX");
    assert(mkdtemp(root));
    assert(test_mkdir("/sdcard", 0700) == 0);
    assert(test_mkdir("/sdcard/podcasts", 0700) == 0);
    assert(test_mkdir("/sdcard/podcasts/Feed", 0700) == 0);
    assert(test_mkdir("/littlefs", 0700) == 0);
    assert(test_mkdir("/littlefs/podcasts", 0700) == 0);
    for (size_t i = 0; i < sizeof(audio); i += 417) {
        audio[i] = 0xff; audio[i + 1] = 0xfb; audio[i + 2] = 0x90;
    }
    test_trim_failures();
    test_trim_success();
    test_refresh();
    test_cache_scan();
    test_retention();
    reset();
    clean_tree(root);
    puts("podcast_lot23: storage faults, trim, cache scan, refresh cancellation and retention passed");
    return 0;
}
