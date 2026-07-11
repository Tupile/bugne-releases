// source_sendspin: C++ wrapper around sendspin-cpp routing decoded PCM to the
// shared audio output. Exposes a C entry point (source_sendspin_init).
#include "source_sendspin.h"
#include "audio.h"
#include "audio_arbiter.h"
#include "board.h"
#include "net.h"
#include "config_store.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"  // xTaskCreateWithCaps
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mdns.h"

#include "sendspin/client.h"
#include "sendspin/player_role.h"
#include "sendspin/metadata_role.h"
#include "sendspin/controller_role.h"

using namespace sendspin;

static const char *TAG = "source_sendspin";

#define SENDSPIN_WS_PORT 8928
#define SENDSPIN_WS_PATH "/sendspin"

// Shared state read by the UI task and a queued transport command the sendspin
// task drains (-1 means none). s_active = audio stream playing (drives the
// pause/play button); s_session = MA is engaged with this player, stays true
// across a pause (when MA ends the audio stream) and drives the screen open/close.
static std::atomic<bool> s_active{false};
static std::atomic<bool> s_session{false};
static std::atomic<int> s_pending_cmd{-1};
static std::atomic<uint32_t> s_progress_ms{0};
static std::atomic<uint32_t> s_duration_ms{0};
static std::mutex s_meta_mutex;
static char s_title[96];
static char s_artist[96];

namespace {

// Routes the player role's decoded PCM and stream events to the audio layer.
class BugnePlayerListener : public PlayerRoleListener {
public:
    PlayerRole *player = nullptr;
    std::atomic<bool> active{false};
    uint32_t sample_rate = 44100;
    uint8_t channels = 2;
    uint8_t bits = 16;

    // Stable playout clock for the sync engine. Reporting wall-clock-at-hand-off
    // jitters with DMA back-pressure and makes the sync engine insert silence /
    // skip chunks (audible stutter). Instead anchor a timestamp at the first write
    // of a stream and advance it by exactly cumulative_frames / sample_rate, which
    // tracks real playout smoothly (I2S consumes at the sample rate and audio_write
    // back-pressures to it).
    int64_t  playout_anchor_us = 0;
    uint64_t playout_frames = 0;
    bool     playout_anchored = false;

    // Fires on the sync task thread.
    size_t on_audio_write(uint8_t *data, size_t length, uint32_t timeout_ms) override {
        (void)timeout_ms;
        if (!active.load()) {
            return 0;
        }
        if (audio_write(data, length) != ESP_OK) {
            return 0;
        }
        // Sync feedback on a stable playout clock (see members above): anchor once
        // at the first write, then report each chunk's finish time as
        // anchor + total_frames_so_far / sample_rate.
        uint32_t frame_bytes = channels * (bits / 8);
        if (frame_bytes && player) {
            uint32_t frames = (uint32_t)(length / frame_bytes);
            uint32_t rate = sample_rate ? sample_rate : 44100;
            if (!playout_anchored) {
                playout_anchor_us = esp_timer_get_time();
                playout_frames = 0;
                playout_anchored = true;
            }
            playout_frames += frames;
            int64_t finish = playout_anchor_us +
                             (int64_t)(playout_frames * 1000000ULL / rate);
            player->notify_audio_played(frames, finish);
        }
        return length;
    }

    // Fires on the main loop thread.
    void on_stream_start() override {
        if (!player) {
            return;
        }
        const ServerPlayerStreamObject &p = player->get_current_stream_params();
        sample_rate = p.sample_rate.value_or(44100);
        channels = p.channels.value_or(2);
        bits = p.bit_depth.value_or(16);
        if (audio_arbiter_acquire(AUDIO_SOURCE_SENDSPIN) != ESP_OK) {
            ESP_LOGW(TAG, "audio busy, cannot start sendspin stream");
            return;
        }
        if (audio_open(sample_rate, bits, channels) != ESP_OK) {
            audio_arbiter_release(AUDIO_SOURCE_SENDSPIN);
            return;
        }
        playout_anchored = false;  // re-anchor the playout clock for this stream
        active.store(true);
        s_active.store(true);   // pause/play button: stream is playing
        s_session.store(true);  // UI: engaged with MA, open the now-playing screen
        ESP_LOGI(TAG, "stream start %u Hz %u ch %u bit",
                 (unsigned)sample_rate, (unsigned)channels, (unsigned)bits);
    }

    void on_stream_end() override {
        s_active.store(false);  // UI: close the now-playing screen
        if (!active.exchange(false)) {
            return;
        }
        audio_close();
        audio_arbiter_release(AUDIO_SOURCE_SENDSPIN);
        ESP_LOGI(TAG, "stream end");
    }

