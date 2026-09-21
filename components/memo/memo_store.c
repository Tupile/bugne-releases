// memo_store: the /sdcard/memos directory. A directory scan is the only
// state: no NVS, no wall clock (memos are ordered by a monotonic sequence
// number embedded in the file name).
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "memo.h"
#include "source_sd.h"

static const char *TAG = "memo";

void memo_abs_path(char *dst, size_t size, const char *name)
{
    // Precision bound: names are at most MEMO_NAME_MAX, callers size buffers
    // accordingly (and -Wformat-truncation cannot see that on a bare %s).
    snprintf(dst, size, MEMO_ABS_DIR "/%.*s", MEMO_NAME_MAX - 1, name);
}

// Insertion sort, seq descending, keeping the max newest entries.
static void insert_sorted(memo_entry_t *out, int max, int filled, const memo_entry_t *e)
{
    int i = filled;
    if (i == max) {                       // full: only keep if newer than the oldest kept
        if (e->seq <= out[max - 1].seq) return;
        i = max - 1;
    }
    while (i > 0 && out[i - 1].seq < e->seq) {
        out[i] = out[i - 1];
        i--;
    }
    out[i] = *e;
}

// One scan serves list/count/unread/next-seq. Returns the total number of
// parseable memos; fills out (up to max) sorted seq-descending.
static int scan(memo_entry_t *out, int max, int *unread, int *max_seq)
{
    int count = 0, nu = 0, ms = 0;
    DIR *dir = opendir(MEMO_ABS_DIR);
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            memo_entry_t e = {0};
            if (!memo_name_parse(de->d_name, &e.is_mine, e.sender, sizeof(e.sender),
                                 &e.seq, &e.unread)) continue;
            if (e.unread) nu++;
            if (e.seq > ms) ms = e.seq;
            if (out && max > 0) {
                strlcpy(e.name, de->d_name, sizeof(e.name));
                insert_sorted(out, max, count < max ? count : max, &e);
            }
            count++;
        }
        closedir(dir);

        if (out && max > 0) {
            int kept = count < max ? count : max;
            for (int i = 0; i < kept; i++) {
                char abs[MEMO_NAME_MAX + 20];
                memo_abs_path(abs, sizeof(abs), out[i].name);
                struct stat st;
                if (stat(abs, &st) == 0 && st.st_size > MEMO_WAV_HEADER_BYTES)
                    out[i].duration_s = (int)((st.st_size - MEMO_WAV_HEADER_BYTES) / (MEMO_RATE_HZ * 2));
            }
        }
    }
    if (unread) *unread = nu;
    if (max_seq) *max_seq = ms;
    return count;
}

int memo_list(memo_entry_t *out, int max)
{
    int n = scan(out, max, NULL, NULL);
    return n < max ? n : max;
}

int memo_count(void) { return scan(NULL, 0, NULL, NULL); }

int memo_unread_count(void)
{
    int nu = 0;
    scan(NULL, 0, &nu, NULL);
    return nu;
}

static int next_seq(void)
{
    int ms = 0;
    scan(NULL, 0, NULL, &ms);
    return (ms % 999) + 1;
}

static FILE *reserve_part(const char *final_abs, const char *read_abs,
                          char *part_abs, size_t part_size)
{
    int n = snprintf(part_abs, part_size, "%s.part", final_abs);
    if (n < 0 || (size_t)n >= part_size) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    FILE *f = fopen(part_abs, "wx");
    if (!f) return NULL;
    struct stat st;
    int err = 0;
    if (stat(final_abs, &st) == 0) err = EEXIST;
    else if (errno != ENOENT) err = errno;
    if (!err && read_abs) {
        if (stat(read_abs, &st) == 0) err = EEXIST;
        else if (errno != ENOENT) err = errno;
    }
    if (!err) return f;
    fclose(f);
    remove(part_abs);
    errno = err;
    return NULL;
}

