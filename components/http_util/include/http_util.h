// http_util: esp_http_client helpers shared by the podcast and stream code.
#pragma once

#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

// esp_http_client_init with cfg->url percent-escaped first (see url_escape.h):
// feed URLs with raw UTF-8 or spaces are otherwise refused at init. The URL is
// copied by esp_http_client_init, so nothing needs to outlive this call.
esp_http_client_handle_t http_util_client_init(const esp_http_client_config_t *cfg);

#ifdef __cplusplus
}
#endif
