// Host unit tests for the SD relative-path guard (source_sd_rel_path_safe in
// source_sd.h). Build and run with test/host/run.sh. No ESP-IDF needed.
#include "source_sd.h"

#include <stdio.h>

static int g_fail;

#define CHECK_SAFE(path, want) do { \
    bool got = source_sd_rel_path_safe(path); \
    if (got != (want)) { g_fail++; printf("FAIL: \"%s\": got %d, want %d\n", (path), got, (want)); } \
} while (0)

int main(void)
{
    // Real episode names whose title has an ellipsis: they must be allowed
    // (bench 2026-09-24, podcast 11 ep 109 and podcast 13 ep 245).
    CHECK_SAFE("podcasts/Bestioles/La tique\xc2\xa0_ un vampire tr\xc3\xa8s sympa... tique.m4a", true);
    CHECK_SAFE("podcasts/Petits Curieux/Pourquoi les notes de musique s'appellent-elles do, r\xc3\xa9, mi... _.mp3", true);
    CHECK_SAFE("podcasts/Bestioles/La tique.m4a.part", true);
    CHECK_SAFE("a..b/c", true);

    // Ordinary paths.
    CHECK_SAFE("", true);
    CHECK_SAFE("a", true);
    CHECK_SAFE("music/a/b.mp3", true);
    CHECK_SAFE(".hidden", true);
    CHECK_SAFE("memos/rx-Bench-001.new.wav", true);

    // Traversal and absolute paths stay refused.
    CHECK_SAFE("..", false);
    CHECK_SAFE("../x", false);
    CHECK_SAFE("a/../b", false);
    CHECK_SAFE("a/..", false);
    CHECK_SAFE("a/.../b", false);
    CHECK_SAFE("a\\..\\b", false);   // FatFs also splits on backslash
    CHECK_SAFE("..\\x", false);
    CHECK_SAFE("/abs", false);
    CHECK_SAFE("\\abs", false);
    if (source_sd_rel_path_safe(NULL)) { g_fail++; printf("FAIL: NULL accepted\n"); }

    if (g_fail) { printf("sd_path: %d failure(s)\n", g_fail); return 1; }
    printf("sd_path: ellipsis names allowed, traversal and absolute paths refused passed\n");
    return 0;
}
