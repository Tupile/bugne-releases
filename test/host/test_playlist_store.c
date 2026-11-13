// Host unit tests for the playlist SD layer (playlist_store.c).
// PLAYLIST_ABS_DIR is redirected to a /tmp directory by run.sh (the #ifndef
// seam in playlist.h). Build and run with test/host/run.sh. No ESP-IDF needed.
#include "playlist.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool g_sd_stub_present = true;  // read by the source_sd.h stub

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define CHECK_STR(got, want, what) \
    CHECK(strcmp((got), (want)) == 0, "%s: got \"%s\", want \"%s\"", (what), (got), (want))

static char s_t[PLAYLIST_MAX_TRACKS + 1][PLAYLIST_TITLE_MAX];
static char s_p[PLAYLIST_MAX_TRACKS + 1][PLAYLIST_PATH_MAX];
static char s_names[PLAYLIST_MAX_LISTS + 2][PLAYLIST_NAME_MAX];

static void reset_dir(void)
{
    mkdir(PLAYLIST_ABS_DIR, 0777);
    DIR *dir = opendir(PLAYLIST_ABS_DIR);
    if (!dir) return;
    struct dirent *de;
    char abs[sizeof(PLAYLIST_ABS_DIR) + 258];
    for (;;) {  // unlink disturbs the iteration: loop until a pass is clean
        bool removed = false;
        rewinddir(dir);
        while ((de = readdir(dir)) != NULL) {
            if (de->d_name[0] == '.') continue;
            snprintf(abs, sizeof(abs), PLAYLIST_ABS_DIR "/%s", de->d_name);
            unlink(abs);
            removed = true;
        }
        if (!removed) break;
    }
    closedir(dir);
}

static void put_raw(const char *file, const char *text)
{
    char abs[sizeof(PLAYLIST_ABS_DIR) + 80];
    snprintf(abs, sizeof(abs), PLAYLIST_ABS_DIR "/%s", file);
    FILE *f = fopen(abs, "wb");
    fputs(text, f);
    fclose(f);
}

static bool file_exists(const char *file)
{
    char abs[sizeof(PLAYLIST_ABS_DIR) + 80];
    struct stat st;
    snprintf(abs, sizeof(abs), PLAYLIST_ABS_DIR "/%s", file);
    return stat(abs, &st) == 0;
}

static void fill(size_t n)
{
    for (size_t i = 0; i < n; i++) {
        snprintf(s_t[i], PLAYLIST_TITLE_MAX, "Title %zu", i);
        snprintf(s_p[i], PLAYLIST_PATH_MAX, "music/t%03zu.mp3", i);
    }
}

static void test_write_read(void)
{
    reset_dir();
    size_t n = 99;
    bool tr = true;
    CHECK(playlist_read("Rock", s_t, s_p, PLAYLIST_MAX_TRACKS, &n, &tr, 0) == ESP_ERR_NOT_FOUND, "unknown playlist");

    fill(3);
    CHECK(playlist_write("Rock", s_t, s_p, 3, 0) == ESP_OK, "write");
    CHECK(file_exists("Rock.m3u") && !file_exists("Rock.m3u.tmp"), "final file, no temp left");
    memset(s_t, 0, sizeof(s_t)); memset(s_p, 0, sizeof(s_p));
    CHECK(playlist_read("Rock", s_t, s_p, PLAYLIST_MAX_TRACKS, &n, &tr, 0) == ESP_OK && n == 3 && !tr, "read back: %zu", n);
    CHECK_STR(s_t[2], "Title 2", "title");
    CHECK_STR(s_p[2], "music/t002.mp3", "path");

    // A write replaces the whole list; an empty list is a valid playlist.
    fill(1);
    CHECK(playlist_write("Rock", s_t, s_p, 1, 0) == ESP_OK, "replace");
    CHECK(playlist_read("Rock", s_t, s_p, PLAYLIST_MAX_TRACKS, &n, &tr, 0) == ESP_OK && n == 1, "replaced: %zu", n);
    CHECK(playlist_write("Empty", s_t, s_p, 0, 0) == ESP_OK, "empty playlist");
    CHECK(playlist_read("Empty", s_t, s_p, PLAYLIST_MAX_TRACKS, &n, &tr, 0) == ESP_OK && n == 0, "empty read: %zu", n);

    // A failed write (unsafe path) keeps the previous file and leaves no temp.
    fill(2);
    strcpy(s_p[1], "../escape.mp3");
    CHECK(playlist_write("Rock", s_t, s_p, 2, 0) != ESP_OK, "unsafe path refused");
    CHECK(!file_exists("Rock.m3u.tmp"), "temp removed after a failed write");
    CHECK(playlist_read("Rock", s_t, s_p, PLAYLIST_MAX_TRACKS, &n, &tr, 0) == ESP_OK && n == 1, "previous file intact: %zu", n);

    // Bad arguments.
    fill(PLAYLIST_MAX_TRACKS + 1);
    CHECK(playlist_write("Big", s_t, s_p, PLAYLIST_MAX_TRACKS + 1, 0) == ESP_ERR_INVALID_ARG, "129 tracks refused");
    CHECK(playlist_write("Big", s_t, s_p, PLAYLIST_MAX_TRACKS, 0) == ESP_OK, "128 tracks accepted");
    CHECK(playlist_write("a/b", s_t, s_p, 1, 0) == ESP_ERR_INVALID_ARG, "bad name on write");
    CHECK(playlist_read("../x", s_t, s_p, PLAYLIST_MAX_TRACKS, &n, &tr, 0) == ESP_ERR_INVALID_ARG, "bad name on read");

    // A file from a PC with more than the cap reads as truncated.
    static char text[(PLAYLIST_MAX_TRACKS + 2) * 20];
    size_t off = 0;
    for (int i = 0; i < PLAYLIST_MAX_TRACKS + 2; i++) off += (size_t)sprintf(text + off, "music/x%03d.mp3\r\n", i);
    put_raw("PC.m3u", text);
    CHECK(playlist_read("PC", s_t, s_p, PLAYLIST_MAX_TRACKS, &n, &tr, 0) == ESP_OK && n == PLAYLIST_MAX_TRACKS && tr,
          "PC file truncated: %zu, %d", n, tr);
}

