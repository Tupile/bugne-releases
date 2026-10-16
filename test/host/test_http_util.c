// Host tests for components/http_util/http_util.c.
#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "../../components/http_util/http_util.c"

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void http_reset(void)
{
    memset(&g_http, 0, sizeof(g_http));
}

int main(void)
{
    esp_http_client_config_t cfg;
    esp_http_client_handle_t client;

    // NULL config
    CHECK(http_util_client_init(NULL) == NULL, "NULL config should return NULL");

    // Valid URL without escaping
    http_reset();
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = "http://example.com/path";
    client = http_util_client_init(&cfg);
    CHECK(client != NULL, "valid init");
    CHECK(strcmp(g_http.url, "http://example.com/path") == 0, "url unmodified: %s", g_http.url);

    // URL that needs escaping (spaces, non-ASCII)
    http_reset();
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = "http://example.com/a b\xC3\xA9";
    client = http_util_client_init(&cfg);
    CHECK(client != NULL, "escaped init");
    CHECK(strcmp(g_http.url, "http://example.com/a%20b%C3%A9") == 0, "url escaped: %s", g_http.url);

    // init_fail mock
    http_reset();
    g_http.init_fail = 1;
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = "http://example.com/";
    client = http_util_client_init(&cfg);
    CHECK(client == NULL, "init_fail propagates");

    if (g_fail) {
        printf("test_http_util: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("test_http_util: all tests passed\n");
    return 0;
}
