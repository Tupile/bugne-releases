// stats: see stats.h for the design. This file keeps the pure accumulation
// logic (host-testable, top of file) separate from the device-only persistence
// (behind ESP_PLATFORM, bottom of file).
#include "stats.h"

#include <stdio.h>
#include <string.h>

// ---- pure accumulation (host-testable) ----

static stats_day_t s_days[STATS_MAX_DAYS];
static int s_count;             // valid days; s_days[s_count-1] is the current day
static int s_ticks_since_save;  // ticks accumulated since the last flush

#define STATS_SAVE_INTERVAL 300  // ticks (~1 Hz) between periodic saves: 5 min

// Add one second to the title map of `d`, evicting the smallest entry when full.
static void add_title(stats_day_t *d, const char *title)
{
    if (!title || !title[0]) return;  // no title (e.g. a radio with no ICY): count seconds only
    for (int i = 0; i < d->top_count; i++) {
        // Stored titles are already truncated to STATS_TITLE_LEN-1; comparing
        // that many chars treats a longer incoming title as its truncation.
        if (strncmp(d->top[i].title, title, STATS_TITLE_LEN - 1) == 0) {
            d->top[i].seconds++;
            return;
        }
    }
    int slot;
    if (d->top_count < STATS_MAX_TITLES) {
        slot = d->top_count++;
    } else {
        slot = 0;  // full: replace the least-listened title
        for (int i = 1; i < STATS_MAX_TITLES; i++) {
            if (d->top[i].seconds < d->top[slot].seconds) slot = i;
        }
    }
    strncpy(d->top[slot].title, title, STATS_TITLE_LEN - 1);
    d->top[slot].title[STATS_TITLE_LEN - 1] = '\0';
    d->top[slot].seconds = 1;
}

bool stats_tick(int date, stats_source_t src, const char *title)
{
    if (date <= 0) return false;  // no valid clock: drop the tick (no fake dates)

    bool rolled = false;
    if (s_count == 0 || s_days[s_count - 1].date != date) {
        if (s_count == STATS_MAX_DAYS) {  // ring full: drop the oldest day
            memmove(&s_days[0], &s_days[1], sizeof(s_days[0]) * (STATS_MAX_DAYS - 1));
            s_count--;
        }
        memset(&s_days[s_count], 0, sizeof(s_days[s_count]));
        s_days[s_count].date = date;
        s_count++;
        rolled = true;
    }

    stats_day_t *d = &s_days[s_count - 1];
    if (src >= 0 && src < STATS_SRC_COUNT) d->src_s[src]++;
    add_title(d, title);

    if (++s_ticks_since_save >= STATS_SAVE_INTERVAL) {
        s_ticks_since_save = 0;
        return true;
    }
    return rolled;  // persist a finished day promptly
}

int stats_day_count(void) { return s_count; }

const stats_day_t *stats_day_at(int i)
{
    if (i < 0 || i >= s_count) return NULL;
    return &s_days[i];
}

void stats_clear(void)
{
    memset(s_days, 0, sizeof(s_days));
    s_count = 0;
    s_ticks_since_save = 0;
}

// ---- persistence (device only) ----
// cJSON cannot be stubbed cleanly for the host build, so the whole persistence
// section is compiled out on the host. The host test exercises only the pure
// logic above (see the deviation note in stats.h).
#ifdef ESP_PLATFORM

#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "stats";

#define STATS_FILE "/littlefs/stats.json"
#define STATS_TMP  "/littlefs/stats.json.tmp"

// Set once at the end of stats_init(); guards flush/reset so an early save
// cannot clobber the file before it is loaded (same convention as played.c).
static volatile bool s_ready;

// Write a JSON string literal for `s`, escaping the characters JSON forbids.
// Titles come from RSS/tags (untrusted), so this must stay strict.
static void write_json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\r') fputs("\\r", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c < 0x20)  fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

