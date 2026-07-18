// Host unit tests for the memo filename scheme (memo_name.c).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "memo.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define CHECK_STR(got, want, what) \
    CHECK(strcmp((got), (want)) == 0, "%s: got \"%s\", want \"%s\"", (what), (got), (want))

static void test_sanitize(void)
{
    char s[MEMO_SENDER_MAX];
    memo_sanitize_sender(s, sizeof(s), "Bugne Alpha");
    CHECK_STR(s, "Bugne-Alpha", "space to dash");
    memo_sanitize_sender(s, sizeof(s), "d\xc3\xa9j\xc3\xa0");   // "déjà": UTF-8 collapses
    CHECK_STR(s, "d-j", "multibyte collapsed");
    memo_sanitize_sender(s, sizeof(s), "a/b..c<>");
    CHECK_STR(s, "a-b-c", "specials collapsed, trailing dropped");
    memo_sanitize_sender(s, sizeof(s), "");
    CHECK_STR(s, "peer", "empty fallback");
    memo_sanitize_sender(s, sizeof(s), "---");
    CHECK_STR(s, "peer", "all-specials fallback");
    memo_sanitize_sender(s, sizeof(s), "abcdefghijklmnopqrstuvwxyz0123");
    CHECK(strlen(s) == MEMO_SENDER_MAX - 1, "truncated to %d, got %zu",
          MEMO_SENDER_MAX - 1, strlen(s));
}

static void test_build(void)
{
    char n[MEMO_NAME_MAX];
    memo_name_mine(n, sizeof(n), 7);
    CHECK_STR(n, "my-007.wav", "own name");
    memo_name_rx(n, sizeof(n), "Bugne-Alpha", 12);
    CHECK_STR(n, "rx-Bugne-Alpha-012.new.wav", "rx name");
}

static void test_parse(void)
{
    bool mine, unread;
    char sender[MEMO_SENDER_MAX];
    int seq;

    CHECK(memo_name_parse("my-007.wav", &mine, sender, sizeof(sender), &seq, &unread),
          "own parses");
    CHECK(mine && seq == 7 && !unread && sender[0] == '\0', "own fields");

    CHECK(memo_name_parse("rx-Bugne-Alpha-012.new.wav", &mine, sender, sizeof(sender),
                          &seq, &unread), "rx new parses");
    CHECK(!mine && seq == 12 && unread, "rx new fields");
    CHECK_STR(sender, "Bugne-Alpha", "sender with dash");

    CHECK(memo_name_parse("rx-abc123-500.wav", &mine, sender, sizeof(sender), &seq, &unread),
          "rx read parses");
    CHECK(!mine && seq == 500 && !unread, "rx read fields");
    CHECK_STR(sender, "abc123", "sender ending in digits");

    CHECK(!memo_name_parse(".rec.wav", &mine, sender, sizeof(sender), &seq, &unread),
          "capture temp ignored");
    CHECK(!memo_name_parse("rx-a-001.new.wav.part", &mine, sender, sizeof(sender), &seq,
                           &unread), "part ignored");
    CHECK(!memo_name_parse("song.wav", &mine, sender, sizeof(sender), &seq, &unread),
          "foreign wav ignored");
    CHECK(!memo_name_parse("my-.wav", &mine, sender, sizeof(sender), &seq, &unread),
          "no digits ignored");
    CHECK(!memo_name_parse("rx--001.wav", &mine, sender, sizeof(sender), &seq, &unread),
          "empty sender ignored");
}

int main(void)
{
    test_sanitize();
    test_build();
    test_parse();
    if (g_fail) { printf("%d FAILURES\n", g_fail); return 1; }
    printf("all memo_name tests passed\n");
    return 0;
}
