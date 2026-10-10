#include "podcast_keep.h"

size_t podcast_keep_select(const bool *cached, const bool *protect, size_t n, int keep, bool *del)
{
    size_t marked = 0;
    for (size_t i = 0; i < n; i++) {
        del[i] = keep > 0 && i >= (size_t)keep && cached[i] && !(protect && protect[i]);
        if (del[i]) marked++;
    }
    return marked;
}
