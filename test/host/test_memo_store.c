// Host unit tests for the memo SD layer (memo_store.c). MEMO_ABS_DIR is
// redirected to a /tmp directory by run.sh (see the #ifndef seam in memo.h).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "memo.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

bool g_sd_stub_present = true;  // read by the source_sd.h stub

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define CHECK_STR(got, want, what) \
    CHECK(strcmp((got), (want)) == 0, "%s: got \"%s\", want \"%s\"", (what), (got), (want))

// Empty (and create) the test directory.
static void reset_dir(void)
{
    mkdir(MEMO_ABS_DIR, 0777);
    DIR *dir = opendir(MEMO_ABS_DIR);
    if (!dir) return;
    struct dirent *de;
    char abs[sizeof(MEMO_ABS_DIR) + 256];  // d_name is up to 256 bytes
    // unlink invalidates the iteration order, so loop until a pass is clean
    for (;;) {
        bool removed = false;
        rewinddir(dir);
        while ((de = readdir(dir)) != NULL) {
            if (de->d_name[0] == '.' && (de->d_name[1] == '\0' ||
                (de->d_name[1] == '.' && de->d_name[2] == '\0'))) continue;
            snprintf(abs, sizeof(abs), MEMO_ABS_DIR "/%s", de->d_name);
            unlink(abs);
            removed = true;
        }
        if (!removed) break;
    }
    closedir(dir);
}

// Create a file of 44 header bytes plus data_bytes zeros.
static void touch(const char *name, long data_bytes)
{
    char abs[MEMO_NAME_MAX + 64];
    snprintf(abs, sizeof(abs), MEMO_ABS_DIR "/%s", name);
    FILE *f = fopen(abs, "wb");
    if (!f) { g_fail++; printf("FAIL: cannot create %s\n", abs); return; }
    for (long i = 0; i < 44 + data_bytes; i++) fputc(0, f);
    fclose(f);
}

static bool file_exists(const char *name)
{
    char abs[MEMO_NAME_MAX + 64];
    snprintf(abs, sizeof(abs), MEMO_ABS_DIR "/%s", name);
    struct stat st;
    return stat(abs, &st) == 0;
}

