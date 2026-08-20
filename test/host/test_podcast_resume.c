// Host unit tests for the podcast resume positions (podcast_resume.c). The storage
// paths are redirected to /tmp by run.sh.
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "podcast_resume.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_DIR  "/tmp/bugne-resume-test"
#define TEST_FILE TEST_DIR "/.resume.bin"

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void test_before_init(void)
{
    // Lookups before init answer 0, never crash.
    uint32_t dur = 999;
    CHECK(podcast_resume_get("podcasts/Folder/episode.mp3", &dur) == 0, "get before init pos");
    CHECK(dur == 0, "get before init dur");
}

static void test_init_empty(void)
{
    unlink(TEST_FILE);
    podcast_resume_init();
    
    uint32_t dur = 999;
    CHECK(podcast_resume_get("podcasts/Folder/episode.mp3", &dur) == 0, "empty store pos");
    CHECK(dur == 0, "empty store dur");
    
    // Test null/empty handling
    podcast_resume_set(NULL, 100, 1000);
    podcast_resume_set("", 100, 1000);
    CHECK(podcast_resume_get("", NULL) == 0, "empty path is never found");
}

static void test_set_and_get(void)
{
    uint32_t dur = 0;
    
    // Set first position
    podcast_resume_set("podcasts/Folder/episode.mp3", 5000, 300000);
    CHECK(podcast_resume_get("podcasts/Folder/episode.mp3", &dur) == 5000, "get correct pos");
    CHECK(dur == 300000, "get correct dur");
    
    // Path normalization test: different styles of same path should match
    CHECK(podcast_resume_get("/sdcard/podcasts/Folder/episode.mp3", &dur) == 5000, "sdcard absolute match");
    CHECK(podcast_resume_get("sdcard/podcasts/Folder/episode.mp3", NULL) == 5000, "sdcard relative match");
    CHECK(podcast_resume_get("/podcasts/Folder/episode.mp3", NULL) == 5000, "leading slash match");
    
    // Update existing item
    podcast_resume_set("/sdcard/podcasts/Folder/episode.mp3", 12000, 300000);
    CHECK(podcast_resume_get("podcasts/Folder/episode.mp3", &dur) == 12000, "get updated pos");
}

static void test_clear(void)
{
    uint32_t dur = 999;
    
    // Set a couple of items
    podcast_resume_set("podcasts/Folder/ep1.mp3", 1000, 10000);
    podcast_resume_set("podcasts/Folder/ep2.mp3", 2000, 20000);
    
    // Clear first item
    podcast_resume_clear("podcasts/Folder/ep1.mp3");
    
    // Ep1 should be gone
    CHECK(podcast_resume_get("podcasts/Folder/ep1.mp3", &dur) == 0, "cleared item gone");
    CHECK(dur == 0, "cleared item dur reset");
    
    // Ep2 should still be there
    CHECK(podcast_resume_get("podcasts/Folder/ep2.mp3", &dur) == 2000, "sibling item remains");
    CHECK(dur == 20000, "sibling item dur remains");
}

static void test_persistence(void)
{
    uint32_t dur = 0;
    
    // Reset state
    unlink(TEST_FILE);
    podcast_resume_init();
    
    podcast_resume_set("podcasts/Folder/ep1.mp3", 1500, 15000);
    podcast_resume_set("podcasts/Folder/ep2.mp3", 2500, 25000);
    
    // Reinitialize to reload from file
    podcast_resume_init();
    
    CHECK(podcast_resume_get("podcasts/Folder/ep1.mp3", &dur) == 1500, "loaded ep1 pos");
    CHECK(dur == 15000, "loaded ep1 dur");
    CHECK(podcast_resume_get("podcasts/Folder/ep2.mp3", &dur) == 2500, "loaded ep2 pos");
    CHECK(dur == 25000, "loaded ep2 dur");
}

static void test_corrupt_file(void)
{
    FILE *f = fopen(TEST_FILE, "wb");
    if (f) {
        fwrite("garbage-corrupt-data", 1, 20, f);
        fclose(f);
    }
    
    podcast_resume_init();
    CHECK(podcast_resume_get("podcasts/Folder/ep1.mp3", NULL) == 0, "corrupt file starts empty");
}

static void test_lru_eviction(void)
{
    char path[128];
    uint32_t dur = 0;
    
    // Reset state
    unlink(TEST_FILE);
    podcast_resume_init();
    
    // Fill the table (32 entries)
    for (int i = 0; i < 32; i++) {
        snprintf(path, sizeof(path), "podcasts/Folder/ep-%02d.mp3", i);
        podcast_resume_set(path, i * 100, i * 1000 + 1000);
    }
    
    // Verify all 32 entries exist
    for (int i = 0; i < 32; i++) {
        snprintf(path, sizeof(path), "podcasts/Folder/ep-%02d.mp3", i);
        CHECK(podcast_resume_get(path, &dur) == (uint32_t)(i * 100), "all fill entries present");
    }
    
    // Now insert 33rd entry -> should evict first entry (ep-00)
    podcast_resume_set("podcasts/Folder/ep-32.mp3", 3200, 33000);
    
    CHECK(podcast_resume_get("podcasts/Folder/ep-00.mp3", NULL) == 0, "first entry (LRU) is evicted");
    CHECK(podcast_resume_get("podcasts/Folder/ep-01.mp3", NULL) != 0, "second entry survives");
    CHECK(podcast_resume_get("podcasts/Folder/ep-32.mp3", &dur) == 3200, "new entry is present");
    
    // Update ep-01 so it becomes most recently used
    podcast_resume_set("podcasts/Folder/ep-01.mp3", 9999, 99999);
    
    // Insert 34th entry -> should evict ep-02 (now the oldest/LRU) instead of ep-01!
    podcast_resume_set("podcasts/Folder/ep-33.mp3", 3300, 34000);
    
    CHECK(podcast_resume_get("podcasts/Folder/ep-02.mp3", NULL) == 0, "ep-02 is evicted");
    CHECK(podcast_resume_get("podcasts/Folder/ep-01.mp3", &dur) == 9999, "ep-01 survived eviction because it was updated");
    CHECK(dur == 99999, "ep-01 updated data survives");
}

static void test_lru_after_persistence_reload(void)
{
    char path[128];
    
    // Reset state
    unlink(TEST_FILE);
    podcast_resume_init();
    
    // Fill the table (32 entries)
    for (int i = 0; i < 32; i++) {
        snprintf(path, sizeof(path), "podcasts/Folder/ep-%02d.mp3", i);
        podcast_resume_set(path, i * 100, i * 1000 + 1000);
    }
    
    // Simulate a reboot/reload
    podcast_resume_init();
    
    // Insert 33rd entry -> should still correctly evict ep-00 (oldest) based on restored updated_at order
    podcast_resume_set("podcasts/Folder/ep-32.mp3", 3200, 33000);
    
    CHECK(podcast_resume_get("podcasts/Folder/ep-00.mp3", NULL) == 0, "ep-00 is evicted after reload");
    CHECK(podcast_resume_get("podcasts/Folder/ep-01.mp3", NULL) != 0, "ep-01 survives after reload");
}

int main(void)
{
    mkdir(TEST_DIR, 0777);
    unlink(TEST_FILE);

    test_before_init();
    test_init_empty();
    test_set_and_get();
    test_clear();
    test_persistence();
    test_corrupt_file();
    test_lru_eviction();
    test_lru_after_persistence_reload();

    unlink(TEST_FILE);
    rmdir(TEST_DIR);

    if (g_fail) {
        printf("test_podcast_resume: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("test_podcast_resume: all tests passed\n");
    return 0;
}
