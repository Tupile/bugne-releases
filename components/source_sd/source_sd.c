// source_sd: mount the SD card and play local files through the decoder.
#include "source_sd.h"
#include "decode.h"
#include "audio_arbiter.h"
#include "board_pins.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

// stdio read-ahead buffer for playback. Batches SD reads into rare large bursts
// instead of many tiny FATFS transactions, so a slow read does not stall the
// decode loop long enough to underrun the I2S DMA (audible stutter).
#define SD_IO_BUF_BYTES (32 * 1024)

static const char *TAG = "source_sd";

#define SD_MOUNT_POINT "/sdcard"

static sdmmc_card_t *s_card;
static bool s_present;
static volatile bool s_stop;
static bool s_completed;  // last play decoded to the end (not stopped, no error)

esp_err_t source_sd_init(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,  // never wipe the user's card
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // Init always probes at 400 kHz, so the old 0x107 was a missing/unseated card,
    // not the clock. Try high-speed 40 MHz first (double the read throughput for
    // library scans and FLAC), falling back to the standard 20 MHz if the card or
    // wiring does not take it (mount failure). max_freq_khz is a ceiling: the
    // driver negotiates down for cards that do not support high-speed mode.
    // Caveat: the fallback only catches a failed mount. A marginal card/wiring
    // (internal pullups only) can mount at 40 MHz yet throw CRC errors during
    // long reads; if playback shows CRC errors on real hardware, set this back
    // to SDMMC_FREQ_DEFAULT (20 MHz, the previously validated speed).
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;  // 40 MHz
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = BOARD_SD_CLK_GPIO;
    slot.cmd = BOARD_SD_CMD_GPIO;
    slot.d0  = BOARD_SD_D0_GPIO;
    slot.d1  = BOARD_SD_D1_GPIO;
    slot.d2  = BOARD_SD_D2_GPIO;
    slot.d3  = BOARD_SD_D3_GPIO;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "mounting SD: 4-bit, %d kHz, CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d",
             host.max_freq_khz, BOARD_SD_CLK_GPIO, BOARD_SD_CMD_GPIO, BOARD_SD_D0_GPIO,
             BOARD_SD_D1_GPIO, BOARD_SD_D2_GPIO, BOARD_SD_D3_GPIO);

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mount at %d kHz failed (%s), retrying at 20 MHz",
                 host.max_freq_khz, esp_err_to_name(err));
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;  // 20 MHz, the previously validated speed
        err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no SD card mounted (%s); SD features disabled", esp_err_to_name(err));
        s_present = false;
        return ESP_OK;  // boot continues without a card
    }
    s_present = true;
    ESP_LOGI(TAG, "SD mounted at %s", SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);  // type, size, negotiated freq, bus width
    return ESP_OK;
}

bool source_sd_present(void)
{
    return s_present;
}

bool source_sd_usage(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!s_present) return false;
    uint64_t t = 0, f = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &t, &f) != ESP_OK) return false;
    if (total_bytes) *total_bytes = t;
    if (free_bytes)  *free_bytes  = f;
    return true;
}

void source_sd_stop(void)
{
    s_stop = true;
}

bool source_sd_completed(void)
{
    return s_completed;
}

// decode_source_t callbacks over a FILE*.
static size_t file_read(void *ctx, void *buf, size_t bytes)
{
    if (s_stop) {
        return 0;  // signal EOF so decode_run returns
    }
    return fread(buf, 1, bytes, (FILE *)ctx);
}

static bool file_seek(void *ctx, int offset, int origin)
{
    int whence = origin == 1 ? SEEK_CUR : (origin == 2 ? SEEK_END : SEEK_SET);
    return fseek((FILE *)ctx, offset, whence) == 0;
}

static bool file_tell(void *ctx, int64_t *cursor)
{
    long pos = ftell((FILE *)ctx);
    if (pos < 0) {
        return false;
    }
    *cursor = pos;
    return true;
}

static bool format_from_path(const char *path, decode_format_t *fmt)
{
    const char *dot = strrchr(path, '.');
    if (!dot) {
        return false;
    }
    if (strcasecmp(dot, ".mp3") == 0) {
        *fmt = DECODE_FORMAT_MP3;
        return true;
    }
    if (strcasecmp(dot, ".flac") == 0) {
        *fmt = DECODE_FORMAT_FLAC;
        return true;
    }
    if (strcasecmp(dot, ".m4a") == 0 || strcasecmp(dot, ".aac") == 0 ||
        strcasecmp(dot, ".mp4") == 0) {
        *fmt = DECODE_FORMAT_AAC;  // run_aac sniffs MP4 vs ADTS; SD is seekable
        return true;
    }
    if (strcasecmp(dot, ".ogg") == 0 || strcasecmp(dot, ".opus") == 0 ||
        strcasecmp(dot, ".oga") == 0) {
        *fmt = DECODE_FORMAT_OGG;  // Ogg container: Opus or Vorbis
        return true;
    }
    return false;
}

