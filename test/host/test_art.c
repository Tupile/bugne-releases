#include "art.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "jpeg_decoder.h"
#include "esp_log.h"

// Mock esp_jpeg variables to control behavior
esp_err_t mock_esp_jpeg_get_image_info_ret = ESP_OK;
esp_jpeg_image_output_t mock_esp_jpeg_info;

esp_err_t mock_esp_jpeg_decode_ret = ESP_OK;
esp_jpeg_image_output_t mock_esp_jpeg_decode_info;
uint8_t *mock_esp_jpeg_decode_outbuf = NULL;
uint32_t mock_esp_jpeg_decode_outbuf_size = 0;

int mock_get_info_calls = 0;
esp_jpeg_image_scale_t mock_last_scale = JPEG_IMAGE_SCALE_0;

esp_err_t esp_jpeg_get_image_info(const esp_jpeg_image_cfg_t *cfg, esp_jpeg_image_output_t *out) {
    mock_get_info_calls++;
    if (cfg) mock_last_scale = cfg->out_scale;
    if (out) {
        *out = mock_esp_jpeg_info;
    }
    return mock_esp_jpeg_get_image_info_ret;
}

esp_err_t esp_jpeg_decode(const esp_jpeg_image_cfg_t *cfg, esp_jpeg_image_output_t *out) {
    if (out) {
        *out = mock_esp_jpeg_decode_info;
    }
    if (cfg && cfg->outbuf) {
        mock_esp_jpeg_decode_outbuf = cfg->outbuf;
        mock_esp_jpeg_decode_outbuf_size = cfg->outbuf_size;
        // write some dummy data
        if (cfg->outbuf_size > 0) {
            memset(cfg->outbuf, 0xAA, cfg->outbuf_size);
        }
    }
    return mock_esp_jpeg_decode_ret;
}

// Include art.c directly to access static state for testing, same as other tests
#include "../../components/decode/art.c"

static int g_fail;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } \
} while (0)

static void reset_mocks(void) {
    mock_esp_jpeg_get_image_info_ret = ESP_OK;
    memset(&mock_esp_jpeg_info, 0, sizeof(mock_esp_jpeg_info));
    mock_esp_jpeg_decode_ret = ESP_OK;
    memset(&mock_esp_jpeg_decode_info, 0, sizeof(mock_esp_jpeg_decode_info));
    mock_esp_jpeg_decode_outbuf = NULL;
    mock_esp_jpeg_decode_outbuf_size = 0;
    mock_get_info_calls = 0;
    mock_last_scale = JPEG_IMAGE_SCALE_0;
    art_clear();
}

static void take_and_free(void) {
    uint8_t *px = NULL;
    if (art_take(&px, NULL, NULL) && px) {
        free(px);
    }
}

static void test_art_set_jpeg_invalid_args(void) {
    reset_mocks(); take_and_free();
    uint8_t dummy_jpeg[10];
    CHECK(!art_set_jpeg(NULL, 10), "art_set_jpeg should fail on NULL jpeg");
    CHECK(!art_set_jpeg(dummy_jpeg, 3), "art_set_jpeg should fail on len < 4");
    CHECK(!art_set_jpeg(dummy_jpeg, ART_MAX_JPEG + 1), "art_set_jpeg should fail on len > ART_MAX_JPEG");

    // get_image_info fails
    mock_esp_jpeg_get_image_info_ret = ESP_FAIL;
    CHECK(!art_set_jpeg(dummy_jpeg, 10), "art_set_jpeg should fail when get_image_info fails");
}

static void test_art_set_jpeg_scale(void) {
    reset_mocks(); take_and_free();
    uint8_t dummy_jpeg[10];

    // Scale 1/2
    mock_esp_jpeg_info.width = ART_BOX * 4;
    mock_esp_jpeg_info.height = ART_BOX * 4;
    mock_esp_jpeg_info.output_len = 100 * 100 * 2;
    mock_esp_jpeg_decode_info.width = 100;
    mock_esp_jpeg_decode_info.height = 100;
    mock_esp_jpeg_decode_info.output_len = 100 * 100 * 2;
    CHECK(art_set_jpeg(dummy_jpeg, 10), "art_set_jpeg should succeed");
    CHECK(mock_last_scale == JPEG_IMAGE_SCALE_1_2, "should select scale 1/2");
    take_and_free();

    reset_mocks(); take_and_free();
    // Scale 1/4
    mock_esp_jpeg_info.width = ART_BOX * 8;
    mock_esp_jpeg_info.height = ART_BOX * 8;
    mock_esp_jpeg_info.output_len = 100 * 100 * 2;
    mock_esp_jpeg_decode_info.width = 100;
    mock_esp_jpeg_decode_info.height = 100;
    mock_esp_jpeg_decode_info.output_len = 100 * 100 * 2;
    CHECK(art_set_jpeg(dummy_jpeg, 10), "art_set_jpeg should succeed");
    CHECK(mock_last_scale == JPEG_IMAGE_SCALE_1_4, "should select scale 1/4");
    take_and_free();

    reset_mocks(); take_and_free();
    // Scale 1/8
    mock_esp_jpeg_info.width = ART_BOX * 16;
    mock_esp_jpeg_info.height = ART_BOX * 16;
    mock_esp_jpeg_info.output_len = 100 * 100 * 2;
    mock_esp_jpeg_decode_info.width = 100;
    mock_esp_jpeg_decode_info.height = 100;
    mock_esp_jpeg_decode_info.output_len = 100 * 100 * 2;
    CHECK(art_set_jpeg(dummy_jpeg, 10), "art_set_jpeg should succeed");
    CHECK(mock_last_scale == JPEG_IMAGE_SCALE_1_8, "should select scale 1/8");
    take_and_free();
}

