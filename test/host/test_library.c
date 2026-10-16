// Host unit tests for library.c
// Redirecting paths to /tmp so we can run file operations safely.
// Build and run with test/host/run.sh. No ESP-IDF needed.

#include "../../components/library/library.c"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define CHECK_STR(got, want, what) \
    CHECK(strcmp((got), (want)) == 0, "%s: got \"%s\", want \"%s\"", (what), (got), (want))

#define CHECK_INT(got, want, what) \
    CHECK((got) == (want), "%s: got %d, want %d", (what), (int)(got), (int)(want))

bool g_sd_stub_present = true;

// Mock implementation of decode_read_tags for testing.
// In the test, we'll create empty dummy files, so the actual read won't yield data.
// We'll use the fseek/ftell calls or some static state to figure out what to return.
// For simplicity, we can read a tiny payload from the dummy file if we write it.
int decode_read_tags(decode_format_t fmt, decode_source_t *src, decode_tags_t *tags) {
    (void)fmt;
    memset(tags, 0, sizeof(*tags));
    // We write a simple string into the test files to identify them.
    char buf[128] = {0};
    if (src->read(src->ctx, buf, sizeof(buf) - 1) > 0) {
        // format: Artist|Album|Title|Track
        char *p = buf;
        char *ar = strsep(&p, "|");
        char *al = strsep(&p, "|");
        char *ti = strsep(&p, "|");
        char *tr = strsep(&p, "|");
        if (ar) strncpy(tags->artist, ar, sizeof(tags->artist)-1);
        if (al) strncpy(tags->album, al, sizeof(tags->album)-1);
        if (ti) strncpy(tags->title, ti, sizeof(tags->title)-1);
        if (tr) tags->track = atoi(tr);
    }
    return 0; // ESP_OK
}

static void setup_dir(void) {
    mkdir(SD_MOUNT, 0777);
    // write a few test files
    mkdir(SD_MOUNT "/Music", 0777);
    mkdir(SD_MOUNT "/Music/Artist1", 0777);
    mkdir(SD_MOUNT "/Music/Artist2", 0777);

    FILE *f1 = fopen(SD_MOUNT "/Music/Artist1/song1.mp3", "wb");
    if (f1) { fprintf(f1, "Artist One|Album One|Song One|1"); fclose(f1); }

    FILE *f2 = fopen(SD_MOUNT "/Music/Artist1/song2.mp3", "wb");
    if (f2) { fprintf(f2, "Artist One|Album One|Song Two|2"); fclose(f2); }

    FILE *f3 = fopen(SD_MOUNT "/Music/Artist2/song3.flac", "wb");
    if (f3) { fprintf(f3, "Artist Two|Album Two|Song Three|1"); fclose(f3); }

    // untagged file
    FILE *f4 = fopen(SD_MOUNT "/Music/untagged.ogg", "wb");
    if (f4) { fclose(f4); } // empty -> decode_read_tags will return empty tags
}

static void test_scan_and_load(void) {
    remove(INDEX_PATH);
    remove(INDEX_TMP);

    // Ensure allocation and correct track count after scan
    CHECK_INT(library_scan(), 0, "library_scan ok");
    CHECK_INT(library_track_count(), 4, "track count");

    // Clear in-memory structure
    s_count = 0;
    memset(s_tracks, 0, sizeof(lib_track_t) * 10);

    // Load from index
    CHECK_INT(library_load(), 0, "library_load ok");
    CHECK_INT(library_track_count(), 4, "track count after load");
}

static void test_browsing(void) {
    char artists[10][LIB_NAME_MAX];
    size_t n = library_artists(artists, 10);
    CHECK_INT(n, 3, "artist count");
    // Should be sorted alphabetically: "Artist One", "Artist Two", "Unknown artist"
    CHECK_STR(artists[0], "Artist One", "artist 0");
    CHECK_STR(artists[1], "Artist Two", "artist 1");
    CHECK_STR(artists[2], "Unknown artist", "artist 2");

    char albums[10][LIB_NAME_MAX];
    n = library_albums("Artist One", albums, 10);
    CHECK_INT(n, 1, "Artist One album count");
    CHECK_STR(albums[0], "Album One", "Artist One album 0");

    n = library_albums("Unknown artist", albums, 10);
    CHECK_INT(n, 1, "Unknown artist album count");
    CHECK_STR(albums[0], "Unknown album", "Unknown artist album 0");

    char titles[10][LIB_NAME_MAX];
    char paths[10][LIB_PATH_MAX];
    n = library_album_tracks("Artist One", "Album One", titles, paths, 10);
    CHECK_INT(n, 2, "Album One track count");
    CHECK_STR(titles[0], "Song One", "Album One track 0");
    CHECK_STR(titles[1], "Song Two", "Album One track 1");

    n = library_album_tracks("Unknown artist", "Unknown album", titles, paths, 10);
    CHECK_INT(n, 1, "Unknown album track count");
    CHECK_STR(titles[0], "untagged", "Unknown track title fallback to filename");
}

int main(void) {
    setup_dir();
    test_scan_and_load();
    test_browsing();

    if (g_fail == 0) {
        printf("OK: all library host tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", g_fail);
    return 1;
}
