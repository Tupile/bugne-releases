// Host-test stub: config_store.h includes esp_err.h just for the esp_err_t
// typedef used by function declarations that alarm_next.h never calls. Not
// the real ESP-IDF header.
#pragma once
typedef int esp_err_t;
