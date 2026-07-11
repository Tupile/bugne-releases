// stats: local listening statistics for parents.
//
// Accumulates seconds of real listening per day, split by source, plus a small
// per-day map of the most-listened titles. The current day and the last 30 days
// live in RAM and are persisted to /littlefs/stats.json (a JSON array of day
// objects), streamed as-is by GET /api/stats. Local only, resettable.
//
// The accumulate / rollover / evict logic (stats_tick, stats_clear and the
// accessors) is pure C with no ESP or FreeRTOS dependency, so it is host-tested
// in test/host/test_stats.c. The persistence (stats_init / stats_flush /
// stats_reset) is device only, compiled behind ESP_PLATFORM.
//
// Thread safety: stats_tick / stats_flush / stats_reset are called only from
// the UI (LVGL) task; stats_init runs once on bg_init_task at boot before the
// UI can accumulate anything. GET /api/stats reads the file from the httpd
// task, which is safe: the file is written atomically (temp + rename).
#pragma once

#include <stdbool.h>

#define STATS_MAX_DAYS    30   // days kept in RAM and in the file (ring)
#define STATS_MAX_TITLES  16   // top-title slots per day
#define STATS_TITLE_LEN   64   // stored title length incl. the NUL

// Source classes, matching what the web chart stacks. Kept in this fixed order:
// the persisted "r/p/s/m" keys and the host tests rely on it.
typedef enum {
    STATS_SRC_RADIO = 0,
    STATS_SRC_PODCAST,
    STATS_SRC_SD,
    STATS_SRC_SENDSPIN,
    STATS_SRC_COUNT,
} stats_source_t;

typedef struct {
    char title[STATS_TITLE_LEN];
    int  seconds;
} stats_title_t;

typedef struct {
    int date;                      // yyyymmdd, 0 = unused slot
    int src_s[STATS_SRC_COUNT];    // seconds listened per source
    stats_title_t top[STATS_MAX_TITLES];
    int top_count;
} stats_day_t;

// --- pure accumulation (host-testable, no ESP deps) ---

// Accumulate one second of listening for `src` and `title` on `date`
// (yyyymmdd). A date <= 0 (no valid clock yet) drops the tick. A new date rolls
// a fresh current day onto the ring, dropping the oldest when full. Returns
// true when the caller should persist (a day rollover, or the periodic save
// interval elapsed): the UI tick then calls stats_flush().
bool stats_tick(int date, stats_source_t src, const char *title);

// Read back the in-RAM history, oldest first, current day last. For tests and
// for the device serializer. i in [0, stats_day_count()).
int  stats_day_count(void);
const stats_day_t *stats_day_at(int i);

// Drop all in-RAM state. Pure (no file I/O); stats_reset() adds the file delete.
void stats_clear(void);

// --- persistence (device only) ---

// Load /littlefs/stats.json into RAM. Call once after LittleFS is mounted.
void stats_init(void);

// Serialize the in-RAM history to /littlefs/stats.json (atomic temp + rename).
void stats_flush(void);

// Clear RAM and remove the file (parental reset). UI task only.
void stats_reset(void);
