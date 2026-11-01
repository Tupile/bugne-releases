// Host unit tests for the new-episode markers (podcast_new.c). run.sh
// redirects storage to /tmp and shrinks the cap to 4 to exercise the ring.
#include "podcast_new.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_DIR  "/tmp/bugne-podnew-test"   // must match run.sh
#define TEST_FILE TEST_DIR "/new.bin"

static int g_fail;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void reset(void)
{
    unlink(TEST_FILE);
    podcast_new_init();
}

static void test_before_init(void)
{
    podcast_new_add(1, "http://e/1.mp3");  // ignored: not ready
    CHECK(!podcast_new_contains("http://e/1.mp3"), "add before init is a no-op");
    CHECK(podcast_new_count(1) == 0, "count before init");
}

static void test_add_count_clear(void)
{
    reset();
    podcast_new_add(1, "http://e/1.mp3");
    podcast_new_add(1, "http://e/2.mp3");
    podcast_new_add(1, "http://e/2.mp3");  // duplicate
    podcast_new_add(2, "http://e/3.mp3");
    podcast_new_add(2, "");                // ignored
    podcast_new_add(2, NULL);              // ignored
    CHECK(podcast_new_count(1) == 2, "count pod 1 = %d", podcast_new_count(1));
    CHECK(podcast_new_count(2) == 1, "count pod 2");
    CHECK(podcast_new_count(3) == 0, "count unknown pod");
    CHECK(podcast_new_contains("http://e/2.mp3"), "contains");
    podcast_new_clear("http://e/2.mp3");
    CHECK(!podcast_new_contains("http://e/2.mp3"), "cleared");
    CHECK(podcast_new_count(1) == 1, "count after clear");
    podcast_new_clear("http://e/none.mp3");  // absent: no-op
    CHECK(podcast_new_count(1) == 1 && podcast_new_count(2) == 1, "clear absent");
}

static void test_persist(void)
{
    reset();
    podcast_new_add(5, "http://e/a.mp3");
    podcast_new_add(6, "http://e/b.mp3");
    podcast_new_clear("http://e/a.mp3");
    podcast_new_init();  // reload from flash
    CHECK(!podcast_new_contains("http://e/a.mp3"), "clear persisted");
    CHECK(podcast_new_contains("http://e/b.mp3"), "add persisted");
    CHECK(podcast_new_count(6) == 1, "count after reload");
}

static void test_ring(void)
{
    reset();  // cap is 4 in the host build
    char u[32];
    for (int i = 0; i < 6; i++) {
        snprintf(u, sizeof(u), "http://e/r%d.mp3", i);
        podcast_new_add(7, u);
    }
    CHECK(podcast_new_count(7) == 4, "ring keeps cap entries (%d)", podcast_new_count(7));
    CHECK(!podcast_new_contains("http://e/r0.mp3") && !podcast_new_contains("http://e/r1.mp3"),
          "oldest overwritten");
    CHECK(podcast_new_contains("http://e/r5.mp3"), "newest kept");
    podcast_new_init();
    CHECK(podcast_new_count(7) == 4 && podcast_new_contains("http://e/r4.mp3"), "ring persisted");
    podcast_new_clear("http://e/r3.mp3");
    podcast_new_add(7, "http://e/r6.mp3");  // reuses the freed slot, overwrites nothing
    CHECK(podcast_new_count(7) == 4 && podcast_new_contains("http://e/r2.mp3"), "freed slot reused");
}

static void test_retain(void)
{
    reset();
    podcast_new_add(1, "http://e/1.mp3");
    podcast_new_add(1, "http://e/2.mp3");
    podcast_new_add(1, "http://e/3.mp3");
    podcast_new_add(2, "http://e/4.mp3");
    uint64_t keep[] = { podcast_new_hash("http://e/3.mp3"), podcast_new_hash("http://e/4.mp3") };
    podcast_new_retain(1, keep, 2);
    CHECK(podcast_new_count(1) == 1 && podcast_new_contains("http://e/3.mp3"), "retain pod 1");
    CHECK(podcast_new_count(2) == 1, "other podcast untouched");
    podcast_new_retain(2, NULL, 0);
    CHECK(podcast_new_count(2) == 0, "retain nothing");
    podcast_new_init();
    CHECK(podcast_new_count(1) == 1 && podcast_new_count(2) == 0, "retain persisted");
}

static void test_corrupt(void)
{
    FILE *f = fopen(TEST_FILE, "wb");
    int bad[2] = { 999, 0 };
    fwrite(bad, sizeof(bad), 1, f);
    fclose(f);
    podcast_new_init();
    CHECK(podcast_new_count(1) == 0, "corrupt file starts empty");
    podcast_new_add(1, "http://e/x.mp3");
    CHECK(podcast_new_count(1) == 1, "usable after corrupt load");
}

int main(void)
{
    test_before_init();
    test_add_count_clear();
    test_persist();
    test_ring();
    test_retain();
    test_corrupt();
    if (g_fail) { printf("%d failure(s)\n", g_fail); return 1; }
    printf("podcast_new: all tests passed\n");
    return 0;
}
