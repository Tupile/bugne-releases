// Host unit tests for the localization logic (lang.c).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "lang.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define CHECK_STR(got, want, what) \
    CHECK(strcmp((got), (want)) == 0, "%s: got \"%s\", want \"%s\"", (what), (got), (want))

#define CHECK_INT(got, want, what) \
    CHECK((got) == (want), "%s: got %d, want %d", (what), (int)(got), (int)(want))

static void test_default_language(void)
{
    // The default language should be English
    lang_set_code(NULL); // Reset to default state effectively if we hadn't set anything
    CHECK_STR(lang_code(), "en", "default lang code");
    CHECK_STR(T(STR_NOW_PLAYING), "Now playing", "default translation for STR_NOW_PLAYING");
}

static void test_set_language(void)
{
    lang_set_code("fr");
    CHECK_STR(lang_code(), "fr", "set lang code fr");
    CHECK_STR(T(STR_NOW_PLAYING), "Lecture en cours", "fr translation for STR_NOW_PLAYING");

    // Switch back
    lang_set_code("en");
    CHECK_STR(lang_code(), "en", "set lang code en");
    CHECK_STR(T(STR_NOW_PLAYING), "Now playing", "en translation for STR_NOW_PLAYING");
}

static void test_invalid_language(void)
{
    // Set to a valid one first
    lang_set_code("fr");

    // Invalid falls back to default (en)
    lang_set_code("xx");
    CHECK_STR(lang_code(), "en", "invalid code falls back to en");
    CHECK_STR(T(STR_NOW_PLAYING), "Now playing", "invalid code translation fallback");
}

static void test_null_language(void)
{
    lang_set_code("fr");

    // NULL is safely ignored, keeps current language
    lang_set_code(NULL);
    CHECK_STR(lang_code(), "fr", "NULL code keeps current lang");
}

static void test_out_of_bounds_ids(void)
{
    lang_set_code("en");

    CHECK_STR(T(-1), "", "negative ID returns empty string");
    CHECK_STR(T(STR__COUNT), "", "STR__COUNT returns empty string");
    CHECK_STR(T(STR__COUNT + 10), "", "out of bounds positive ID returns empty string");
}

static void test_metadata(void)
{
    size_t count = lang_count();
    CHECK(count >= 2, "at least 2 languages expected, got %zu", count);

    CHECK_STR(lang_code_at(0), "en", "first lang code");
    CHECK_STR(lang_name_at(0), "English", "first lang name");

    CHECK_STR(lang_code_at(1), "fr", "second lang code");
    CHECK_STR(lang_name_at(1), "Français", "second lang name");

    CHECK_STR(lang_code_at(count), "", "out of bounds lang_code_at returns empty string");
    CHECK_STR(lang_name_at(count), "", "out of bounds lang_name_at returns empty string");
}

int main(void)
{
    test_default_language();
    test_set_language();
    test_invalid_language();
    test_null_language();
    test_out_of_bounds_ids();
    test_metadata();

    if (g_fail == 0) {
        printf("OK: all lang host tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", g_fail);
    return 1;
}
