// Host unit tests for the log ring buffer (logstore.c). Single translation
// unit: the .c is included so the test can drive the vprintf hook the stub
// esp_log.h records. Build and run with test/host/run.sh. No ESP-IDF needed.
#include "../../components/logstore/logstore.c"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

// Feed one formatted line through the hook logstore installed.
static void emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    esp_log_stub_vprintf(fmt, ap);
    va_end(ap);
}

static char g_read[LOGSTORE_SIZE + 1024];

static void test_read_before_init(void)
{
    size_t len = 99;
    logstore_read(g_read, sizeof(g_read), &len);
    CHECK(len == 0 && g_read[0] == '\0', "read before init is empty");

    logstore_read(g_read, 0, &len);  // zero-size buffer, never crash
    CHECK(len == 0, "zero-size read");
}

static void test_capture_order(void)
{
    emit("first %d\n", 1);
    emit("second %s\n", "line");
    size_t len = 0;
    logstore_read(g_read, sizeof(g_read), &len);
    CHECK(strcmp(g_read, "first 1\nsecond line\n") == 0,
          "capture and order: got \"%s\"", g_read);
    CHECK(len == strlen(g_read), "out_len matches");
}

static void test_line_truncation(void)
{
    // The formatting scratch is 512 bytes: a longer line is captured truncated
    // to 511 chars, and the ring keeps working afterwards.
    char big[600];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    size_t before = 0;
    logstore_read(g_read, sizeof(g_read), &before);
    emit("%s", big);
    size_t after = 0;
    logstore_read(g_read, sizeof(g_read), &after);
    CHECK(after - before == 511, "long line truncated: grew %zu", after - before);
}

static void test_small_read_buffer(void)
{
    char small[8];
    size_t len = 0;
    logstore_read(small, sizeof(small), &len);
    CHECK(len == 7 && small[7] == '\0', "small buffer: len %zu", len);
}

static void test_wraparound(void)
{
    // 128-byte lines, enough of them to fill the 16 KB ring twice over.
    char pad[129];
    memset(pad, '.', sizeof(pad) - 1);
    pad[sizeof(pad) - 1] = '\0';
    char line[160];
    int total = (2 * LOGSTORE_SIZE) / 128;
    for (int i = 0; i < total; i++) {
        snprintf(line, sizeof(line), "line %04d %.117s\n", i, pad);
        emit("%s", line);  // each emit is exactly 128 bytes
    }
    size_t len = 0;
    logstore_read(g_read, sizeof(g_read), &len);
    CHECK(len == LOGSTORE_SIZE, "wrapped read is full ring: %zu", len);
    snprintf(line, sizeof(line), "line %04d %.117s\n", total - 1, pad);
    CHECK(strcmp(g_read + len - 128, line) == 0, "last line at the end");
    CHECK(strstr(g_read, "line 0000") == NULL, "oldest lines rolled off");
    // Oldest-first: the earliest surviving full line appears before the last.
    snprintf(line, sizeof(line), "line %04d", total - (LOGSTORE_SIZE / 128));
    CHECK(strstr(g_read, line) != NULL && strstr(g_read, line) < g_read + 256,
          "oldest surviving line near the start");
}

int main(void)
{
    test_read_before_init();

    CHECK(logstore_init() == ESP_OK, "init");
    CHECK(esp_log_stub_vprintf == log_vprintf, "hook installed");

    test_capture_order();
    test_line_truncation();
    test_small_read_buffer();
    test_wraparound();

    if (g_fail) {
        printf("test_logstore: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("test_logstore: all tests passed\n");
    return 0;
}
