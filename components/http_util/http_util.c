// http_util: esp_http_client helpers shared by the podcast and stream code.
#include "http_util.h"
#include "url_escape.h"

#include <stdlib.h>
#include <string.h>

esp_http_client_handle_t http_util_client_init(const esp_http_client_config_t *cfg)
{
    if (!cfg) return NULL;
    esp_http_client_config_t c = *cfg;
    char *escaped = NULL;
    if (c.url && url_escape_len(c.url) != strlen(c.url)) {
        escaped = url_escape_dup(c.url);
        if (!escaped) return NULL;
        c.url = escaped;
    }
    esp_http_client_handle_t client = esp_http_client_init(&c);
    free(escaped);
    return client;
}