    void on_volume_changed(uint8_t volume) override {
        audio_set_volume((int)volume * 100 / 255);  // 0..255 -> 0..100
    }
};

class BugneNetworkProvider : public SendspinNetworkProvider {
public:
    bool is_network_ready() override {
        return net_state() == NET_STATE_CONNECTED;
    }
};

// Receives track metadata from the server; stores the title for the UI.
class BugneMetadataListener : public MetadataRoleListener {
public:
    void on_metadata(const ServerMetadataStateObject &m) override {
        std::lock_guard<std::mutex> lock(s_meta_mutex);
        strlcpy(s_title, m.title.value_or(std::string()).c_str(), sizeof(s_title));
        strlcpy(s_artist, m.artist.value_or(std::string()).c_str(), sizeof(s_artist));
    }
    void on_metadata_clear() override {
        // Fires on disconnect: the session is over, close the screen.
        std::lock_guard<std::mutex> lock(s_meta_mutex);
        s_title[0] = '\0';
        s_artist[0] = '\0';
        s_session.store(false);
    }
};

SendspinClient *g_client = nullptr;
BugnePlayerListener g_player_listener;
BugneNetworkProvider g_net_provider;
BugneMetadataListener g_metadata_listener;
MetadataRole *g_metadata = nullptr;
ControllerRole *g_controller = nullptr;
std::string g_name;
bool g_mdns_done = false;
esp_err_t advertise_mdns(const char *instance);  // defined below

void sendspin_task(void *arg)
{
    (void)arg;
    bool warned = false;
    int last_vol = -1;      // -1 forces one publish so the boot volume reaches the role
    bool last_ext = false;  // matches the client's default SYNCHRONIZED state
    while (true) {
        g_client->loop();
        // Drain a UI-queued transport command on this task (sendspin-cpp is not
        // called from the UI task directly).
        int cmd = s_pending_cmd.exchange(-1);
        if (cmd >= 0 && g_controller) {
            switch ((sendspin_cmd_t)cmd) {
            case SENDSPIN_CMD_PLAY:  g_controller->send_command(SendspinControllerCommand::PLAY);  break;
            case SENDSPIN_CMD_PAUSE: g_controller->send_command(SendspinControllerCommand::PAUSE); break;
            case SENDSPIN_CMD_STOP:
                g_controller->send_command(SendspinControllerCommand::STOP);
                s_session.store(false);  // stop ends the session: close the screen
                break;
            case SENDSPIN_CMD_NEXT:     g_controller->send_command(SendspinControllerCommand::NEXT);     break;
            case SENDSPIN_CMD_PREVIOUS: g_controller->send_command(SendspinControllerCommand::PREVIOUS); break;
            }
        }
        // Report the device volume to the server, edge-triggered. Runs on this
        // task because publish_state is not thread-safe. Round to nearest so a
        // server-set volume echoes back unchanged (128 -> 50 -> 128).
        int vol = audio_get_volume();
        if (vol != last_vol && g_player_listener.player) {
            last_vol = vol;
            uint8_t v255 = (uint8_t)((vol * 255 + 50) / 100);
            g_player_listener.player->update_volume(v255);
            ESP_LOGI(TAG, "volume %d published to server (%u/255)", vol, (unsigned)v255);
        }
        // Report external_source when a non-Sendspin source owns the audio
        // output, so the server knows the player is busy.
        audio_source_t src = audio_arbiter_active();
        bool ext = (src != AUDIO_SOURCE_NONE && src != AUDIO_SOURCE_SENDSPIN);
        if (ext != last_ext) {
            last_ext = ext;
            g_client->update_state(ext ? SendspinClientState::EXTERNAL_SOURCE
                                       : SendspinClientState::SYNCHRONIZED);
            ESP_LOGI(TAG, "client state -> %s", ext ? "external_source" : "synchronized");
        }
        // Snapshot interpolated playback progress for the UI.
        if (g_metadata) {
            s_progress_ms.store(g_metadata->get_track_progress_ms());
            s_duration_ms.store(g_metadata->get_track_duration_ms());
        }
        // Advertise over mDNS once the station is up (mDNS is initialized by the
        // net component on connect, not in AP provisioning mode). Keep retrying
        // until it succeeds: only mark done on ESP_OK, so a premature attempt
        // before mdns_init does not permanently skip advertising.
        if (!g_mdns_done && net_state() == NET_STATE_CONNECTED) {
            esp_err_t err = advertise_mdns(g_name.c_str());
            if (err == ESP_OK) {
                g_mdns_done = true;
                ESP_LOGI(TAG, "mDNS _sendspin._tcp advertised on port %d", SENDSPIN_WS_PORT);
            } else if (!warned) {
                ESP_LOGW(TAG, "mDNS not ready (%s), retrying", esp_err_to_name(err));
                warned = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// The library does not register mDNS on ESP-IDF, so advertise the player here.
// mdns_init() is done by the net component once connected.
esp_err_t advertise_mdns(const char *instance)
{
    mdns_txt_item_t txt[] = {
        {"path", SENDSPIN_WS_PATH},
        {"name", instance},
    };
    return mdns_service_add(instance, "_sendspin", "_tcp", SENDSPIN_WS_PORT, txt, 2);
}

}  // namespace

extern "C" esp_err_t source_sendspin_init(void)
{
    // Use the configured device name (shown in Music Assistant); fall back to the
    // unique "Bugne <id>" when unset.
    const config_t *c = config_store_get();
    if (c && c->device_name[0]) {
        g_name = c->device_name;
    } else {
        g_name = std::string("Bugne ") + board_device_id();
    }

    SendspinClientConfig cfg;
    cfg.client_id = board_device_id();
    cfg.name = g_name;
    cfg.product_name = "Bugne";
    cfg.manufacturer = "Bugne";
    cfg.software_version = "0.1.0";
    cfg.httpd_psram_stack = true;  // httpd task stack in PSRAM: frees internal RAM for HTTPS streaming

    g_client = new SendspinClient(std::move(cfg));

    PlayerRoleConfig pcfg;
    pcfg.audio_formats = {
        {SendspinCodecFormat::FLAC, 2, 44100, 16},
        {SendspinCodecFormat::FLAC, 2, 48000, 16},
        {SendspinCodecFormat::OPUS, 2, 48000, 16},
        {SendspinCodecFormat::PCM,  2, 44100, 16},
        {SendspinCodecFormat::PCM,  2, 48000, 16},
    };
    pcfg.audio_buffer_capacity = 1024 * 1024;  // ~6 s at 44.1k/16/2; smooths stutter (PSRAM is ample)
    pcfg.psram_stack = true;                   // sync task stack in PSRAM

    PlayerRole &player = g_client->add_player(std::move(pcfg));
    g_player_listener.player = &player;
    player.set_listener(&g_player_listener);

    // Metadata role: track title/artist/progress for the now-playing screen.
    g_metadata = &g_client->add_metadata();
    g_metadata->set_listener(&g_metadata_listener);

    // Controller role: lets the device send PLAY/PAUSE/STOP to Music Assistant.
    g_controller = &g_client->add_controller();

    g_client->set_network_provider(&g_net_provider);

    if (!g_client->start_server()) {
        ESP_LOGE(TAG, "sendspin start_server failed");
        return ESP_FAIL;
    }
    // mDNS is advertised by sendspin_task once the station connects (mDNS is not
    // initialized in AP provisioning mode).

    // 8 KB stack: loop() builds and sends the client/hello (JSON serialization)
    // on this task; 4 KB overflowed. The stack lives in PSRAM to keep internal
    // RAM free for HTTPS streaming (TLS reads fail with only a few KB internal).
    static TaskHandle_t s_task;
    xTaskCreateWithCaps(sendspin_task, "sendspin", 8192, nullptr, 4, &s_task, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "sendspin player '%s' started", g_name.c_str());
    return ESP_OK;
}

extern "C" bool source_sendspin_active(void)
{
    return s_active.load();
}

extern "C" bool source_sendspin_session_active(void)
{
    return s_session.load();
}

extern "C" size_t source_sendspin_title(char *buf, size_t size)
{
    if (!buf || size == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(s_meta_mutex);
    strlcpy(buf, s_title, size);
    return strlen(buf);
}

extern "C" size_t source_sendspin_artist(char *buf, size_t size)
{
    if (!buf || size == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(s_meta_mutex);
    strlcpy(buf, s_artist, size);
    return strlen(buf);
}

extern "C" void source_sendspin_progress(uint32_t *pos_ms, uint32_t *dur_ms)
{
    if (pos_ms) *pos_ms = s_progress_ms.load();
    if (dur_ms) *dur_ms = s_duration_ms.load();
}

extern "C" void source_sendspin_command(sendspin_cmd_t cmd)
{
    // Mute at once on Stop: the sendspin engine keeps draining its ~6 s audio
    // buffer to the codec until the stream actually ends, so without this the
    // sound lingers for seconds after the button press.
    if (cmd == SENDSPIN_CMD_STOP) {
        audio_output_off();
    }
    s_pending_cmd.store((int)cmd);
}