static void test_list(void)
{
    reset_dir();
    size_t n = 99;
    CHECK(playlist_list(s_names, PLAYLIST_MAX_LISTS, &n, 0) == ESP_OK && n == 0, "empty folder: %zu", n);
    fill(1);
    playlist_write("zebra", s_t, s_p, 1, 0);
    playlist_write("Alpha", s_t, s_p, 1, 0);
    playlist_write("mike", s_t, s_p, 1, 0);
    put_raw("ghost.m3u.tmp", "x");        // temp of an interrupted write
    put_raw("notes.txt", "x");            // not a playlist
    put_raw("UPPER.M3U", "music/a.mp3\n");  // extension in another case
    CHECK(playlist_list(s_names, PLAYLIST_MAX_LISTS, &n, 0) == ESP_OK && n == 4, "list count: %zu", n);
    CHECK_STR(s_names[0], "Alpha", "name order, case-insensitive");
    CHECK_STR(s_names[1], "mike", "name order");
    CHECK_STR(s_names[2], "UPPER", "extension stripped whatever its case");
    CHECK_STR(s_names[3], "zebra", "name order");
    // max is honored.
    CHECK(playlist_list(s_names, 2, &n, 0) == ESP_OK && n == 2, "max honored: %zu", n);
}

static void test_cap(void)
{
    reset_dir();
    fill(1);
    char name[PLAYLIST_NAME_MAX];
    for (int i = 0; i < PLAYLIST_MAX_LISTS; i++) {
        snprintf(name, sizeof(name), "list %02d", i);
        CHECK(playlist_write(name, s_t, s_p, 1, 0) == ESP_OK, "playlist %d", i);
    }
    CHECK(playlist_write("one too many", s_t, s_p, 1, 0) == ESP_ERR_NO_MEM, "21st playlist refused");
    CHECK(!file_exists("one too many.m3u") && !file_exists("one too many.m3u.tmp"), "nothing written for the 21st");
    CHECK(playlist_write("list 05", s_t, s_p, 1, 0) == ESP_OK, "replacing an existing one at the cap");
}

static void test_rename_delete(void)
{
    reset_dir();
    size_t n;
    bool tr;
    fill(2);
    playlist_write("Old", s_t, s_p, 2, 0);
    playlist_write("Other", s_t, s_p, 1, 0);
    CHECK(playlist_rename("Old", "Other", 0) == ESP_ERR_NOT_ALLOWED, "rename onto an existing playlist");
    CHECK(playlist_rename("Nope", "New", 0) == ESP_ERR_NOT_FOUND, "rename an unknown playlist");
    CHECK(playlist_rename("Old", "a:b", 0) == ESP_ERR_INVALID_ARG, "rename to a bad name");
    CHECK(playlist_rename("Old", "New", 0) == ESP_OK, "rename");
    CHECK(!file_exists("Old.m3u") && file_exists("New.m3u"), "file moved");
    CHECK(playlist_read("New", s_t, s_p, PLAYLIST_MAX_TRACKS, &n, &tr, 0) == ESP_OK && n == 2, "content kept: %zu", n);
    CHECK(playlist_rename("New", "NEW", 0) == ESP_OK, "case-only rename is allowed");

    CHECK(playlist_delete("Other", 0) == ESP_OK && !file_exists("Other.m3u"), "delete");
    CHECK(playlist_delete("Other", 0) == ESP_ERR_NOT_FOUND, "delete twice");
    CHECK(playlist_delete("../x", 0) == ESP_ERR_INVALID_ARG, "delete with a bad name");
}

static void test_no_card(void)
{
    reset_dir();
    size_t n;
    bool tr;
    fill(1);
    g_sd_stub_present = false;
    CHECK(playlist_list(s_names, PLAYLIST_MAX_LISTS, &n, 0) == ESP_ERR_INVALID_STATE, "list without a card");
    CHECK(playlist_read("x", s_t, s_p, PLAYLIST_MAX_TRACKS, &n, &tr, 0) == ESP_ERR_INVALID_STATE, "read without a card");
    CHECK(playlist_write("x", s_t, s_p, 1, 0) == ESP_ERR_INVALID_STATE, "write without a card");
    CHECK(playlist_rename("x", "y", 0) == ESP_ERR_INVALID_STATE, "rename without a card");
    CHECK(playlist_delete("x", 0) == ESP_ERR_INVALID_STATE, "delete without a card");
    g_sd_stub_present = true;
}

int main(void)
{
    playlist_init();
    test_write_read();
    test_list();
    test_cap();
    test_rename_delete();
    test_no_card();
    reset_dir();
    rmdir(PLAYLIST_ABS_DIR);
    if (g_fail) { printf("test_playlist_store: %d FAILED\n", g_fail); return 1; }
    printf("test_playlist_store: all passed\n");
    return 0;
}
