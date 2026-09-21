#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0

// The test counts every PSRAM allocation and can fail one by ordinal, so the
// best-effort paths (a missing dedup table, a missing scratch buffer) can be
// driven without a malloc interposer.
extern int lot23_alloc_calls, lot23_alloc_fail_nth;

static inline void *heap_caps_malloc(size_t n, unsigned caps)
{
    (void)caps;
    if (++lot23_alloc_calls == lot23_alloc_fail_nth) return NULL;
    return malloc(n);
}
static inline void *heap_caps_realloc(void *p, size_t n, unsigned caps)
{
    (void)caps;
    return realloc(p, n);
}