static void test_art_set_jpeg_too_large(void) {
    reset_mocks(); take_and_free();
    uint8_t dummy_jpeg[10];

    mock_esp_jpeg_info.width = 100;
    mock_esp_jpeg_info.height = 100;
    // Larger than (ART_BOX * 2) * (ART_BOX * 2) * 2
    mock_esp_jpeg_info.output_len = (size_t)(ART_BOX * 2) * (ART_BOX * 2) * 2 + 1;

    CHECK(!art_set_jpeg(dummy_jpeg, 10), "art_set_jpeg should fail if output_len is too large");
}

static void test_art_set_jpeg_decode_fail(void) {
    reset_mocks(); take_and_free();
    uint8_t dummy_jpeg[10];

    mock_esp_jpeg_info.width = 100;
    mock_esp_jpeg_info.height = 100;
    mock_esp_jpeg_info.output_len = 100 * 100 * 2;

    mock_esp_jpeg_decode_ret = ESP_FAIL;
    CHECK(!art_set_jpeg(dummy_jpeg, 10), "art_set_jpeg should fail if decode fails");
}

static void test_art_gen_counter(void) {
    reset_mocks(); take_and_free();
    uint32_t gen1 = art_gen();
    art_clear();
    uint32_t gen2 = art_gen();
    CHECK(gen2 == gen1 + 1, "art_clear should bump generation");

    uint8_t dummy_jpeg[10];
    mock_esp_jpeg_info.width = 100;
    mock_esp_jpeg_info.height = 100;
    mock_esp_jpeg_info.output_len = 100 * 100 * 2;
    mock_esp_jpeg_decode_info.width = 100;
    mock_esp_jpeg_decode_info.height = 100;
    mock_esp_jpeg_decode_info.output_len = 100 * 100 * 2;
    CHECK(art_set_jpeg(dummy_jpeg, 10), "art_set_jpeg should succeed");
    uint32_t gen3 = art_gen();
    CHECK(gen3 == gen2 + 1, "art_set_jpeg should bump generation");
    take_and_free();
}

static void test_art_set_jpeg_success_and_take(void) {
    reset_mocks(); take_and_free();
    uint8_t dummy_jpeg[10];

    // We want output width/height to be small enough so it doesn't trigger resizing branch just yet
    mock_esp_jpeg_info.width = 100;
    mock_esp_jpeg_info.height = 100;
    mock_esp_jpeg_info.output_len = 100 * 100 * 2;

    mock_esp_jpeg_decode_info.width = 100;
    mock_esp_jpeg_decode_info.height = 100;
    mock_esp_jpeg_decode_info.output_len = 100 * 100 * 2;

    CHECK(art_set_jpeg(dummy_jpeg, 10), "art_set_jpeg should succeed");

    uint8_t *px = NULL;
    uint16_t w = 0, h = 0;
    CHECK(art_take(&px, &w, &h), "art_take should return true after set");
    CHECK(px != NULL, "px should not be NULL");
    CHECK(w == 100, "w should be 100");
    CHECK(h == 100, "h should be 100");

    if (px) free(px);

    CHECK(!art_take(&px, &w, &h), "art_take should return false after already taken");
}

static void test_art_set_jpeg_resize(void) {
    reset_mocks(); take_and_free();
    uint8_t dummy_jpeg[10];

    // Make size larger than ART_BOX so it triggers resizing
    mock_esp_jpeg_info.width = 200;
    mock_esp_jpeg_info.height = 100;
    mock_esp_jpeg_info.output_len = 200 * 100 * 2;

    mock_esp_jpeg_decode_info.width = 200;
    mock_esp_jpeg_decode_info.height = 100;
    mock_esp_jpeg_decode_info.output_len = 200 * 100 * 2;

    CHECK(art_set_jpeg(dummy_jpeg, 10), "art_set_jpeg should succeed and resize");

    uint8_t *px = NULL;
    uint16_t w = 0, h = 0;
    CHECK(art_take(&px, &w, &h), "art_take should return true");
    CHECK(w == ART_BOX, "w should be scaled to ART_BOX (%d), got %d", ART_BOX, w);
    CHECK(h == ART_BOX / 2, "h should be scaled proportionally");

    if (px) free(px);
}

static void test_art_clear_without_init(void) {
    reset_mocks(); take_and_free();
    s_lock = NULL;
    art_clear(); // this shouldn't fail and it actually creates the lock since art_clear calls lock_get()
    CHECK(s_lock != NULL, "art_clear creates lock if it doesn't exist");
}

int main(void) {
    test_art_set_jpeg_invalid_args();
    test_art_set_jpeg_scale();
    test_art_set_jpeg_too_large();
    test_art_set_jpeg_decode_fail();
    test_art_gen_counter();
    test_art_set_jpeg_success_and_take();
    test_art_set_jpeg_resize();
    test_art_clear_without_init();
    if (g_fail) {
        printf("test_art: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("test_art: all tests passed\n");
    return 0;
}
