from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(text, name):
    match = re.search(r"^.*\b" + name + r"\([^;]*?\)\n\{", text, re.M)
    assert match, name
    opening = text.index("{", match.end() - 1)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end]


sd = (ROOT / "components/source_sd/source_sd.c").read_text()
stream = (ROOT / "components/source_stream/source_stream.c").read_text()
prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_NOT_SUPPORTED 3
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define MALLOC_CAP_SPIRAM 0
#define AUDIO_SOURCE_SD 1
#define SD_IO_BUF_BYTES 1024
#define taskENTER_CRITICAL(m) do { assert(!locked); locked = true; } while (0)
#define taskEXIT_CRITICAL(m) do { assert(locked); locked = false; } while (0)
typedef int esp_err_t;
typedef enum { DECODE_FORMAT_MP3, DECODE_FORMAT_FLAC, DECODE_FORMAT_AAC, DECODE_FORMAT_OGG } decode_format_t;
typedef struct {
    size_t (*read)(void *, void *, size_t);
    bool (*seek)(void *, int, int);
    bool (*tell)(void *, int64_t *);
    void *ctx;
    int64_t total_bytes;
} decode_source_t;
static bool locked, s_present = true, s_completed;
static volatile bool s_stop;
static uint32_t s_generation, s_completed_generation, s_rf_gen;
static int hook, decodes, acquired, released;
static esp_err_t decode_result, acquire_result;
static FILE *file;
static void source_sd_stop(void);
static FILE *open_file(const char *p, const char *mode) {
    assert(!locked); (void)mode;
    if (!strcmp(p, "missing.mp3")) return NULL;
    if (hook == 1) source_sd_stop();
    rewind(file); return file;
}
static int close_file(FILE *f) { assert(!locked && f == file); if (hook == 4) source_sd_stop(); return 0; }
#define fopen open_file
#define fclose close_file
static void *heap_caps_malloc(size_t n, int caps) { assert(!locked); (void)caps; return malloc(n); }
static int buffer_file(FILE *f, char *b, int mode, size_t size) { (void)f; (void)b; (void)mode; (void)size; return 0; }
#define setvbuf buffer_file
static esp_err_t audio_arbiter_acquire(int source) {
    assert(!locked); (void)source; acquired++;
    if (hook == 2) source_sd_stop();
    return acquire_result;
}
static void audio_arbiter_release(int source) { assert(!locked); (void)source; released++; }
static esp_err_t decode_run(decode_format_t fmt, decode_source_t *src) {
    assert(!locked); (void)fmt; decodes++;
    if (hook == 3) { source_sd_stop(); char buf[1]; assert(src->read(src->ctx, buf, 1) == 0); }
    return decode_result;
}
esp_err_t source_sd_play_generation(const char *path, uint32_t generation);
'''
body = "\n".join(function(sd, name) for name in (
    "source_sd_generation", "source_sd_stop", "source_sd_completed", "file_read", "file_seek",
    "file_tell", "format_from_path", "source_sd_play", "source_sd_play_generation",
))
startup = function(stream, "source_stream_play_generation")
startup = startup[:startup.index('    icy_set_title("");')] + "    (void)rf_generation;\n    return ESP_OK;\n}"
body += "\n" + "\n".join(function(stream, name) for name in (
    "source_stream_generation", "source_stream_stop", "source_stream_completed",
)) + "\n" + startup
cases = r'''
static void reset(void) {
    source_sd_stop(); hook = decodes = acquired = released = 0;
    decode_result = acquire_result = ESP_OK; s_present = true; s_completed = true;
    s_completed_generation = s_generation;
}
int main(void) {
    file = tmpfile(); assert(file);
    reset(); uint32_t queued = source_sd_generation(); source_sd_stop();
    assert(source_sd_play_generation("file.mp3", queued) == ESP_ERR_INVALID_STATE);
    assert(!decodes && !source_sd_completed());
    for (int h = 1; h <= 4; h++) {
        reset(); hook = h;
        source_sd_play_generation("file.mp3", source_sd_generation());
        assert(!source_sd_completed() && acquired == released);
        assert(decodes == (h >= 3));
    }
    reset(); assert(source_sd_play("file.mp3") == ESP_OK && source_sd_completed());
    assert(source_sd_play(NULL) == ESP_ERR_INVALID_ARG && !source_sd_completed());
    reset(); assert(source_sd_play("missing.mp3") == ESP_FAIL && !source_sd_completed());
    reset(); assert(source_sd_play("file.txt") == ESP_ERR_NOT_SUPPORTED && !source_sd_completed());
    reset(); s_present = false;
    assert(source_sd_play("file.mp3") == ESP_ERR_INVALID_STATE && !source_sd_completed());
    reset(); acquire_result = ESP_FAIL;
    assert(source_sd_play("file.mp3") == ESP_FAIL && !source_sd_completed());
    reset(); decode_result = ESP_FAIL;
    assert(source_sd_play("file.mp3") == ESP_FAIL && !source_sd_completed());
    reset(); queued = source_stream_generation(); source_stream_stop();
    assert(source_stream_play_generation("url", queued) == ESP_ERR_INVALID_STATE && !source_stream_completed());
    queued = source_stream_generation();
    assert(source_stream_play_generation("url", queued) == ESP_OK && !s_stop);
    s_stop = true;
    assert(source_stream_play_generation("url", queued) == ESP_OK && !s_stop);
    source_stream_stop();
    assert(source_stream_play_generation("url", queued) == ESP_ERR_INVALID_STATE && s_stop);
    assert(source_stream_play_generation(NULL, source_stream_generation()) == ESP_ERR_INVALID_ARG);
    puts("actual SD playback and stream startup generation guards: passed");
}
'''
with tempfile.TemporaryDirectory(prefix="bugne-source-startup-") as directory:
    source = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    source.write_text(prelude + body + cases)
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
