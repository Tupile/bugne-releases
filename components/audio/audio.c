// audio: shared output layer over ES8311 (esp_codec_dev) and the I2S std driver.
#include "audio.h"
#include "audio_arbiter.h"
#include "board_pins.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "audio";

// Default format used to init I2S. esp_codec_dev reconfigures it on open().
#define AUDIO_DEFAULT_RATE   44100
#define AUDIO_I2S_PORT       I2S_NUM_0

// Amp (FM8002E on IO1, active low): low = on, high = muted. Keep it muted while
// the codec opens/closes so the DAC power transient is not amplified into a
// speaker pop. Unmute only after the DAC output has settled.
#define AMP_SETTLE_MS  80
static inline void amp_mute(bool mute) { gpio_set_level(BOARD_AMP_EN_GPIO, mute ? 1 : 0); }

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static esp_codec_dev_handle_t s_dev;
static esp_codec_dev_handle_t s_rec_dev;       // mic capture, created lazily
static i2c_master_bus_handle_t s_i2c_bus;      // kept for the lazy record setup
static bool s_open;
static volatile bool s_paused;  // pause playback without tearing down the stream
static int s_volume = 70;  // 0..100, applied on each open so playback is audible

// Lazy codec close: audio_close keeps the DAC powered for a short window so an
// immediate next track (folder/episode auto-advance) reopens with no codec
// power cycle and no 80 ms settle gap. The amp stays muted through the window
// (and I2S auto_clear feeds zeros), so the gap is silent; the fast reopen
// unmutes only once the first PCM of the new track has been written.
#define CODEC_IDLE_CLOSE_MS 2000
static SemaphoreHandle_t s_lock;          // guards the codec open/close state
static esp_timer_handle_t s_close_timer;
static bool s_codec_open;                 // physical codec state (>= s_open)
static esp_codec_dev_sample_info_t s_codec_fs;  // format the codec is opened at
static volatile bool s_unmute_pending;    // fast reopen: unmute on first write

// Lazy close fires only when nothing reopened the codec within the window.
static void codec_close_timer_cb(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_open && s_codec_open) {
        esp_codec_dev_close(s_dev);
        s_codec_open = false;
    }
    xSemaphoreGive(s_lock);
}

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    // Send zeros when the decoder stops feeding instead of replaying the last DMA
    // buffer. Without this, the end of a track (no next one to reopen the codec)
    // leaves the I2S looping its final buffer as a continuous tone until stop.
    chan_cfg.auto_clear = true;
    // TX and RX are allocated together as a duplex pair: they share the codec's
    // single BCLK/WS pin set, and esp_codec_dev relies on the pairing to keep
    // the clocks coherent. RX (mic, tuner) stays disabled until first use.
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG, "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_DEFAULT_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK_GPIO,
            .bclk = BOARD_I2S_BCLK_GPIO,
            .ws   = BOARD_I2S_WS_GPIO,
            .dout = BOARD_I2S_DOUT_GPIO,
            .din  = BOARD_I2S_DIN_GPIO,
            .invert_flags = {0},
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s init std failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "i2s init rx failed");
    // The channels are enabled by esp_codec_dev on open, not here.
    return ESP_OK;
}

esp_err_t audio_init(i2c_master_bus_handle_t i2c_bus)
{
    s_i2c_bus = i2c_bus;
    ESP_RETURN_ON_ERROR(init_i2s(), TAG, "i2s setup failed");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = AUDIO_I2S_PORT,
        .tx_handle = s_tx,
        .rx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "new i2s data_if failed");

    // ES8311 control over the shared I2C bus created by the board component.
    // esp_codec_dev expects the 8-bit I2C address (it right-shifts internally),
    // so pass the 7-bit board address shifted left by one (0x18 -> 0x30).
    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr = BOARD_ES8311_ADDR << 1,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "new i2c ctrl_if failed");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if, ESP_FAIL, TAG, "new gpio_if failed");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,          // amp enable (IO1) is driven directly below, not by the codec
        .master_mode = false,  // ESP32-S3 is the I2S master, codec is slave
        .use_mclk = true,      // MCLK is wired (IO4)
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "new es8311 failed");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_dev, ESP_FAIL, TAG, "codec_dev_new failed");

    // FM8002E amp enable (IO1), driven directly. Start muted: it is unmuted only
    // during active playback (see audio_open), which avoids the open/close pop and
    // idle hiss.
    gpio_config_t amp = {
        .pin_bit_mask = 1ULL << BOARD_AMP_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&amp);
    amp_mute(true);

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "audio lock alloc failed");
    const esp_timer_create_args_t tca = {
        .callback = codec_close_timer_cb,
        .name = "codec_close",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&tca, &s_close_timer), TAG, "close timer failed");

    ESP_RETURN_ON_ERROR(audio_arbiter_init(), TAG, "arbiter init failed");

    ESP_LOGI(TAG, "audio output ready (ES8311 on shared I2C, I2S port %d, amp IO%d)",
             AUDIO_I2S_PORT, BOARD_AMP_EN_GPIO);
    return ESP_OK;
}

