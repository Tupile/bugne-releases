// tags: ID3v2 and Vorbis-comment parsing, extracted from decode.c on
// 2026-07-24 so it can be host-tested (test/host/test_tags.c). Pure: no ESP or
// decoder dependency. Every input is untrusted tag data, so every read is
// bounded and every output is a NUL-terminated, length-bounded UTF-8 string.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TAGS_MAX 64

typedef struct {
    char title[TAGS_MAX];
    char artist[TAGS_MAX];
    char album[TAGS_MAX];
    char album_artist[TAGS_MAX];  // ALBUMARTIST / TPE2 (library scanner only)
    int  track;                   // track number, 0 if unknown
} tags_t;

// Fill the EMPTY fields of out from a raw tag. A field already set is left
// alone: the decoders feed several tag sources per file and the first wins.
void tags_parse_id3v2(tags_t *out, const uint8_t *tag, size_t size);
void tags_parse_vorbis(tags_t *out, const uint8_t *p, uint32_t n);

// One ID3v2 text frame body (leading encoding byte + text) into bounded UTF-8.
// Exposed because the streaming scanner reads frames one at a time.
void tags_id3_text(char *out, size_t size, const uint8_t *p, size_t len);

// Bounded copy of a Vorbis comment value (UTF-8 by spec).
void tags_copy_utf8(char *out, size_t size, const char *s, size_t n);