// Serialize by hand straight to the file: no big transient JSON tree, so a save
// during HTTPS streaming (5-minute mark) does not spike internal RAM.
void stats_flush(void)
{
    if (!s_ready) return;
    FILE *f = fopen(STATS_TMP, "w");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s for write", STATS_TMP);
        return;
    }
    fputc('[', f);
    for (int i = 0; i < s_count; i++) {
        const stats_day_t *d = &s_days[i];
        if (i) fputc(',', f);
        fprintf(f, "{\"d\":%d,\"r\":%d,\"p\":%d,\"s\":%d,\"m\":%d,\"top\":[",
                d->date, d->src_s[STATS_SRC_RADIO], d->src_s[STATS_SRC_PODCAST],
                d->src_s[STATS_SRC_SD], d->src_s[STATS_SRC_SENDSPIN]);
        for (int t = 0; t < d->top_count; t++) {
            if (t) fputc(',', f);
            fputs("{\"t\":", f);
            write_json_string(f, d->top[t].title);
            fprintf(f, ",\"s\":%d}", d->top[t].seconds);
        }
        fputs("]}", f);
    }
    fputc(']', f);
    long size = ftell(f);
    bool ok = (fclose(f) == 0);
    if (!ok || size < 0) {
        ESP_LOGE(TAG, "short write to %s", STATS_TMP);
        remove(STATS_TMP);
        return;
    }
    remove(STATS_FILE);
    if (rename(STATS_TMP, STATS_FILE) != 0) {
        ESP_LOGE(TAG, "rename to %s failed", STATS_FILE);
        remove(STATS_TMP);
        return;
    }
    ESP_LOGI(TAG, "saved stats: %d day(s), %ld bytes", s_count, size);
}

static int json_int(const cJSON *o, const char *key)
{
    const cJSON *x = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(x) ? x->valueint : 0;
}

void stats_init(void)
{
    FILE *f = fopen(STATS_FILE, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 128 * 1024) {
            char *buf = malloc((size_t)sz + 1);
            if (buf) {
                size_t rd = fread(buf, 1, (size_t)sz, f);
                buf[rd] = '\0';
                cJSON *arr = cJSON_Parse(buf);
                if (cJSON_IsArray(arr)) {
                    int n = cJSON_GetArraySize(arr);
                    // No reliable clock at load time: keep the newest 30 entries
                    // (the file is written newest-last), which is the ring rule.
                    int start = (n > STATS_MAX_DAYS) ? (n - STATS_MAX_DAYS) : 0;
                    for (int i = start; i < n && s_count < STATS_MAX_DAYS; i++) {
                        const cJSON *o = cJSON_GetArrayItem(arr, i);
                        if (!cJSON_IsObject(o)) continue;
                        stats_day_t *d = &s_days[s_count];
                        memset(d, 0, sizeof(*d));
                        d->date = json_int(o, "d");
                        if (d->date <= 0) continue;
                        d->src_s[STATS_SRC_RADIO]    = json_int(o, "r");
                        d->src_s[STATS_SRC_PODCAST]  = json_int(o, "p");
                        d->src_s[STATS_SRC_SD]       = json_int(o, "s");
                        d->src_s[STATS_SRC_SENDSPIN] = json_int(o, "m");
                        const cJSON *top = cJSON_GetObjectItem(o, "top");
                        if (cJSON_IsArray(top)) {
                            int tn = cJSON_GetArraySize(top);
                            for (int t = 0; t < tn && d->top_count < STATS_MAX_TITLES; t++) {
                                const cJSON *e = cJSON_GetArrayItem(top, t);
                                const cJSON *ts = cJSON_GetObjectItem(e, "t");
                                if (!cJSON_IsString(ts) || !ts->valuestring) continue;
                                strncpy(d->top[d->top_count].title, ts->valuestring, STATS_TITLE_LEN - 1);
                                d->top[d->top_count].title[STATS_TITLE_LEN - 1] = '\0';
                                d->top[d->top_count].seconds = json_int(e, "s");
                                d->top_count++;
                            }
                        }
                        s_count++;
                    }
                }
                cJSON_Delete(arr);
                free(buf);
            }
        }
        fclose(f);
    }
    s_ready = true;
    ESP_LOGI(TAG, "loaded stats: %d day(s)", s_count);
}

void stats_reset(void)
{
    stats_clear();
    remove(STATS_FILE);
    remove(STATS_TMP);
    ESP_LOGI(TAG, "stats reset");
}

#endif  // ESP_PLATFORM
