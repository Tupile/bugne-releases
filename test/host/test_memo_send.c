// Host unit tests for the memo LAN sender (memo_send.c), focused on the
// error paths. Single translation unit: the .c is included so the test
// shares the scripted esp_http_client stub state. Build and run with
// test/host/run.sh. No ESP-IDF needed.
#include "../../components/memo/memo_send.c"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_WAV "/tmp/bugne-send-test.wav"
#define TEST_WAV_BYTES (44 + 9000)  // 3 full 4 KB chunks incl. a short tail

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void http_reset(void)
{
    memset(&g_http, 0, sizeof(g_http));
    g_http.open_err = ESP_OK;
    g_http.write_budget = -1;
    g_http.status = 200;
}

static void make_wav(long bytes)
{
    FILE *f = fopen(TEST_WAV, "wb");
    if (!f) { g_fail++; printf("FAIL: cannot create %s\n", TEST_WAV); return; }
    for (long i = 0; i < bytes; i++) fputc((int)(i & 0xff), f);
    fclose(f);
}

static void test_bad_file(void)
{
    int status = -1;
    http_reset();
    unlink(TEST_WAV);
    CHECK(memo_send("192.0.2.1", 80, "Bench", TEST_WAV, false, &status, NULL)
          == ESP_ERR_INVALID_ARG, "missing file");
    CHECK(status == 0, "missing file reports no status");

    make_wav(44);  // header only: nothing to send
    CHECK(memo_send("192.0.2.1", 80, "Bench", TEST_WAV, false, &status, NULL)
          == ESP_ERR_INVALID_ARG, "header-only file");
}

static void test_client_failures(void)
{
    make_wav(TEST_WAV_BYTES);

    http_reset();
    g_http.init_fail = 1;
    CHECK(memo_send("192.0.2.1", 80, "Bench", TEST_WAV, false, NULL, NULL)
          == ESP_FAIL, "client init failure");

    http_reset();
    g_http.open_err = ESP_FAIL;
    CHECK(memo_send("192.0.2.1", 80, "Bench", TEST_WAV, false, NULL, NULL)
          == ESP_FAIL, "connect failure");
    CHECK(g_http.closed == 0 && g_http.cleaned == 1, "failed open still cleans up");

    int status = -1;
    http_reset();
    g_http.write_budget = 1000;  // connection dies mid-body
    CHECK(memo_send("192.0.2.1", 80, "Bench", TEST_WAV, false, &status, NULL)
          == ESP_FAIL, "write failure");
    CHECK(status == 0, "write failure reports no status");
    CHECK(g_http.closed == 1 && g_http.cleaned == 1, "aborted send cleans up");

    http_reset();
    g_http.fetch_headers_ret = -1;  // no answer after the body
    CHECK(memo_send("192.0.2.1", 80, "Bench", TEST_WAV, false, &status, NULL)
          == ESP_FAIL, "no response headers");
}

static void test_status_codes(void)
{
    int status = 0;

    http_reset();
    g_http.status = 500;
    CHECK(memo_send("192.0.2.1", 80, "Bench", TEST_WAV, false, &status, NULL)
          == ESP_FAIL, "HTTP 500 fails");
    CHECK(status == 500, "HTTP 500 still reported: got %d", status);

    http_reset();
    g_http.status = 507;  // receiver full (20-memo cap)
    CHECK(memo_send("192.0.2.1", 80, "Bench", TEST_WAV, false, &status, NULL)
          == ESP_FAIL, "HTTP 507 fails");
    CHECK(status == 507, "HTTP 507 reported");

    // 202 = talkie stored as a normal memo: still a successful delivery.
    http_reset();
    g_http.status = 202;
    CHECK(memo_send("192.0.2.1", 80, "Bench", TEST_WAV, true, &status, NULL)
          == ESP_OK, "HTTP 202 succeeds");
    CHECK(status == 202, "HTTP 202 reported");
}

static void test_success(void)
{
    volatile int pct = -1;
    int status = 0;
    http_reset();
    CHECK(memo_send("192.0.2.1", 8080, "Bench", TEST_WAV, false, &status, &pct)
          == ESP_OK, "plain send");
    CHECK(status == 200, "status 200");
    CHECK(g_http.written == TEST_WAV_BYTES, "whole file sent: %ld", g_http.written);
    CHECK(pct == 100, "progress reached 100: got %d", pct);
    CHECK(strcmp(g_http.url, "http://192.0.2.1:8080/api/memo?from=Bench") == 0,
          "url: got \"%s\"", g_http.url);

    // Partial writes are retried until the chunk is fully out.
    http_reset();
    g_http.write_chunk_max = 333;
    CHECK(memo_send("192.0.2.1", 80, "Bench", TEST_WAV, false, &status, NULL)
          == ESP_OK, "partial writes");
    CHECK(g_http.written == TEST_WAV_BYTES, "partial writes send it all");

    // talkie=1 rides the query string.
    http_reset();
    CHECK(memo_send("192.0.2.1", 80, "Bench", TEST_WAV, true, NULL, NULL)
          == ESP_OK, "talkie send");
    CHECK(strstr(g_http.url, "?from=Bench&talkie=1") != NULL,
          "talkie url: got \"%s\"", g_http.url);
}

int main(void)
{
    test_bad_file();
    test_client_failures();
    test_status_codes();
    test_success();

    unlink(TEST_WAV);

    if (g_fail) {
        printf("test_memo_send: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("test_memo_send: all tests passed\n");
    return 0;
}