int memo_keep_rec(void)
{
    int seq = next_seq();
    for (int attempt = 0; attempt < 999; attempt++, seq = (seq % 999) + 1) {
        char name[MEMO_NAME_MAX], dst[MEMO_NAME_MAX + 20], part[MEMO_NAME_MAX + 25];
        memo_name_mine(name, sizeof(name), seq);
        memo_abs_path(dst, sizeof(dst), name);
        FILE *reservation = reserve_part(dst, NULL, part, sizeof(part));
        if (!reservation) {
            if (errno == EEXIST) continue;
            return -1;
        }
        int result = fclose(reservation);
        if (result == 0) result = rename(MEMO_ABS_DIR "/" MEMO_REC_NAME, dst);
        remove(part);
        if (result != 0) {
            ESP_LOGW(TAG, "keep failed: %s", dst);
            return -1;
        }
        return seq;
    }
    return -1;
}

FILE *memo_rx_create(const char *sender, char *final_abs, size_t final_size,
                     char *part_abs, size_t part_size)
{
    if (!source_sd_present()) return NULL;
    if (source_sd_mkdir(MEMO_DIR) != ESP_OK) return NULL;
    int seq = next_seq();
    for (int attempt = 0; attempt < 999; attempt++, seq = (seq % 999) + 1) {
        char name[MEMO_NAME_MAX], read_abs[MEMO_NAME_MAX + 20];
        memo_name_rx(name, sizeof(name), sender, seq);
        int n = snprintf(final_abs, final_size, MEMO_ABS_DIR "/%s", name);
        if (n < 0 || (size_t)n >= final_size) return NULL;
        snprintf(read_abs, sizeof(read_abs), MEMO_ABS_DIR "/rx-%s-%03d.wav", sender, seq);
        FILE *f = reserve_part(final_abs, read_abs, part_abs, part_size);
        if (f) return f;
        if (errno != EEXIST) return NULL;
    }
    return NULL;
}

FILE *memo_tk_create(char *final_abs, size_t final_size,
                     char *part_abs, size_t part_size)
{
    if (!source_sd_present()) return NULL;
    if (source_sd_mkdir(MEMO_DIR) != ESP_OK) return NULL;
    for (int seq = 1; seq <= 999; seq++) {
        int n = snprintf(final_abs, final_size, MEMO_ABS_DIR "/" MEMO_TK_PREFIX "%03d.wav", seq);
        if (n < 0 || (size_t)n >= final_size) return NULL;
        FILE *f = reserve_part(final_abs, NULL, part_abs, part_size);
        if (f) return f;
        if (errno != EEXIST) return NULL;
    }
    return NULL;
}

// Delete temporaries: talkie files (tk-*) when talkie is true, in-flight
// leftovers (*.part and the finalized capture) otherwise. Names are collected
// into a small stack buffer before deleting (never delete while walking the
// directory), so a directory holding more leftovers than fit takes several
// passes. Loops until a pass is not full, or makes no progress.
#define PURGE_BATCH 8

static void purge(bool talkie)
{
    for (;;) {
        DIR *dir = opendir(MEMO_ABS_DIR);
        if (!dir) return;
        char victims[PURGE_BATCH][MEMO_NAME_MAX];
        int nv = 0;
        struct dirent *de;
        while ((de = readdir(dir)) != NULL && nv < PURGE_BATCH) {
            bool hit;
            if (talkie) {
                hit = strncmp(de->d_name, MEMO_TK_PREFIX, strlen(MEMO_TK_PREFIX)) == 0;
            } else {
                size_t len = strlen(de->d_name);
                hit = (len > 5 && strcmp(de->d_name + len - 5, ".part") == 0) ||
                      strcmp(de->d_name, MEMO_REC_NAME) == 0;
            }
            if (hit) strlcpy(victims[nv++], de->d_name, MEMO_NAME_MAX);
        }
        closedir(dir);
        int removed = 0;
        for (int i = 0; i < nv; i++) {
            char abs[MEMO_NAME_MAX + 20];
            memo_abs_path(abs, sizeof(abs), victims[i]);
            if (!talkie) ESP_LOGI(TAG, "removing leftover %s", victims[i]);
            if (remove(abs) == 0) removed++;
        }
        if (nv < PURGE_BATCH || removed == 0) return;
    }
}

void memo_clean_parts(void) { purge(false); }

void memo_clean_talkie(void) { purge(true); }