esp_err_t audio_open(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = bits_per_sample,
        .channel = channels,
        .sample_rate = sample_rate,
    };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_timer_stop(s_close_timer);  // cancel a pending lazy close
    if (s_codec_open && s_codec_fs.sample_rate == fs.sample_rate &&
        s_codec_fs.bits_per_sample == fs.bits_per_sample &&
        s_codec_fs.channel == fs.channel) {
        // Fast reopen within the lazy-close window, same format: the DAC never
        // powered down, so no settle is needed. Stay muted until the first PCM
        // of the new track is in the DMA (audio_write unmutes), so nothing
        // stale or transient is amplified.
        s_open = true;
        s_paused = false;
        esp_codec_dev_set_out_vol(s_dev, s_volume);
        s_unmute_pending = true;
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (s_codec_open) {  // format change: real close before reopening
        esp_codec_dev_close(s_dev);
        s_codec_open = false;
    }
    amp_mute(true);  // keep the speaker muted across the codec power-up transient
    int r = esp_codec_dev_open(s_dev, &fs);
    if (r != ESP_CODEC_DEV_OK) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "codec open failed (%d)", r);
        return ESP_FAIL;
    }
    s_codec_open = true;
    s_codec_fs = fs;
    s_open = true;
    s_paused = false;  // each new stream starts playing
    s_unmute_pending = false;
    esp_codec_dev_set_out_vol(s_dev, s_volume);  // codec volume is per-open
    vTaskDelay(pdMS_TO_TICKS(AMP_SETTLE_MS));  // let the DAC settle, then unmute
    amp_mute(false);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t audio_write(const void *pcm, size_t bytes)
{
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    // While paused, hold this PCM chunk and keep the codec fed with silence so
    // the I2S DMA does not underrun and buzz. The decode loop blocks here, which
    // also stalls the byte source (a live stream simply resumes from live).
    while (s_paused && s_open) {
        int16_t silence[128] = {0};
        esp_codec_dev_write(s_dev, silence, sizeof(silence));
    }
    int r = esp_codec_dev_write(s_dev, (void *)pcm, (int)bytes);
    if (r == ESP_CODEC_DEV_OK && s_unmute_pending) {
        s_unmute_pending = false;
        amp_mute(false);  // fast reopen: first real PCM is in the DMA, unmute now
    }
    return r == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

void audio_set_paused(bool paused)
{
    s_paused = paused;
}

bool audio_is_paused(void)
{
    return s_paused;
}

int audio_get_volume(void)
{
    return s_volume;
}

bool audio_is_active(void)
{
    return s_open;
}

void audio_output_off(void)
{
    amp_mute(true);  // instant hardware silence; buffered PCM drains into a muted amp
}

esp_err_t audio_close(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_open) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    s_open = false;
    s_unmute_pending = false;
    amp_mute(true);  // instant silence; the DAC power-down is deferred
    // Lazy close: keep the codec powered for a short window so an immediate
    // next track reopens gap-free (see codec_close_timer_cb).
    esp_timer_stop(s_close_timer);
    esp_timer_start_once(s_close_timer, (uint64_t)CODEC_IDLE_CLOSE_MS * 1000);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

// Microphone capture path (instrument tuner). A second esp_codec_dev instance
// drives the ES8311 ADC half: the vendored es8311 driver pairs the ADC and DAC
// instances of one chip (paired_8311), and the I2S data layer pairs the RX and
// TX channels of the port, so the playback device above is untouched. Created
// lazily: most sessions never open the mic.
#define RECORD_MIC_GAIN_DB 42.0f  // analog mic gain; 42 is the ES8311 PGA max
                                  // (30 dB lacked sensitivity on a real guitar)

static esp_err_t record_dev_create(void)
{
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = AUDIO_I2S_PORT,
        .tx_handle = NULL,
        .rx_handle = s_rx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "rec: new i2s data_if failed");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr = BOARD_ES8311_ADDR << 1,
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "rec: new i2c ctrl_if failed");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if, ESP_FAIL, TAG, "rec: new gpio_if failed");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin = -1,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,  // the onboard mic is analog
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "rec: new es8311 adc failed");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_rec_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_rec_dev, ESP_FAIL, TAG, "rec: codec_dev_new failed");
    return ESP_OK;
}

esp_err_t audio_record_open(uint32_t sample_rate)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Flush a pending lazy close (audio_close defers the real codec close by
    // 2 s): the still-open playback channel would otherwise make the I2S
    // layer refuse the record rate as a full-duplex conflict.
    esp_timer_stop(s_close_timer);
    if (!s_open && s_codec_open) {
        esp_codec_dev_close(s_dev);
        s_codec_open = false;
    }
    if (!s_rec_dev && record_dev_create() != ESP_OK) {
        s_rec_dev = NULL;
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .sample_rate = sample_rate,
    };
    int r = esp_codec_dev_open(s_rec_dev, &fs);
    if (r != ESP_CODEC_DEV_OK) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "record open failed (%d)", r);
        return ESP_FAIL;
    }
    esp_codec_dev_set_in_gain(s_rec_dev, RECORD_MIC_GAIN_DB);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "mic capture open (%lu Hz)", (unsigned long)sample_rate);
    return ESP_OK;
}

esp_err_t audio_record_read(void *pcm, size_t bytes)
{
    if (!s_rec_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    int r = esp_codec_dev_read(s_rec_dev, pcm, (int)bytes);
    return r == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

void audio_record_close(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_rec_dev) {
        esp_codec_dev_close(s_rec_dev);
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "mic capture closed");
}

// Volume ceiling (child-ear protection). Every volume request, from the UI
// slider, the web API or Music Assistant, funnels through audio_set_volume,
// so clamping here covers them all. Set from config by the ui component.
static int s_volume_limit = 100;

esp_err_t audio_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > s_volume_limit) volume = s_volume_limit;
    s_volume = volume;
    if (!s_open) {
        return ESP_OK;  // taken into account on the next open
    }
    int r = esp_codec_dev_set_out_vol(s_dev, volume);
    return r == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

void audio_set_volume_limit(int limit)
{
    if (limit < 1) limit = 1;
    if (limit > 100) limit = 100;
    s_volume_limit = limit;
    if (s_volume > limit) audio_set_volume(limit);  // lower a live volume too
}

int audio_get_volume_limit(void)
{
    return s_volume_limit;
}
