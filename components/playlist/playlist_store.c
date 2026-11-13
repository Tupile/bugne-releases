// Playlist SD layer. See playlist.h.
#include "playlist.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "source_sd.h"

#define EXT      ".m3u"
#define ABS_MAX  (sizeof(PLAYLIST_ABS_DIR) + PLAYLIST_NAME_MAX + 12)  // "/" name ".m3u.tmp"

static SemaphoreHandle_t s_lock;

void playlist_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

// Common entry checks, then the mutex. ESP_OK means the caller holds it.
static esp_err_t enter(uint32_t wait_ms)
{
    if (!source_sd_present()) return ESP_ERR_INVALID_STATE;
    if (!s_lock) return ESP_ERR_INVALID_STATE;  // playlist_init not called
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(wait_ms)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void leave(void)
{
    xSemaphoreGive(s_lock);
}

static void abs_path(char *out, size_t size, const char *name, const char *suffix)
{
    snprintf(out, size, PLAYLIST_ABS_DIR "/%s" EXT "%s", name, suffix);
}

static bool exists(const char *name)
{
    char abs[ABS_MAX];
    struct stat st;
    abs_path(abs, sizeof(abs), name, "");
    return stat(abs, &st) == 0;
}

static int cmp_names(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

// Scan the folder. names may be NULL to count only. Caller holds the mutex.
static size_t scan(char (*names)[PLAYLIST_NAME_MAX], size_t max)
{
    DIR *dir = opendir(PLAYLIST_ABS_DIR);
    if (!dir) return 0;
    size_t n = 0;
    struct dirent *de;
    while (n < max && (de = readdir(dir)) != NULL) {
        size_t len = strlen(de->d_name);
        // Exactly "<name>.m3u": this also skips the ".m3u.tmp" of a write.
        if (len <= strlen(EXT) || strcasecmp(de->d_name + len - strlen(EXT), EXT) != 0) continue;
        char name[PLAYLIST_NAME_MAX];
        size_t nlen = len - strlen(EXT);
        if (nlen >= sizeof(name)) continue;
        memcpy(name, de->d_name, nlen);
        name[nlen] = '\0';
        if (!playlist_name_valid(name)) continue;
        if (names) memcpy(names[n], name, nlen + 1);
        n++;
    }
    closedir(dir);
    if (names) qsort(names, n, sizeof(names[0]), cmp_names);
    return n;
}

esp_err_t playlist_list(char (*names)[PLAYLIST_NAME_MAX], size_t max, size_t *count,
                        uint32_t wait_ms)
{
    if (count) *count = 0;
    if (!names || !count) return ESP_ERR_INVALID_ARG;
    esp_err_t err = enter(wait_ms);
    if (err != ESP_OK) return err;
    *count = scan(names, max);
    leave();
    return ESP_OK;
}

esp_err_t playlist_read(const char *name, char (*titles)[PLAYLIST_TITLE_MAX],
                        char (*paths)[PLAYLIST_PATH_MAX], size_t max, size_t *count,
                        bool *truncated, uint32_t wait_ms)
{
    if (count) *count = 0;
    if (truncated) *truncated = false;
    if (!playlist_name_valid(name) || !titles || !paths || !count) return ESP_ERR_INVALID_ARG;
    esp_err_t err = enter(wait_ms);
    if (err != ESP_OK) return err;
    char abs[ABS_MAX];
    abs_path(abs, sizeof(abs), name, "");
    FILE *f = fopen(abs, "rb");
    if (!f) {
        leave();
        return ESP_ERR_NOT_FOUND;
    }
    *count = playlist_m3u_read(f, titles, paths, max, truncated);
    fclose(f);
    leave();
    return ESP_OK;
}

esp_err_t playlist_write(const char *name, char (*titles)[PLAYLIST_TITLE_MAX],
                         char (*paths)[PLAYLIST_PATH_MAX], size_t count, uint32_t wait_ms)
{
    if (!playlist_name_valid(name) || count > PLAYLIST_MAX_TRACKS ||
        (count && (!titles || !paths))) {
        return ESP_ERR_INVALID_ARG;
    }
    // Refuse an unsafe path before touching the card.
    for (size_t i = 0; i < count; i++) {
        char norm[PLAYLIST_PATH_MAX];
        if (!playlist_path_normalize(paths[i], norm, sizeof(norm))) return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = enter(wait_ms);
    if (err != ESP_OK) return err;
    if (source_sd_mkdir(PLAYLIST_DIR) != ESP_OK) {
        leave();
        return ESP_FAIL;
    }
    if (!exists(name) && scan(NULL, PLAYLIST_MAX_LISTS) >= PLAYLIST_MAX_LISTS) {
        leave();
        return ESP_ERR_NO_MEM;
    }
    char abs[ABS_MAX], tmp[ABS_MAX];
    abs_path(abs, sizeof(abs), name, "");
    abs_path(tmp, sizeof(tmp), name, ".tmp");
    err = ESP_FAIL;
    FILE *f = fopen(tmp, "wb");
    if (f) {
        bool ok = playlist_m3u_write(f, titles, paths, count);
        // The final flush is where a full card fails: check it before the
        // temp file replaces good data.
        if (fclose(f) != 0) ok = false;
        if (ok) {
            remove(abs);  // FatFs rename does not replace an existing file
            if (rename(tmp, abs) == 0) err = ESP_OK;
        }
        if (err != ESP_OK) remove(tmp);
    }
    leave();
    return err;
}

esp_err_t playlist_rename(const char *from, const char *to, uint32_t wait_ms)
{
    if (!playlist_name_valid(from) || !playlist_name_valid(to)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = enter(wait_ms);
    if (err != ESP_OK) return err;
    char a[ABS_MAX], b[ABS_MAX];
    abs_path(a, sizeof(a), from, "");
    abs_path(b, sizeof(b), to, "");
    if (!exists(from)) {
        err = ESP_ERR_NOT_FOUND;
#ifndef ESP_PLATFORM
    // POSIX rename replaces an existing target silently, so the host build
    // needs its own check. On the device FatFs is the judge (below).
    } else if (strcasecmp(from, to) != 0 && exists(to)) {
        err = ESP_ERR_NOT_ALLOWED;
#endif
    } else if (rename(a, b) == 0) {
        err = ESP_OK;
    } else {
        // FatFs refuses a target that is ANOTHER file (EEXIST) and allows a
        // change of case of the same entry, accents included: it folds
        // Unicode, which strcasecmp does not.
        err = (errno == EEXIST) ? ESP_ERR_NOT_ALLOWED : ESP_FAIL;
    }
    leave();
    return err;
}

esp_err_t playlist_delete(const char *name, uint32_t wait_ms)
{
    if (!playlist_name_valid(name)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = enter(wait_ms);
    if (err != ESP_OK) return err;
    char abs[ABS_MAX];
    abs_path(abs, sizeof(abs), name, "");
    if (!exists(name)) err = ESP_ERR_NOT_FOUND;
    else err = remove(abs) == 0 ? ESP_OK : ESP_FAIL;
    leave();
    return err;
}
