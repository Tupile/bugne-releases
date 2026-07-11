// Host unit tests for the Radio France livemeta helpers (rf_meta.c).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "rf_meta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define CHECK_INT(got, want, what) \
    CHECK((got) == (want), "%s: got %d, want %d", (what), (int)(got), (int)(want))

#define CHECK_STR(got, want, what) \
    CHECK(strcmp((got), (want)) == 0, "%s: got \"%s\", want \"%s\"", (what), (got), (want))

// ---- fixture helper ----

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL: cannot open %s\n", path); g_fail++; return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static void test_live_fip_electro(void)
{
    char *json = read_file("rf_live_fip_electro.json");
    if (!json) return;
    char out[256];
    int64_t end_epoch = -1;
    bool ok = rf_meta_parse(json, out, sizeof out, &end_epoch);
    CHECK(ok, "fip_electro: parse should succeed");
    if (ok) {
        CHECK_STR(out, "Overmono - Arla fearn", "fip_electro title");
        CHECK(end_epoch == 1783239907, "fip_electro end_epoch: got %lld", (long long)end_epoch);
    }
    free(json);
}

static void test_live_france_inter(void)
{
    char *json = read_file("rf_live_france_inter.json");
    if (!json) return;
    char out[256];
    int64_t end_epoch = -1;
    bool ok = rf_meta_parse(json, out, sizeof out, &end_epoch);
    CHECK(ok, "france_inter: parse should succeed");
    if (ok) {
        CHECK_STR(out,
                  "Salman Rushdie, Didier Decoin, Jonas Sollberger, Pauline Clavi\xc3\xa8re, "
                  "Gabriel Tallent\xc2\xa0: que lire cette semaine\xc2\xa0?",
                  "france_inter title");
        CHECK(end_epoch == 1783241999, "france_inter end_epoch: got %lld", (long long)end_epoch);
    }
    free(json);
}

static void test_song_uuid_but_no_second_line(void)
{
    const char *j =
        "{\"now\":{\"firstLine\":\"Title Only\",\"secondLine\":\"\","
        "\"songUuid\":\"abc-123\",\"endTime\":42}}";
    char out[256];
    int64_t end_epoch = 0;
    bool ok = rf_meta_parse(j, out, sizeof out, &end_epoch);
    CHECK(ok, "song, empty secondLine: parse should succeed");
    CHECK_STR(out, "Title Only", "song, empty secondLine");
    CHECK_INT(end_epoch, 42, "song, empty secondLine end");
}

static void test_escaped_quote_and_backslash(void)
{
    const char *j =
        "{\"now\":{\"firstLine\":\"He said \\\"hi\\\" and \\\\ ok\",\"songUuid\":null}}";
    char out[256];
    bool ok = rf_meta_parse(j, out, sizeof out, NULL);
    CHECK(ok, "escaped firstLine: parse should succeed");
    CHECK_STR(out, "He said \"hi\" and \\ ok", "escaped firstLine");
}

static void test_unicode_and_surrogate_pair(void)
{
    // é -> e-acute. 🎵 -> U+1F3B5 (musical note), 4-byte UTF-8.
    const char *j =
        "{\"now\":{\"firstLine\":\"caf\\u00e9 \\ud83c\\udfb5\",\"songUuid\":null}}";
    char out[256];
    bool ok = rf_meta_parse(j, out, sizeof out, NULL);
    CHECK(ok, "unicode firstLine: parse should succeed");
    CHECK_STR(out, "caf\xc3\xa9 \xf0\x9f\x8e\xb5", "unicode firstLine");
}

static void test_first_line_null(void)
{
    const char *j = "{\"now\":{\"firstLine\":null,\"songUuid\":null}}";
    char out[256];
    bool ok = rf_meta_parse(j, out, sizeof out, NULL);
    CHECK(!ok, "firstLine null: parse should fail");
}

static void test_first_line_missing(void)
{
    const char *j = "{\"now\":{\"secondLine\":\"X\",\"songUuid\":null}}";
    char out[256];
    bool ok = rf_meta_parse(j, out, sizeof out, NULL);
    CHECK(!ok, "firstLine missing: parse should fail");
}

static void test_now_missing(void)
{
    const char *j = "{\"prev\":[],\"next\":[]}";
    char out[256];
    bool ok = rf_meta_parse(j, out, sizeof out, NULL);
    CHECK(!ok, "now missing: parse should fail");
}

static void test_truncated_mid_string(void)
{
    const char *j = "{\"now\":{\"firstLine\":\"Unterminat";
    char out[256];
    bool ok = rf_meta_parse(j, out, sizeof out, NULL);
    CHECK(!ok, "truncated mid-string: parse should fail");
}

static void test_end_time_missing(void)
{
    const char *j = "{\"now\":{\"firstLine\":\"X\",\"songUuid\":null}}";
    char out[256];
    int64_t end_epoch = -1;
    bool ok = rf_meta_parse(j, out, sizeof out, &end_epoch);
    CHECK(ok, "endTime missing: parse should still succeed");
    CHECK_STR(out, "X", "endTime missing firstLine");
    CHECK_INT(end_epoch, 0, "endTime missing end_epoch");
}

static void test_out_size_truncation(void)
{
    const char *j = "{\"now\":{\"firstLine\":\"HelloWorld\",\"songUuid\":null}}";
    char out[6];
    bool ok = rf_meta_parse(j, out, sizeof out, NULL);
    CHECK(ok, "out_size truncation: parse should succeed");
    CHECK_STR(out, "Hello", "out_size truncation");
}

// ---- rf_station_id ----

static void test_station_id(void)
{
    CHECK_INT(rf_station_id("https://icecast.radiofrance.fr/fipelectro-midfi.mp3"), 74,
              "fipelectro");
    CHECK_INT(rf_station_id("http://icecast.radiofrance.fr/fip-hifi.aac"), 7, "fip");
    CHECK_INT(rf_station_id("https://icecast.radiofrance.fr/franceinter-midfi.mp3"), 1,
              "franceinter");
    CHECK_INT(rf_station_id("https://icecast.radiofrance.fr/unknownslug-midfi.mp3"), 0,
              "unknown slug");
    CHECK_INT(rf_station_id("http://ouifm3.ice.infomaniak.ch/ouifm3.mp3"), 0, "non RF host");
    CHECK_INT(rf_station_id("https://radiofrance.fr.evil.com/fip-midfi.mp3"), 0,
              "host suffix trick");
    CHECK_INT(rf_station_id("https://evilradiofrance.fr/fip-midfi.mp3"), 0,
              "host prefix trick");
    CHECK_INT(rf_station_id("https://icecast.radiofrance.fr/"), 0, "no path segment");
    CHECK_INT(rf_station_id("https://ICECAST.RADIOFRANCE.FR/FIPROCK-midfi.mp3"), 64,
              "case insensitive");
    CHECK_INT(rf_station_id("https://icecast.radiofrance.fr/fip-midfi.mp3?id=x"), 7,
              "query string");
}

int main(void)
{
    test_live_fip_electro();
    test_live_france_inter();
    test_song_uuid_but_no_second_line();
    test_escaped_quote_and_backslash();
    test_unicode_and_surrogate_pair();
    test_first_line_null();
    test_first_line_missing();
    test_now_missing();
    test_truncated_mid_string();
    test_end_time_missing();
    test_out_size_truncation();
    test_station_id();
    if (g_fail == 0) {
        printf("OK: all rf_meta host tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", g_fail);
    return 1;
}
