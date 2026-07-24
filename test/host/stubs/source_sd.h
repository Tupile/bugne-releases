// Host-test stub: memo_store.c checks SD presence and creates the memo
// directory. The test binary defines g_sd_stub_present and creates the
// (redirected) directory itself, so mkdir here is a no-op success.
#pragma once
#include <stdbool.h>
#include "esp_err.h"

extern bool g_sd_stub_present;

static inline bool source_sd_present(void) { return g_sd_stub_present; }
static inline esp_err_t source_sd_mkdir(const char *rel)
{
    (void)rel;
    return ESP_OK;
}
