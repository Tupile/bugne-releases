// memo_store: the /sdcard/memos directory. A directory scan is the only
// state: no NVS, no wall clock (memos are ordered by a monotonic sequence
// number embedded in the file name).
#include <dirent.h>
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
                char abs[MEMO_NAME_MAX + 20];
                memo_abs_path(abs, sizeof(abs), e.name);
                struct stat st;
                if (stat(abs, &st) == 0 && st.st_size > MEMO_WAV_HEADER_BYTES)
                    e.duration_s = (int)((st.st_size - MEMO_WAV_HEADER_BYTES) / (MEMO_RATE_HZ * 2));
                insert_sorted(out, max, count < max ? count : max, &e);
            }
            count++;
        }
        closedir(dir);
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

int memo_keep_rec(void)
{
    int seq = next_seq();
    char name[MEMO_NAME_MAX], dst[MEMO_NAME_MAX + 20];
    memo_name_mine(name, sizeof(name), seq);
    memo_abs_path(dst, sizeof(dst), name);
    if (rename(MEMO_ABS_DIR "/" MEMO_REC_NAME, dst) != 0) {
        ESP_LOGW(TAG, "keep failed: %s", dst);
        return -1;
    }
    return seq;
}

FILE *memo_rx_create(const char *sender, char *final_abs, size_t final_size,
                     char *part_abs, size_t part_size)
{
    if (!source_sd_present()) return NULL;
    if (source_sd_mkdir(MEMO_DIR) != ESP_OK) return NULL;
    int seq = next_seq();
    // "wx" (O_EXCL) makes concurrent allocations collision-safe without a lock:
    // a taken name fails the open and the next sequence number is tried.
    for (int attempt = 0; attempt < 8; attempt++) {
        char name[MEMO_NAME_MAX];
        memo_name_rx(name, sizeof(name), sender, seq);
        snprintf(final_abs, final_size, MEMO_ABS_DIR "/%s", name);
        snprintf(part_abs, part_size, "%s.part", final_abs);
        FILE *f = fopen(part_abs, "wx");
        if (f) return f;
        seq = (seq % 999) + 1;
    }
    return NULL;
}

FILE *memo_tk_create(char *final_abs, size_t final_size,
                     char *part_abs, size_t part_size)
{
    if (!source_sd_present()) return NULL;
    if (source_sd_mkdir(MEMO_DIR) != ESP_OK) return NULL;
    // tk files are few and short-lived: a static counter is enough, "wx"
    // (O_EXCL) keeps concurrent receives collision-safe like memo_rx_create.
    static int s_tk_seq;
    for (int attempt = 0; attempt < 8; attempt++) {
        s_tk_seq = (s_tk_seq % 999) + 1;
        snprintf(final_abs, final_size, MEMO_ABS_DIR "/" MEMO_TK_PREFIX "%03d.wav", s_tk_seq);
        snprintf(part_abs, part_size, "%s.part", final_abs);
        FILE *f = fopen(part_abs, "wx");
        if (f) return f;
    }
    return NULL;
}

void memo_clean_parts(void)
{
    DIR *dir = opendir(MEMO_ABS_DIR);
    if (!dir) return;
    char victims[8][MEMO_NAME_MAX];
    int nv = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && nv < 8) {
        size_t len = strlen(de->d_name);
        bool part = len > 5 && strcmp(de->d_name + len - 5, ".part") == 0;
        if (part || strcmp(de->d_name, MEMO_REC_NAME) == 0)
            strlcpy(victims[nv++], de->d_name, MEMO_NAME_MAX);
    }
    closedir(dir);
    for (int i = 0; i < nv; i++) {
        char abs[MEMO_NAME_MAX + 20];
        memo_abs_path(abs, sizeof(abs), victims[i]);
        ESP_LOGI(TAG, "removing leftover %s", victims[i]);
        remove(abs);
    }
}

void memo_clean_talkie(void)
{
    DIR *dir = opendir(MEMO_ABS_DIR);
    if (!dir) return;
    char victims[8][MEMO_NAME_MAX];
    int nv = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && nv < 8) {
        if (strncmp(de->d_name, MEMO_TK_PREFIX, strlen(MEMO_TK_PREFIX)) == 0)
            strlcpy(victims[nv++], de->d_name, MEMO_NAME_MAX);
    }
    closedir(dir);
    for (int i = 0; i < nv; i++) {
        char abs[MEMO_NAME_MAX + 20];
        memo_abs_path(abs, sizeof(abs), victims[i]);
        remove(abs);
    }
}
