// Host-test stub: just enough FreeRTOS surface for single-threaded tests.
#pragma once
#include <stdbool.h>  // the real header provides bool transitively

typedef int BaseType_t;
#define pdTRUE  1
#define pdFALSE 0
