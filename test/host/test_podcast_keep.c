// Host tests for podcast_keep_select / podcast_keep_window.
#include <stdio.h>
#include <string.h>
#include "podcast_keep.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main(void)
{
    bool cached[6] = { true, true, true, true, true, true };
    bool prot[6] = { 0 };
    bool del[6];

    CHECK(podcast_keep_select(cached, prot, 6, 0, del) == 0, "keep 0 = no limit");
    CHECK(podcast_keep_select(cached, prot, 6, -3, del) == 0, "negative keep = no limit");
    CHECK(podcast_keep_select(cached, prot, 6, 6, del) == 0, "keep == count marks nothing");
    CHECK(podcast_keep_select(cached, prot, 6, 10, del) == 0, "keep > count marks nothing");

    CHECK(podcast_keep_select(cached, prot, 6, 2, del) == 4, "keep 2 of 6 marks the 4 oldest");
    CHECK(!del[0] && !del[1] && del[2] && del[5], "newest (first) entries stay");

    // Not downloaded: nothing to delete, and the window stays positional.
    cached[0] = false; cached[3] = false;
    CHECK(podcast_keep_select(cached, prot, 6, 2, del) == 3, "absent files are not marked");
    CHECK(!del[1] && del[2] && !del[3], "window is positional, not a count of files");

    // Protected (playing, favorite, alarm) is never deleted.
    prot[4] = true;
    CHECK(podcast_keep_select(cached, prot, 6, 2, del) == 2, "protected entry is kept");
    CHECK(!del[4] && del[5], "protected skipped, the others still go");
    CHECK(podcast_keep_select(cached, NULL, 6, 2, del) == 3, "NULL protect list allowed");

    CHECK(podcast_keep_window(30, 0) == 30, "window: no limit");
    CHECK(podcast_keep_window(30, 10) == 10, "window: limited");
    CHECK(podcast_keep_window(4, 10) == 4, "window: short feed");
    CHECK(podcast_keep_window(0, 5) == 0, "window: empty feed");

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("podcast_keep: all tests passed\n");
    return 0;
}
