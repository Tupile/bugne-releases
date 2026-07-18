// memo_name: memo filename scheme and sender sanitizing (pure, host-tested).
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "memo.h"

void memo_sanitize_sender(char *dst, size_t size, const char *src)
{
    size_t n = 0;
    bool last_dash = false;
    for (const char *p = src ? src : ""; *p && n + 1 < size && n < MEMO_SENDER_MAX - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '_') {
            dst[n++] = (char)c;
            last_dash = false;
        } else if (!last_dash && n > 0) {     // collapse runs, drop a leading dash
            dst[n++] = '-';
            last_dash = true;
        }
    }
    while (n > 0 && dst[n - 1] == '-') n--;   // drop a trailing dash
    dst[n] = '\0';
    if (n == 0) snprintf(dst, size, "peer");
}

void memo_name_mine(char *dst, size_t size, int seq)
{
    snprintf(dst, size, "my-%03d.wav", seq);
}

void memo_name_rx(char *dst, size_t size, const char *sender, int seq)
{
    snprintf(dst, size, "rx-%s-%03d.new.wav", sender, seq);
}

bool memo_name_parse(const char *name, bool *is_mine, char *sender, size_t sender_size,
                     int *seq, bool *unread)
{
    size_t len = strlen(name);
    if (len < 4 || strcmp(name + len - 4, ".wav") != 0) return false;
    len -= 4;
    bool nu = false;
    if (len >= 4 && strncmp(name + len - 4, ".new", 4) == 0) { nu = true; len -= 4; }

    size_t d = len;                           // digits run at the end
    while (d > 0 && isdigit((unsigned char)name[d - 1])) d--;
    if (d == len || len - d > 4 || d == 0 || name[d - 1] != '-') return false;
    int s = 0;
    for (size_t i = d; i < len; i++) s = s * 10 + (name[i] - '0');

    bool mine;
    if (strncmp(name, "my-", 3) == 0 && d == 3) {
        mine = true;
        if (sender_size > 0) sender[0] = '\0';
    } else if (strncmp(name, "rx-", 3) == 0 && d > 4) {   // at least 1 sender char
        mine = false;
        size_t sl = d - 1 - 3;                            // between "rx-" and the seq dash
        if (sl >= sender_size) sl = sender_size - 1;
        memcpy(sender, name + 3, sl);
        sender[sl] = '\0';
    } else {
        return false;
    }
    *is_mine = mine;
    *seq = s;
    *unread = nu;
    return true;
}
