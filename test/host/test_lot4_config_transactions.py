from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
STORE = (ROOT / 'components/config_store/config_store.c').read_text()
WEB = (ROOT / 'components/web_config/web_config.c').read_text()
# Registry cJSON (ESP-IDF 6 removed its json component), fetched by idf.py reconfigure.
CJSON = ROOT / 'managed_components/espressif__cjson/cJSON'


def function(text, name):
    match = re.search(r'^.*\b' + name + r'\([^;]*?\)\n\{', text, re.M)
    assert match, name
    end = text.index('{', match.end() - 1)
    depth = 1
    while depth:
        end += 1
        depth += (text[end] == '{') - (text[end] == '}')
    return text[match.start():end + 1]


prelude = r'''
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "config_store.h"
#include "cJSON.h"
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_RETURN_ON_ERROR(expr, ...) do { int e = (expr); if (e) return e; } while (0)
#define MALLOC_CAP_SPIRAM 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(n) (n)
#define pdTRUE 1
#define NVS_NAMESPACE "bugne"
#define NVS_KEY_HA_TOKN "ha_token"
#define NVS_READWRITE 1
#define WEB_MAX_BODY 32768
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_401_UNAUTHORIZED 401
#define HTTPD_413_CONTENT_TOO_LARGE 413
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
static config_t live, disk, before;
static config_t *s_cfg = &live;
#define s_config (*s_cfg)
static bool s_ready = true;
static int s_lock = 1, held, alloc_fail, save_error, saves;
static int xSemaphoreTake(int lock, uint32_t ticks) {
    assert(lock);
    if (held) { assert(ticks != portMAX_DELAY); return 0; }
    held = 1;
    return pdTRUE;
}
static void xSemaphoreGive(int lock) { assert(lock && held); held = 0; }
static void *heap_caps_malloc(size_t size, int caps) {
    assert(caps == MALLOC_CAP_SPIRAM);
    return alloc_fail ? NULL : malloc(size);
}
static void *heap_caps_calloc(size_t n, size_t size, int caps) {
    assert(caps == MALLOC_CAP_SPIRAM);
    return alloc_fail ? NULL : calloc(n, size);
}
static int real_serializer;
static esp_err_t checked_save(const config_t *c);
static esp_err_t save_to_disk(const config_t *c) {
    if (real_serializer) return checked_save(c);
    assert(held);
    assert(memcmp(&live, &before, sizeof(live)) == 0);
    saves++;
    if (!save_error) disk = *c;
    return save_error;
}
typedef int nvs_handle_t;
static int nvs_open_error, nvs_set_error, nvs_commit_error, nvs_calls, nvs_commits;
static char token_pending[CFG_HA_TOKEN_MAX], token_saved[CFG_HA_TOKEN_MAX];
static int nvs_open(const char *ns, int mode, nvs_handle_t *h) {
    (void)ns; (void)mode; *h = 1; nvs_calls++; return nvs_open_error;
}
static int nvs_set_str(nvs_handle_t h, const char *key, const char *value) {
    (void)h; (void)key;
    if (!nvs_set_error) strcpy(token_pending, value);
    return nvs_set_error;
}
static int nvs_erase_key(nvs_handle_t h, const char *key) {
    (void)h; (void)key;
    token_pending[0] = 0;
    return nvs_set_error ? nvs_set_error : (token_saved[0] ? ESP_OK : ESP_ERR_NVS_NOT_FOUND);
}
static int nvs_commit(nvs_handle_t h) {
    (void)h; nvs_commits++;
    if (!nvs_commit_error) strcpy(token_saved, token_pending);
    return nvs_commit_error;
}
static void nvs_close(nvs_handle_t h) { (void)h; }
typedef struct { size_t content_len, consumed; const char *body; bool authed, recv_fail; int status; } httpd_req_t;
static int httpd_resp_send_err(httpd_req_t *r, int status, const char *msg) {
    (void)msg; r->status = status; return ESP_OK;
}
static int httpd_resp_sendstr(httpd_req_t *r, const char *msg) {
    (void)msg; r->status = 200; return ESP_OK;
}
static int httpd_req_recv(httpd_req_t *r, char *out, size_t len) {
    if (r->recv_fail && r->consumed) return -1;
    if (len > 3) len = 3;
    memcpy(out, r->body + r->consumed, len); r->consumed += len; return (int)len;
}
static int httpd_req_to_sockfd(httpd_req_t *r) { (void)r; return -1; }
#define REQUIRE_AUTH(req, rv) do { if (!(req)->authed) { (req)->status = 401; return rv; } } while (0)
'''

