// Host unit tests for the notification data module (notif.c).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "notif.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static notif_t two_actions(void)
{
    notif_t n;
    memset(&n, 0, sizeof(n));
    strcpy(n.id, "living-1");
    strcpy(n.message, "Turn off the living room?");
    strcpy(n.reply_url, "http://192.0.2.10:8123/api/webhook/abc");
    n.count = 2;
    strcpy(n.act[0].id, "yes");
    strcpy(n.act[0].label, "Yes");
    strcpy(n.act[1].id, "no");
    strcpy(n.act[1].label, "No");
    return n;
}

static void test_url(void)
{
    CHECK(notif_url_ok("http://192.0.2.10:8123/x"), "http accepted");
    CHECK(notif_url_ok("https://ha.example/x"), "https accepted");
    CHECK(!notif_url_ok("ftp://ha.example/x"), "ftp refused");
    CHECK(!notif_url_ok("http://"), "no host refused");
    CHECK(!notif_url_ok("https://"), "no host refused (https)");
    CHECK(!notif_url_ok(""), "empty refused");
    CHECK(!notif_url_ok(NULL), "NULL refused");
    CHECK(!notif_url_ok("192.0.2.10/x"), "no scheme refused");
}

static void test_split(void)
{
    char h[32];
    const char *rest = NULL;
    CHECK(notif_url_split("http://homeassistant.local:8123/api/webhook/x", h, sizeof(h), &rest)
          && strcmp(h, "homeassistant.local") == 0 && strcmp(rest, ":8123/api/webhook/x") == 0,
          "host and port: '%s' '%s'", h, rest ? rest : "(null)");
    CHECK(notif_url_split("https://ha.example/x", h, sizeof(h), &rest)
          && strcmp(h, "ha.example") == 0 && strcmp(rest, "/x") == 0, "no port: '%s'", h);
    CHECK(notif_url_split("http://ha.local", h, sizeof(h), &rest)
          && strcmp(h, "ha.local") == 0 && rest[0] == '\0', "host alone: '%s'", h);
    CHECK(!notif_url_split("http://user@ha.local/x", h, sizeof(h), &rest), "user info refused");
    CHECK(!notif_url_split("http://[fe80::1]:8123/x", h, sizeof(h), &rest), "IPv6 literal refused");
    CHECK(!notif_url_split("http://:8123/x", h, sizeof(h), &rest), "empty host refused");
    CHECK(!notif_url_split("ftp://ha.local/x", h, sizeof(h), &rest), "bad scheme refused");
    char small[4];
    CHECK(!notif_url_split("http://ha.local/x", small, sizeof(small), &rest), "host too long refused");
    // An "@" in the path is not user info.
    CHECK(notif_url_split("http://ha.local/a@b", h, sizeof(h), &rest), "@ in the path accepted");
}

static void test_valid(void)
{
    notif_t n = two_actions();
    CHECK(notif_valid(&n), "two actions with a URL");
    CHECK(!notif_valid(NULL), "NULL");

    notif_t m = n;
    m.message[0] = '\0';
    CHECK(!notif_valid(&m), "no message");

    m = n;
    m.reply_url[0] = '\0';
    CHECK(!notif_valid(&m), "actions without a URL");

    m = n;
    strcpy(m.reply_url, "file:///etc/passwd");
    CHECK(!notif_valid(&m), "actions with a bad scheme");

    m = n;
    m.act[1].id[0] = '\0';
    CHECK(!notif_valid(&m), "action without an id");

    m = n;
    m.act[0].label[0] = '\0';
    CHECK(!notif_valid(&m), "action without a label");

    m = n;
    m.count = 3;
    CHECK(!notif_valid(&m), "count above the maximum");
    m.count = -1;
    CHECK(!notif_valid(&m), "negative count");

    // Text only: no action, no URL needed.
    memset(&m, 0, sizeof(m));
    strcpy(m.message, "Dinner is ready");
    CHECK(notif_valid(&m), "message alone");

    // A URL that is present must still be a usable one.
    strcpy(m.reply_url, "nonsense");
    CHECK(!notif_valid(&m), "bad URL without actions");
}

static void test_reply(void)
{
    char b[128];
    int n = notif_reply_json(b, sizeof(b), "living-1", "yes");
    CHECK(strcmp(b, "{\"id\":\"living-1\",\"action\":\"yes\"}") == 0, "plain: '%s'", b);
    CHECK(n == (int)strlen(b), "length returned: %d", n);

    notif_reply_json(b, sizeof(b), "a\"b\\c", "x");
    CHECK(strcmp(b, "{\"id\":\"a\\\"b\\\\c\",\"action\":\"x\"}") == 0, "escaped: '%s'", b);

    notif_reply_json(b, sizeof(b), "a\nb", "x");
    CHECK(strcmp(b, "{\"id\":\"a\\u000ab\",\"action\":\"x\"}") == 0, "control char: '%s'", b);

    notif_reply_json(b, sizeof(b), "\xC3\xA9t\xC3\xA9", "x");
    CHECK(strcmp(b, "{\"id\":\"\xC3\xA9t\xC3\xA9\",\"action\":\"x\"}") == 0, "UTF-8 kept: '%s'", b);

    n = notif_reply_json(b, sizeof(b), NULL, "x");
    CHECK(n > 0 && strcmp(b, "{\"id\":\"\",\"action\":\"x\"}") == 0, "NULL id is empty: '%s'", b);

    // Too small: refused, never a truncated (invalid) JSON.
    char s[16];
    n = notif_reply_json(s, sizeof(s), "living-1", "yes");
    CHECK(n < 0 && s[0] == '\0', "overflow refused: %d '%s'", n, s);
    CHECK(notif_reply_json(NULL, 10, "a", "b") < 0, "NULL buffer");
    CHECK(notif_reply_json(s, 0, "a", "b") < 0, "zero size");

    // Worst case fits in NOTIF_REPLY_MAX: every byte escaped as \u00XX.
    char id[NOTIF_ID_MAX], act[NOTIF_ACT_ID_MAX], big[NOTIF_REPLY_MAX];
    memset(id, '\x01', sizeof(id) - 1);   id[sizeof(id) - 1] = '\0';
    memset(act, '\x01', sizeof(act) - 1); act[sizeof(act) - 1] = '\0';
    CHECK(notif_reply_json(big, sizeof(big), id, act) > 0, "worst case fits NOTIF_REPLY_MAX");
}

int main(void)
{
    test_url();
    test_split();
    test_valid();
    test_reply();
    if (g_fail) { printf("notif: %d FAILED\n", g_fail); return 1; }
    printf("notif: all tests passed\n");
    return 0;
}
