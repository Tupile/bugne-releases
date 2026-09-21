#include "memo.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool g_sd_stub_present = true;
static bool fail_rename;
static bool collide_on_open;

static int review_rename(const char *old_path, const char *new_path)
{
    struct stat st;
    if (stat(new_path, &st) == 0) {
        errno = EEXIST;
        return -1;
    }
    if (fail_rename) {
        errno = EIO;
        return -1;
    }
    return rename(old_path, new_path);
}

static FILE *review_fopen(const char *path, const char *mode)
{
    FILE *f = fopen(path, mode);
    if (f && collide_on_open && strcmp(mode, "wx") == 0) {
        collide_on_open = false;
        char final[256];
        assert(strlen(path) < sizeof(final));
        strcpy(final, path);
        final[strlen(final) - 5] = '\0';
        FILE *other = fopen(final, "wx");
        assert(other);
        assert(fputs("old data", other) >= 0);
        assert(fclose(other) == 0);
    }
    return f;
}

#define rename review_rename
#define fopen review_fopen
#include "../../components/memo/memo_store.c"
#undef fopen
#undef rename

int main(void)
{
    assert(mkdir(MEMO_ABS_DIR, 0700) == 0);
    FILE *f = fopen(MEMO_ABS_DIR "/" MEMO_REC_NAME, "wx");
    assert(f);
    assert(fputs("capture", f) >= 0);
    assert(fclose(f) == 0);
    fail_rename = true;
    assert(memo_keep_rec() == -1);
    struct stat st;
    assert(stat(MEMO_ABS_DIR "/" MEMO_REC_NAME, &st) == 0 && st.st_size == 7);
    assert(stat(MEMO_ABS_DIR "/my-001.wav", &st) != 0);
    assert(stat(MEMO_ABS_DIR "/my-001.wav.part", &st) != 0);
    fail_rename = false;
    assert(memo_keep_rec() == 1);
    assert(stat(MEMO_ABS_DIR "/my-001.wav", &st) == 0 && st.st_size == 7);
    collide_on_open = true;
    char final[128], part[136];
    f = memo_rx_create("Peer", final, sizeof(final), part, sizeof(part));
    assert(f);
    assert(strcmp(final, MEMO_ABS_DIR "/rx-Peer-003.new.wav") == 0);
    assert(stat(MEMO_ABS_DIR "/rx-Peer-002.new.wav", &st) == 0 && st.st_size == 8);
    assert(stat(MEMO_ABS_DIR "/rx-Peer-002.new.wav.part", &st) != 0);
    assert(fclose(f) == 0);
    assert(unlink(part) == 0);
    assert(unlink(MEMO_ABS_DIR "/rx-Peer-002.new.wav") == 0);
    assert(unlink(MEMO_ABS_DIR "/my-001.wav") == 0);
    assert(rmdir(MEMO_ABS_DIR) == 0);
    puts("review memo fault tests: passed");
    return 0;
}
