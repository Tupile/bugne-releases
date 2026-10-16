#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DECODE_FORMAT_MP3,
    DECODE_FORMAT_FLAC,
    DECODE_FORMAT_OGG,
} decode_format_t;

typedef struct {
    size_t (*read)(void *ctx, void *buf, size_t n);
    bool (*seek)(void *ctx, int off, int origin);
    bool (*tell)(void *ctx, int64_t *cur);
    void *ctx;
} decode_source_t;

typedef struct {
    char artist[128];
    char album[128];
    char title[128];
    char album_artist[128];
    int track;
} decode_tags_t;

// We declare it extern here; we'll implement it in the test file
// so we can return mock tags deterministically.
extern int decode_read_tags(decode_format_t fmt, decode_source_t *src, decode_tags_t *tags);