static void test_abs_path(void)
{
    char dst[MEMO_NAME_MAX + 64];
    memo_abs_path(dst, sizeof(dst), "my-001.wav");
    CHECK_STR(dst, MEMO_ABS_DIR "/my-001.wav", "abs path");

    // Overlong names are truncated to MEMO_NAME_MAX - 1 chars by the %.*s.
    char longname[2 * MEMO_NAME_MAX];
    memset(longname, 'a', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    memo_abs_path(dst, sizeof(dst), longname);
    CHECK(strlen(dst) == strlen(MEMO_ABS_DIR) + 1 + MEMO_NAME_MAX - 1,
          "abs path truncates: len %zu", strlen(dst));
}

static void test_missing_dir(void)
{
    reset_dir();
    rmdir(MEMO_ABS_DIR);
    memo_entry_t out[4];
    CHECK(memo_count() == 0, "count without dir");
    CHECK(memo_unread_count() == 0, "unread without dir");
    CHECK(memo_list(out, 4) == 0, "list without dir");
}

static void test_scan_and_list(void)
{
    reset_dir();
    touch("my-001.wav", 0);
    touch("my-004.wav", 32000);            // 1 s at 16 kHz 16-bit mono
    touch("rx-Bench-007.new.wav", 0);
    touch("rx-Bench-002.wav", 0);
    // All of these must be invisible to count/list/unread:
    touch(MEMO_REC_NAME, 0);
    touch("upload.part", 0);
    touch("tk-005.wav", 0);
    touch("notes.txt", 0);

    CHECK(memo_count() == 4, "count: got %d, want 4", memo_count());
    CHECK(memo_unread_count() == 1, "unread: got %d", memo_unread_count());

    memo_entry_t out[8];
    int n = memo_list(out, 8);
    CHECK(n == 4, "list: got %d entries", n);
    if (n == 4) {
        // Sorted newest first (seq descending).
        CHECK(out[0].seq == 7 && out[1].seq == 4 && out[2].seq == 2 && out[3].seq == 1,
              "list order: %d %d %d %d", out[0].seq, out[1].seq, out[2].seq, out[3].seq);
        CHECK(!out[0].is_mine && out[0].unread, "seq 7 is unread rx");
        CHECK_STR(out[0].sender, "Bench", "seq 7 sender");
        CHECK(out[1].is_mine && !out[1].unread, "seq 4 is own");
        CHECK_STR(out[1].name, "my-004.wav", "seq 4 name");
        CHECK(out[1].duration_s == 1, "seq 4 duration: got %d", out[1].duration_s);
        CHECK(out[0].duration_s == 0, "empty file duration: got %d", out[0].duration_s);
    }

    // A smaller out keeps only the newest entries.
    n = memo_list(out, 2);
    CHECK(n == 2, "capped list: got %d", n);
    if (n == 2) {
        CHECK(out[0].seq == 7 && out[1].seq == 4, "capped keeps newest: %d %d",
              out[0].seq, out[1].seq);
        CHECK(out[1].duration_s == 1, "capped duration: got %d, want 1", out[1].duration_s);
    }
}

static void test_keep_rec(void)
{
    // Continues from test_scan_and_list: max stored seq is 7, so keep gets 8.
    touch(MEMO_REC_NAME, 100);
    int seq = memo_keep_rec();
    CHECK(seq == 8, "keep seq: got %d, want 8", seq);
    CHECK(file_exists("my-008.wav"), "kept file exists");
    CHECK(!file_exists(MEMO_REC_NAME), "rec name gone after keep");

    CHECK(memo_keep_rec() == -1, "keep without a capture fails");
}

static void test_rx_create(void)
{
    // Max stored seq is now 8, so the next allocation is 9.
    char final_abs[MEMO_NAME_MAX + 64], part_abs[MEMO_NAME_MAX + 64];
    FILE *f = memo_rx_create("Bench", final_abs, sizeof(final_abs),
                             part_abs, sizeof(part_abs));
    CHECK(f != NULL, "rx create");
    if (f) fclose(f);
    CHECK_STR(final_abs, MEMO_ABS_DIR "/rx-Bench-009.new.wav", "rx final path");
    CHECK(file_exists("rx-Bench-009.new.wav.part"), "rx part exists");
    CHECK(!file_exists("rx-Bench-009.new.wav"), "rx final not created yet");
    unlink(part_abs);

    // A taken .part makes the allocation retry the next sequence number.
    touch("rx-Bench-009.new.wav.part", 0);
    f = memo_rx_create("Bench", final_abs, sizeof(final_abs),
                       part_abs, sizeof(part_abs));
    CHECK(f != NULL, "rx create with collision");
    if (f) fclose(f);
    CHECK_STR(final_abs, MEMO_ABS_DIR "/rx-Bench-010.new.wav", "rx collision retries");
    unlink(part_abs);
    unlink(MEMO_ABS_DIR "/rx-Bench-009.new.wav.part");

    // SD absent: no allocation.
    g_sd_stub_present = false;
    f = memo_rx_create("Bench", final_abs, sizeof(final_abs),
                       part_abs, sizeof(part_abs));
    CHECK(f == NULL, "rx create refused without SD");
    g_sd_stub_present = true;
}

static void test_tk_create(void)
{
    char final_abs[MEMO_NAME_MAX + 64], part_abs[MEMO_NAME_MAX + 64];
    FILE *f = memo_tk_create(final_abs, sizeof(final_abs), part_abs, sizeof(part_abs));
    CHECK(f != NULL, "tk create");
    if (f) fclose(f);
    CHECK_STR(final_abs, MEMO_ABS_DIR "/tk-001.wav", "tk first name");

    // Collision on the next static-counter value skips to the one after.
    touch("tk-002.wav.part", 0);
    f = memo_tk_create(final_abs, sizeof(final_abs), part_abs, sizeof(part_abs));
    CHECK(f != NULL, "tk create with collision");
    if (f) fclose(f);
    CHECK_STR(final_abs, MEMO_ABS_DIR "/tk-003.wav", "tk collision retries");

    // tk files never count as stored memos.
    int before = memo_count();
    touch("tk-004.wav", 100);
    CHECK(memo_count() == before, "tk invisible to count");
}

static void test_clean(void)
{
    reset_dir();
    touch("my-001.wav", 0);
    touch("rx-Bench-002.wav", 0);
    touch("tk-003.wav", 0);
    touch("a.part", 0);
    touch("rx-Bench-005.new.wav.part", 0);
    touch(MEMO_REC_NAME, 0);

    memo_clean_parts();
    CHECK(!file_exists("a.part") && !file_exists("rx-Bench-005.new.wav.part"),
          "parts removed");
    CHECK(!file_exists(MEMO_REC_NAME), "rec removed");
    CHECK(file_exists("my-001.wav") && file_exists("rx-Bench-002.wav") &&
          file_exists("tk-003.wav"), "memos and tk survive clean_parts");

    memo_clean_talkie();
    CHECK(!file_exists("tk-003.wav"), "tk removed by clean_talkie");
    CHECK(file_exists("my-001.wav") && file_exists("rx-Bench-002.wav"),
          "memos survive clean_talkie");
}

// More leftovers than one collection batch holds: the purge must loop until the
// directory is clean, not stop after the first batch (a power cut mid-receive
// can leave several .part files, and stray tk- files are invisible to the list,
// the badge and the 20-memo cap, so nothing else would ever remove them).
static void test_clean_many(void)
{
    reset_dir();
    char name[32];
    for (int i = 0; i < 12; i++) {
        snprintf(name, sizeof(name), "leftover-%02d.part", i);
        touch(name, 0);
        snprintf(name, sizeof(name), "tk-%03d.wav", 100 + i);
        touch(name, 0);
    }
    touch("my-001.wav", 0);

    memo_clean_parts();
    for (int i = 0; i < 12; i++) {
        snprintf(name, sizeof(name), "leftover-%02d.part", i);
        CHECK(!file_exists(name), "every part removed, not just the first batch");
    }

    memo_clean_talkie();
    for (int i = 0; i < 12; i++) {
        snprintf(name, sizeof(name), "tk-%03d.wav", 100 + i);
        CHECK(!file_exists(name), "every tk removed, not just the first batch");
    }
    CHECK(file_exists("my-001.wav"), "stored memo survives both purges");
}

int main(void)
{
    test_abs_path();
    test_missing_dir();
    test_scan_and_list();
    test_keep_rec();
    test_rx_create();
    test_tk_create();
    test_clean();
    test_clean_many();
    reset_dir();
    rmdir(MEMO_ABS_DIR);

    if (g_fail) {
        printf("test_memo_store: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("test_memo_store: all tests passed\n");
    return 0;
}
