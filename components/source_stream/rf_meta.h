// rf_meta: pure helpers for Radio France livemeta now-playing metadata.
// No ESP-IDF dependencies so the logic is host-testable (test/host).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Map a resolved stream URL to a Radio France livemeta station id.
// Returns 0 when the URL is not a known Radio France webradio.
int rf_station_id(const char *url);

// Extract the current program from a livemeta live/<id>/webrf_webradio_player
// response. When the "now" object has a non-empty songUuid (a song is
// playing), out receives "secondLine - firstLine" (artist - title), else
// firstLine alone (a talk program), UTF-8.
// *end_epoch receives "now.endTime" (0 if absent).
// Returns false when the JSON has no usable "now.firstLine".
bool rf_meta_parse(const char *json, char *out, size_t out_size, int64_t *end_epoch);
