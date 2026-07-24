// Host unit tests for the played-episode markers (played.c). The storage
// paths are redirected to /tmp by run.sh (see the #ifndef seam in played.c).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "played.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Must match the -D values in run.sh.
#define TEST_DIR  "/tmp/bugne-played-test"
#define TEST_FILE TEST_DIR "/played.bin"

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void test_before_init(void)
{
    // The s_ready gate: lookups before init answer false, never crash.
    CHECK(!played_contains("http://example.org/e1.mp3"), "contains before init");
}

static void test_init_empty(void)
{
    unlink(TEST_FILE);
    played_init();
    CHECK(!played_contains("http://example.org/e1.mp3"), "empty store");
    played_mark(NULL);      // no-ops, never crash
    played_mark("");
    CHECK(!played_contains(""), "empty url is never contained");
}

static void test_mark_and_contains(void)
{
    played_mark("http://example.org/e1.mp3");
    CHECK(played_contains("http://example.org/e1.mp3"), "marked url found");
    CHECK(!played_contains("http://example.org/e2.mp3"), "other url not found");

    played_mark("http://example.org/e1.mp3");  // double mark is a no-op
    CHECK(played_contains("http://example.org/e1.mp3"), "still found after remark");
}

static void test_persistence(void)
{
    played_mark("http://example.org/e2.mp3");
    // Simulate a reboot: reload the statics from the file.
    played_init();
    CHECK(played_contains("http://example.org/e1.mp3"), "e1 survives reload");
    CHECK(played_contains("http://example.org/e2.mp3"), "e2 survives reload");
}

static void test_corrupt_file(void)
{
    FILE *f = fopen(TEST_FILE, "wb");
    if (f) { fwrite("xx", 1, 2, f); fclose(f); }
    played_init();
    CHECK(!played_contains("http://example.org/e1.mp3"), "corrupt file starts empty");
}

static void test_ring_wrap(void)
{
    // Starts empty (after test_corrupt_file). Fill the 256-entry ring, then
    // one more: the oldest entry is evicted, everything else stays.
    char url[64];
    for (int i = 0; i < 256; i++) {
        snprintf(url, sizeof(url), "http://example.org/ep-%03d.mp3", i);
        played_mark(url);
    }
    CHECK(played_contains("http://example.org/ep-000.mp3"), "full ring keeps first");
    CHECK(played_contains("http://example.org/ep-255.mp3"), "full ring keeps last");

    played_mark("http://example.org/ep-256.mp3");
    CHECK(played_contains("http://example.org/ep-256.mp3"), "overflow entry stored");
    CHECK(!played_contains("http://example.org/ep-000.mp3"), "oldest evicted on wrap");
    CHECK(played_contains("http://example.org/ep-001.mp3"), "second oldest survives");

    // The wrapped state also survives a reload.
    played_init();
    CHECK(played_contains("http://example.org/ep-256.mp3"), "wrap survives reload");
    CHECK(!played_contains("http://example.org/ep-000.mp3"), "eviction survives reload");
}

int main(void)
{
    mkdir(TEST_DIR, 0777);
    unlink(TEST_FILE);

    test_before_init();
    test_init_empty();
    test_mark_and_contains();
    test_persistence();
    test_corrupt_file();
    test_ring_wrap();

    unlink(TEST_FILE);
    rmdir(TEST_DIR);

    if (g_fail) {
        printf("test_played: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("test_played: all tests passed\n");
    return 0;
}