esp_err_t source_sd_play(const char *path)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    decode_format_t fmt;
    if (!format_from_path(path, &fmt)) {
        ESP_LOGW(TAG, "unsupported file: %s", path);
        return ESP_ERR_NOT_SUPPORTED;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return ESP_FAIL;
    }
    // Batch SD reads through a large PSRAM-backed stdio buffer (valid until fclose).
    char *iobuf = heap_caps_malloc(SD_IO_BUF_BYTES, MALLOC_CAP_SPIRAM);
    if (iobuf) {
        setvbuf(f, iobuf, _IOFBF, SD_IO_BUF_BYTES);
    }
    esp_err_t err = audio_arbiter_acquire(AUDIO_SOURCE_SD);
    if (err != ESP_OK) {
        fclose(f);
        free(iobuf);
        return err;
    }
    s_stop = false;

    // File size for the decoder's duration estimate (seek back to the start so
    // the decoder reads from the beginning).
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    decode_source_t src = {
        .read = file_read,
        .seek = file_seek,
        .tell = file_tell,
        .ctx  = f,
        .total_bytes = fsize > 0 ? fsize : 0,
    };
    ESP_LOGI(TAG, "playing %s", path);
    err = decode_run(fmt, &src);
    // A clean decode that was not interrupted by source_sd_stop() means the file
    // reached its end, so the caller can move to the next track in the folder.
    s_completed = (err == ESP_OK) && !s_stop;

    audio_arbiter_release(AUDIO_SOURCE_SD);
    fclose(f);       // flushes/detaches the stdio buffer before we free it
    free(iobuf);
    return err;
}

esp_err_t source_sd_list(const char *dir, char names[][SOURCE_SD_NAME_MAX], size_t max, size_t *count)
{
    *count = 0;
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    DIR *d = opendir(dir);
    if (!d) {
        return ESP_ERR_NOT_FOUND;
    }
    struct dirent *ent;
    decode_format_t fmt;
    while (*count < max && (ent = readdir(d)) != NULL) {
        if (ent->d_type == DT_DIR) {
            continue;
        }
        if (!format_from_path(ent->d_name, &fmt)) {
            continue;  // only playable files
        }
        strlcpy(names[*count], ent->d_name, SOURCE_SD_NAME_MAX);
        (*count)++;
    }
    closedir(d);
    return ESP_OK;
}

// ---- File-manager API (#29) ----

#define SD_PATH_MAX 320

// Build "/sdcard/<rel>" into out, rejecting traversal and absolute paths. rel
// is untrusted (from the web). "" maps to the mount root.
static bool sd_safe_path(const char *rel, char *out, size_t out_size)
{
    if (!rel) rel = "";
    if (rel[0] == '/' || strstr(rel, "..")) {
        return false;  // no absolute paths, no parent traversal
    }
    int n = snprintf(out, out_size, "%s/%s", SD_MOUNT_POINT, rel);
    return n > 0 && (size_t)n < out_size;
}

// Create every parent directory of full (an absolute /sdcard/... path). The
// final component is treated as a file and not created.
static void sd_make_parents(char *full)
{
    for (char *p = full + strlen(SD_MOUNT_POINT) + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(full, 0777);  // ignore EEXIST
            *p = '/';
        }
    }
}

esp_err_t source_sd_browse(const char *rel_dir, source_sd_entry_t *out, size_t max, size_t *count,
                           bool with_sizes)
{
    *count = 0;
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    char full[SD_PATH_MAX];
    if (!sd_safe_path(rel_dir, full, sizeof(full))) {
        return ESP_ERR_INVALID_ARG;
    }
    DIR *d = opendir(full);
    if (!d) {
        return ESP_ERR_NOT_FOUND;
    }
    struct dirent *ent;
    while (*count < max && (ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
            continue;
        }
        source_sd_entry_t *e = &out[*count];
        strlcpy(e->name, ent->d_name, sizeof(e->name));
        e->is_dir = (ent->d_type == DT_DIR);
        e->size = 0;
        if (!e->is_dir && with_sizes) {
            char fp[SD_PATH_MAX];
            struct stat st;
            if (snprintf(fp, sizeof(fp), "%s/%s", full, ent->d_name) < (int)sizeof(fp) &&
                stat(fp, &st) == 0) {
                e->size = (uint32_t)st.st_size;
            }
        }
        (*count)++;
    }
    closedir(d);
    return ESP_OK;
}

FILE *source_sd_create(const char *rel_path)
{
    if (!s_present) {
        return NULL;
    }
    char full[SD_PATH_MAX];
    if (!sd_safe_path(rel_path, full, sizeof(full))) {
        return NULL;
    }
    sd_make_parents(full);
    return fopen(full, "wb");
}

esp_err_t source_sd_mkdir(const char *rel_path)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    char full[SD_PATH_MAX];
    if (!sd_safe_path(rel_path, full, sizeof(full))) {
        return ESP_ERR_INVALID_ARG;
    }
    sd_make_parents(full);  // create intermediate dirs
    if (mkdir(full, 0777) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Recursively delete the path in `full` (an absolute /sdcard/... buffer of size
// SD_PATH_MAX). For a directory, removes its contents first. `full` is mutated
// in place (child name appended then restored).
static esp_err_t sd_rm_recursive(char *full)
{
    struct stat st;
    if (stat(full, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(full) == 0 ? ESP_OK : ESP_FAIL;
    }
    DIR *d = opendir(full);
    if (!d) {
        return ESP_FAIL;
    }
    size_t base = strlen(full);
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
            continue;
        }
        int n = snprintf(full + base, SD_PATH_MAX - base, "/%s", ent->d_name);
        if (n > 0 && (size_t)n < SD_PATH_MAX - base) {
            sd_rm_recursive(full);
        }
        full[base] = '\0';  // restore the parent path for the next entry
    }
    closedir(d);
    return rmdir(full) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t source_sd_delete(const char *rel_path)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!rel_path || !rel_path[0]) {
        return ESP_ERR_INVALID_ARG;  // refuse to delete the root
    }
    char full[SD_PATH_MAX];
    if (!sd_safe_path(rel_path, full, sizeof(full))) {
        return ESP_ERR_INVALID_ARG;
    }
    return sd_rm_recursive(full);
}
