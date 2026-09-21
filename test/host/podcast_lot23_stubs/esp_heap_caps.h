#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
static inline void *heap_caps_malloc(size_t n, unsigned caps)
{
    (void)caps;
    return malloc(n);
}
static inline void *heap_caps_realloc(void *p, size_t n, unsigned caps)
{
    (void)caps;
    return realloc(p, n);
}
