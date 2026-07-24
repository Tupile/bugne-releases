// Host-test stub: single-threaded tests need no real locking, so the mutex
// is a dummy handle and take/give always succeed.
#pragma once
#include "freertos/FreeRTOS.h"

typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffffUL

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static int dummy;
    return &dummy;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, unsigned long ticks)
{
    (void)s; (void)ticks;
    return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    (void)s;
    return pdTRUE;
}
