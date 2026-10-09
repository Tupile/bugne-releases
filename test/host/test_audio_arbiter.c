// Host unit tests for the audio source arbiter (audio_arbiter.c).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#define ESP_ERR_INVALID_STATE 0x103

#include "../../components/audio/audio_arbiter.c"

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void test_uninitialized(void)
{
    // Ensure it's uninitialized
    s_lock = NULL;
    s_active = AUDIO_SOURCE_NONE;

    esp_err_t ret = audio_arbiter_acquire(AUDIO_SOURCE_SD);
    CHECK(ret == ESP_ERR_INVALID_STATE, "acquire when uninitialized returns INVALID_STATE");
    CHECK(audio_arbiter_active() == AUDIO_SOURCE_NONE, "active is NONE when uninitialized");
}

static void test_acquire_success(void)
{
    esp_err_t init_ret = audio_arbiter_init();
    CHECK(init_ret == ESP_OK, "init returns ESP_OK");
    CHECK(s_lock != NULL, "lock is created");

    esp_err_t ret = audio_arbiter_acquire(AUDIO_SOURCE_SD);
    CHECK(ret == ESP_OK, "acquire returns ESP_OK");
    CHECK(audio_arbiter_active() == AUDIO_SOURCE_SD, "active is SD");
}

static void test_acquire_reentrant(void)
{
    esp_err_t ret = audio_arbiter_acquire(AUDIO_SOURCE_SD);
    CHECK(ret == ESP_OK, "reentrant acquire returns ESP_OK");
    CHECK(audio_arbiter_active() == AUDIO_SOURCE_SD, "active is still SD");
}

static void test_acquire_conflict(void)
{
    esp_err_t ret = audio_arbiter_acquire(AUDIO_SOURCE_STREAM);
    CHECK(ret == ESP_ERR_INVALID_STATE, "conflict acquire returns INVALID_STATE");
    CHECK(audio_arbiter_active() == AUDIO_SOURCE_SD, "active remains SD");
}

static void test_release_and_acquire(void)
{
    audio_arbiter_release(AUDIO_SOURCE_SD);
    CHECK(audio_arbiter_active() == AUDIO_SOURCE_NONE, "active is NONE after release");

    esp_err_t ret = audio_arbiter_acquire(AUDIO_SOURCE_STREAM);
    CHECK(ret == ESP_OK, "acquire STREAM returns ESP_OK");
    CHECK(audio_arbiter_active() == AUDIO_SOURCE_STREAM, "active is STREAM");
}

static void test_release_wrong_source(void)
{
    audio_arbiter_release(AUDIO_SOURCE_SD);
    CHECK(audio_arbiter_active() == AUDIO_SOURCE_STREAM, "active remains STREAM after wrong release");
}

int main(void)
{
    test_uninitialized();
    test_acquire_success();
    test_acquire_reentrant();
    test_acquire_conflict();
    test_release_and_acquire();
    test_release_wrong_source();

    if (g_fail == 0) {
        printf("OK: all audio_arbiter host tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", g_fail);
    return 1;
}