names = ('clampi', 'set_defaults', 'parse_one_alarm', 'dedup_ids', 'load_from_json',
         'config_store_get', 'candidate_begin', 'candidate_finish',
         'config_store_set_lang', 'config_store_set_orientation', 'config_store_set_theme',
         'config_store_set_alarm', 'config_store_favorite_add', 'config_store_favorite_remove',
         'config_store_write_json', 'config_store_set_ha_token')
serializer = r'''
#define CONFIG_PATH "config.json"
#define CONFIG_TMP_PATH "config.tmp"
#define CONFIG_FILE_MAX 32768
static size_t json_calls, json_fail_at, json_live, array_calls, array_fail_at;
static int file_opens, promotions;
static void *json_malloc(size_t size) {
    if (++json_calls == json_fail_at) return NULL;
    void *p = malloc(size);
    if (p) json_live++;
    return p;
}
static void json_free(void *p) {
    if (p) { assert(json_live); json_live--; free(p); }
}
static cJSON_bool add_array(cJSON *array, cJSON *item) {
    if (++array_calls == array_fail_at) return false;
    return cJSON_AddItemToArray(array, item);
}
static FILE *save_fopen(const char *path, const char *mode) {
    file_opens++;
    return fopen(path, mode);
}
static int save_rename(const char *old, const char *new) {
    assert(!memcmp(&live, &before, sizeof(live)));
    promotions++;
    return rename(old, new);
}
#define cJSON_AddItemToArray add_array
#define fopen save_fopen
#define rename save_rename
'''
serializer += function(STORE, 'save_to_disk').replace('save_to_disk(', 'real_save(')
serializer += r'''
#undef cJSON_AddItemToArray
#undef fopen
#undef rename
static esp_err_t checked_save(const config_t *c) {
    assert(held && !json_live);
    assert(!memcmp(&live, &before, sizeof(live)));
    json_calls = array_calls = 0;
    cJSON_Hooks hooks = {json_malloc, json_free};
    cJSON_InitHooks(&hooks);
    esp_err_t err = real_save(c);
    assert(!json_live);
    cJSON_InitHooks(NULL);
    return err;
}
'''
body = serializer + '\n'.join(function(STORE, name) for name in names)
body += '\n' + '\n'.join(function(WEB, name) for name in ('read_body', 'ha_token_post', 'config_post'))
cases = r'''
static void reset(void) {
    set_defaults(&live);
    live.favorite_count = 2;
    live.favorites[0].radio_id = 2;
    live.favorites[1].radio_id = 3;
    before = disk = live;
    save_error = saves = alloc_fail = held = 0;
}
static int change(int which) {
    config_alarm_t a = {.enabled = 1, .hour = 25, .minute = 62, .volume = 105};
    config_favorite_t f = {.type = 0, .radio_id = 4};
    switch (which) {
    case 0: return config_store_set_lang("fr");
    case 1: return config_store_set_orientation(1);
    case 2: return config_store_set_theme(0, 4);
    case 3: return config_store_set_alarm(0, &a);
    case 4: return config_store_favorite_add(&f);
    case 5: return config_store_favorite_remove(0);
    default: return config_store_write_json("{\"schema_version\":1,\"device\":{\"name\":\"new\"}}");
    }
}
static void seed_file(void) {
    reset();
    live.webradio_count = 1;
    live.webradios[0] = (config_webradio_t){.id = 2, .name = "Radio", .url = "https://example.com/radio", .skip_preroll = 1};
    before = live;
    json_fail_at = array_fail_at = 0;
    assert(real_save(&live) == ESP_OK);
    file_opens = promotions = 0;
}
static void read_file(char *out) {
    FILE *f = fopen(CONFIG_PATH, "r");
    assert(f);
    size_t n = fread(out, 1, CONFIG_FILE_MAX, f);
    assert(n < CONFIG_FILE_MAX && !ferror(f));
    out[n] = 0;
    assert(!fclose(f));
}
static void check_roundtrip(void) {
    char text[CONFIG_FILE_MAX];
    read_file(text);
    cJSON *root = cJSON_Parse(text);
    assert(root);
    config_t parsed, expected = live;
    set_defaults(&parsed);
    load_from_json(&parsed, root);
    cJSON_Delete(root);
    memset(expected.favorites + expected.favorite_count, 0,
           (CFG_MAX_FAVORITES - expected.favorite_count) * sizeof(expected.favorites[0]));
    assert(!memcmp(&parsed, &expected, sizeof(parsed)));
}
static void serializer_failures(void) {
    char original[CONFIG_FILE_MAX], after[CONFIG_FILE_MAX];
    real_serializer = 1;
    for (int which = 0; which < 7; which++) {
        seed_file();
        assert(change(which) == ESP_OK);
        size_t allocations = json_calls, inserts = array_calls;
        assert(allocations > 100 && inserts >= CFG_MAX_ALARMS + CFG_QUIET_WINDOWS);
        assert(!held && file_opens == 1 && promotions == 1);
        check_roundtrip();
        for (int mode = 0; mode < 2; mode++) {
            size_t count = mode ? inserts : allocations;
            for (size_t fail = 1; fail <= count; fail++) {
                seed_file();
                read_file(original);
                if (mode) array_fail_at = fail;
                else json_fail_at = fail;
                assert(change(which) == ESP_ERR_NO_MEM);
                assert(!held && !json_live && !file_opens && !promotions);
                assert(!memcmp(&live, &before, sizeof(live)));
                read_file(after);
                assert(!strcmp(original, after));
                FILE *tmp = fopen(CONFIG_TMP_PATH, "r");
                assert(!tmp);
                json_fail_at = array_fail_at = 0;
                assert(change(which) == ESP_OK);
                assert(!held && file_opens == 1 && promotions == 1);
                check_roundtrip();
            }
        }
        printf("real serializer transaction %d: %zu allocation and %zu insertion failures passed\n",
               which, allocations, inserts);
    }
    real_serializer = 0;
    assert(!remove(CONFIG_PATH));
}
static httpd_req_t request(const char *s) {
    return (httpd_req_t){.body = s, .content_len = strlen(s), .authed = true};
}
int main(void) {
    const config_t *borrowed = config_store_get();
    for (int i = 0; i < 7; i++) {
        for (int e = 0; e < 3; e++) {
            reset();
            save_error = e == 0 ? ESP_FAIL : e == 1 ? ESP_ERR_INVALID_SIZE : ESP_ERR_NO_MEM;
            assert(change(i) == save_error);
            assert(!held && saves == 1);
            assert(!memcmp(&live, &before, sizeof(live)));
            assert(!memcmp(&disk, &before, sizeof(live)));
        }
        reset(); alloc_fail = 1;
        assert(change(i) == ESP_ERR_NO_MEM);
        assert(!held && !saves && !memcmp(&live, &before, sizeof(live)));
        reset();
        assert(change(i) == ESP_OK);
        assert(!held && saves == 1);
        assert(memcmp(&live, &before, sizeof(live)));
        assert(!memcmp(&live, &disk, sizeof(live)));
        assert(config_store_get() == borrowed);
    }
    reset();
    assert(config_store_set_orientation(1) == ESP_OK);
    before = live;
    assert(config_store_set_theme(0, 4) == ESP_OK);
    assert(live.ui.orientation == 1 && live.ui.dark == 0 && live.ui.accent == 4);
    s_ready = false;
    assert(config_store_set_orientation(0) == ESP_ERR_INVALID_STATE);
    s_ready = true;
    reset();
    assert(config_store_favorite_remove(-1) == ESP_ERR_INVALID_ARG && !held);
    assert(config_store_set_alarm(-1, &live.alarms[0]) == ESP_ERR_INVALID_ARG);
    live.favorite_count = CFG_MAX_FAVORITES;
    assert(config_store_favorite_add(&live.favorites[0]) == ESP_ERR_NO_MEM && !held);
    reset();
    const char *legacy = "{\"schema_version\":1,\"ha_token\":\"test\"}";
    assert(config_store_write_json(legacy) == ESP_ERR_INVALID_ARG && !saves);
    httpd_req_t r = request(legacy);
    assert(config_post(&r) == ESP_OK && r.status == 400 && r.consumed == r.content_len);
    assert(!nvs_calls && !saves);
    r = request("test-token"); r.authed = false;
    assert(ha_token_post(&r) == ESP_FAIL && r.status == 401 && !r.consumed);
    r = request("test-token"); r.content_len = CFG_HA_TOKEN_MAX;
    assert(ha_token_post(&r) == ESP_FAIL && r.status == 413 && !r.consumed);
    r = request("test-token"); r.recv_fail = true;
    assert(ha_token_post(&r) == ESP_FAIL && r.status == 400 && r.consumed < r.content_len);
    r = request("bad\ntoken");
    assert(ha_token_post(&r) == ESP_OK && r.status == 400 && !nvs_calls);
    r = request("abc"); r.body = "a\0b";
    assert(ha_token_post(&r) == ESP_OK && r.status == 400 && !nvs_calls);
    for (int i = 0; i < 3; i++) {
        nvs_open_error = i == 0 ? ESP_FAIL : ESP_OK;
        nvs_set_error = i == 1 ? ESP_FAIL : ESP_OK;
        nvs_commit_error = i == 2 ? ESP_FAIL : ESP_OK;
        r = request("test-token");
        assert(ha_token_post(&r) == ESP_OK && r.status == 500);
        assert(r.consumed == r.content_len && !token_saved[0]);
    }
    nvs_open_error = nvs_set_error = nvs_commit_error = 0;
    char max[CFG_HA_TOKEN_MAX]; memset(max, 'x', sizeof(max) - 1); max[sizeof(max) - 1] = 0;
    r = request(max);
    assert(ha_token_post(&r) == ESP_OK && r.status == 200);
    assert(!strcmp(token_saved, max));
    r = request("");
    assert(ha_token_post(&r) == ESP_OK && r.status == 200 && !token_saved[0]);
    r = request("");
    assert(ha_token_post(&r) == ESP_OK && r.status == 200);
    assert(!saves && !memcmp(&live, &before, sizeof(live)));
    serializer_failures();
    puts("lot4 config: candidate failures/success, stable pointer, token auth/bounds/NVS/legacy rejection passed");
}
'''

with tempfile.TemporaryDirectory(prefix='lot4-', dir='/tmp/opencode') as tmp:
    src = Path(tmp) / 'lot4.c'
    exe = Path(tmp) / 'lot4'
    src.write_text(prelude + body + cases)
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(ROOT / 'test/host/stubs'),
                    '-I', str(ROOT / 'components/config_store/include'), '-I', str(CJSON),
                    str(src), str(CJSON / 'cJSON.c'), '-lm', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, cwd=tmp)
