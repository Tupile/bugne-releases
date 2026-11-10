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

// Count live allocations made by art.c, so a replaced or dropped bitmap that
// is not freed shows up. The stub header is included first so its inline
// functions are defined before the names are redirected.
#include "esp_heap_caps.h"
static int g_live;
static void *cnt_malloc(size_t n) { void *p = malloc(n); if (p) g_live++; return p; }
static void cnt_free(void *p) { if (p) g_live--; free(p); }
#define heap_caps_malloc(n, caps) cnt_malloc(n)
#define heap_caps_free(p) cnt_free(p)

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
    // The Music Assistant slot: nothing current, nothing staged.
    art_ss_stage_none();
    art_ss_commit();
}

static void take_and_free(void) {
    uint8_t *px = NULL;
    if (art_take(&px, NULL, NULL) && px) {
        cnt_free(px);
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

    if (px) cnt_free(px);

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

    if (px) cnt_free(px);
}

static void test_art_clear_without_init(void) {
    reset_mocks(); take_and_free();
    s_lock = NULL;
    art_clear(); // this shouldn't fail and it actually creates the lock since art_clear calls lock_get()
    CHECK(s_lock != NULL, "art_clear creates lock if it doesn't exist");
}

// ---- Music Assistant slot (staged, then committed at display time) ----

static void mock_cover(int w, int h) {
    mock_esp_jpeg_get_image_info_ret = ESP_OK;
    mock_esp_jpeg_decode_ret = ESP_OK;
    mock_esp_jpeg_info.width = w;
    mock_esp_jpeg_info.height = h;
    mock_esp_jpeg_info.output_len = (size_t)w * h * 2;
    mock_esp_jpeg_decode_info = mock_esp_jpeg_info;
}

static bool ss_take_and_free(uint16_t *w) {
    uint8_t *px = NULL;
    uint16_t ww = 0, hh = 0;
    bool got = art_ss_take(&px, &ww, &hh, NULL);
    if (got) cnt_free(px);
    if (w) *w = ww;
    return got;
}

static void test_ss_stage_then_commit(void) {
    reset_mocks(); take_and_free(); ss_take_and_free(NULL);
    uint8_t jpeg[10];
    int base = g_live;
    uint32_t gen = art_ss_gen();

    mock_cover(100, 100);
    art_ss_stage_jpeg(jpeg, sizeof(jpeg));
    CHECK(art_ss_gen() == gen, "a staged cover does not bump the generation");
    CHECK(!ss_take_and_free(NULL), "a staged cover is not visible");
    CHECK(g_live == base + 1, "one staged bitmap held, live %d", g_live - base);

    art_ss_commit();
    CHECK(art_ss_gen() == gen + 1, "commit bumps the generation");
    uint16_t w = 0;
    CHECK(ss_take_and_free(&w) && w == 100, "commit publishes the staged cover, w %d", w);
    CHECK(!ss_take_and_free(NULL), "taken once");
    CHECK(g_live == base, "nothing left after take, live %d", g_live - base);
}

static void test_ss_restage_replaces(void) {
    reset_mocks(); take_and_free(); ss_take_and_free(NULL);
    uint8_t jpeg[10];
    int base = g_live;
    mock_cover(100, 100);
    art_ss_stage_jpeg(jpeg, sizeof(jpeg));
    mock_cover(80, 80);
    art_ss_stage_jpeg(jpeg, sizeof(jpeg));
    CHECK(g_live == base + 1, "the older staged bitmap is freed, live %d", g_live - base);
    art_ss_commit();
    uint16_t w = 0;
    CHECK(ss_take_and_free(&w) && w == 80, "the newest staged cover wins, w %d", w);
    CHECK(g_live == base, "no leak, live %d", g_live - base);
}

static void test_ss_refused_image_clears(void) {
    reset_mocks(); take_and_free(); ss_take_and_free(NULL);
    uint8_t jpeg[10];
    int base = g_live;
    mock_cover(100, 100);
    art_ss_stage_jpeg(jpeg, sizeof(jpeg));
    art_ss_commit();                      // a good cover is current, not taken yet
    uint32_t gen = art_ss_gen();

    // The next image cannot be decoded: the display that follows must remove
    // the previous cover, not keep it.
    mock_esp_jpeg_decode_ret = ESP_FAIL;
    art_ss_stage_jpeg(jpeg, sizeof(jpeg));
    art_ss_commit();
    CHECK(art_ss_gen() == gen + 1, "commit after a refused image bumps the generation");
    CHECK(!ss_take_and_free(NULL), "the previous cover is gone");
    CHECK(g_live == base, "no leak, live %d", g_live - base);

    // Same for a format we do not decode, and for bad arguments.
    mock_cover(100, 100);
    art_ss_stage_jpeg(jpeg, sizeof(jpeg));
    art_ss_commit();
    art_ss_stage_none();
    art_ss_commit();
    CHECK(!ss_take_and_free(NULL), "stage_none then commit clears");
    art_ss_stage_jpeg(jpeg, sizeof(jpeg));
    art_ss_commit();
    art_ss_stage_jpeg(NULL, 10);
    art_ss_commit();
    CHECK(!ss_take_and_free(NULL), "a NULL image then commit clears");
    CHECK(g_live == base, "no leak, live %d", g_live - base);
}

static void test_ss_clear_keeps_staged(void) {
    reset_mocks(); take_and_free(); ss_take_and_free(NULL);
    uint8_t jpeg[10];
    int base = g_live;
    mock_cover(100, 100);
    art_ss_stage_jpeg(jpeg, sizeof(jpeg));
    art_ss_commit();                      // current: 100
    mock_cover(90, 90);
    art_ss_stage_jpeg(jpeg, sizeof(jpeg));  // staged for the next track: 90
    uint32_t gen = art_ss_gen();

    art_ss_clear();
    CHECK(art_ss_gen() == gen + 1, "clear bumps the generation");
    CHECK(!ss_take_and_free(NULL), "clear drops the current cover");
    CHECK(g_live == base + 1, "the staged cover survives a clear, live %d", g_live - base);
    art_ss_commit();
    uint16_t w = 0;
    CHECK(ss_take_and_free(&w) && w == 90, "the staged cover is still committed, w %d", w);
    CHECK(g_live == base, "no leak, live %d", g_live - base);
}

static void test_slots_are_independent(void) {
    reset_mocks(); take_and_free(); ss_take_and_free(NULL);
    uint8_t jpeg[10];
    int base = g_live;

    // Local activity never reaches the Music Assistant slot.
    mock_cover(100, 100);
    art_ss_stage_jpeg(jpeg, sizeof(jpeg));
    art_ss_commit();
    uint32_t ss_gen = art_ss_gen();
    mock_cover(60, 60);
    CHECK(art_set_jpeg(jpeg, sizeof(jpeg)), "local set");
    art_clear();
    CHECK(art_set_jpeg(jpeg, sizeof(jpeg)), "local set again");
    CHECK(art_ss_gen() == ss_gen, "local set and clear leave the other generation alone");

    // Music Assistant activity never reaches the local slot.
    uint32_t gen = art_gen();
    mock_cover(70, 70);
    art_ss_stage_jpeg(jpeg, sizeof(jpeg));
    art_ss_commit();
    art_ss_clear();
    art_ss_stage_none();
    art_ss_commit();
    CHECK(art_gen() == gen, "the other slot leaves the local generation alone");
    uint8_t *px = NULL;
    uint16_t w = 0;
    CHECK(art_take(&px, &w, NULL) && w == 60, "the local cover is still there, w %d", w);
    cnt_free(px);
    CHECK(!ss_take_and_free(NULL), "and the other slot is empty");
    CHECK(g_live == base, "no leak, live %d", g_live - base);
}

// Pins the contract only (the value is the one read under the lock); the
// interleaving it protects against cannot be staged in a single-threaded test.
static void test_ss_take_reports_generation(void) {
    reset_mocks(); take_and_free(); ss_take_and_free(NULL);
    uint8_t jpeg[10];
    mock_cover(100, 100);
    art_ss_stage_jpeg(jpeg, sizeof(jpeg));
    art_ss_commit();
    uint8_t *px = NULL;
    uint32_t gen = 0;
    CHECK(art_ss_take(&px, NULL, NULL, &gen) && gen == art_ss_gen(), "take reports the generation it read");
    cnt_free(px);
    art_ss_clear();
    gen = 0;
    CHECK(!art_ss_take(&px, NULL, NULL, &gen) && gen == art_ss_gen(), "an empty take reports it too");
}

static void test_ss_edges(void) {
    reset_mocks(); take_and_free(); ss_take_and_free(NULL);
    uint8_t jpeg[10];
    int base = g_live;
    // The size asked from the server: decoded 1:1, no scale step, no resample.
    mock_cover(ART_BOX, ART_BOX);
    art_ss_stage_jpeg(jpeg, sizeof(jpeg));
    CHECK(mock_last_scale == JPEG_IMAGE_SCALE_0, "a 112 px cover is decoded at full size");
    art_ss_commit();
    uint16_t w = 0;
    CHECK(ss_take_and_free(&w) && w == ART_BOX, "and stays 112 px wide, got %d", w);

    // Commit twice with nothing staged in between, and clear with nothing
    // current: the slot stays empty and each call still bumps the generation.
    uint32_t gen = art_ss_gen();
    art_ss_commit();
    art_ss_commit();
    art_ss_clear();
    CHECK(art_ss_gen() == gen + 3, "three bumps, got %u", (unsigned)(art_ss_gen() - gen));
    CHECK(!ss_take_and_free(NULL), "still empty");

    // NULL out-pointers are allowed; the bitmap then stays unreachable to the
    // caller, so only use that form on an empty slot.
    CHECK(!art_ss_take(NULL, NULL, NULL, NULL), "take with NULL pointers on an empty slot");
    CHECK(g_live == base, "no leak, live %d", g_live - base);
}

static void test_art_init(void) {
    reset_mocks(); take_and_free(); ss_take_and_free(NULL);
    s_lock = NULL;
    art_init();
    CHECK(s_lock != NULL, "art_init creates the lock");
    SemaphoreHandle_t first = s_lock;
    art_init();
    CHECK(s_lock == first, "art_init is idempotent");
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
    test_ss_stage_then_commit();
    test_ss_restage_replaces();
    test_ss_refused_image_clears();
    test_ss_clear_keeps_staged();
    test_slots_are_independent();
    test_ss_take_reports_generation();
    test_ss_edges();
    test_art_init();
    if (g_fail) {
        printf("test_art: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("test_art: all tests passed\n");
    return 0;
}
