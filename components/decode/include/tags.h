// tags: ID3v2 and Vorbis-comment parsing, extracted from decode.c on
// 2026-07-24 so it can be host-tested (test/host/test_tags.c). Pure: no ESP or
// decoder dependency. Every input is untrusted tag data, so every read is
// bounded and every output is a NUL-terminated, length-bounded UTF-8 string.
//
// It also hosts one small UTF-8 utility, tags_utf8_trim_partial, for any code
// that strlcpy's untrusted UTF-8 into a fixed buffer (SD file names, ICY
// titles). source_stream.c still carries its own static copy for the ICY path:
// converge here if a third call site appears.
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

// Drop a multi-byte UTF-8 sequence left incomplete by a truncating copy, in
// place. A truncated sequence is not just cosmetic: it makes the JSON of an API
// that embeds the string invalid (GET /api/sd/list did this, and the web Files
// tab stopped parsing that folder) and renders as a garbage glyph on screen.
void tags_utf8_trim_partial(char *s);

// Locate the embedded JPEG cover in an ID3v2 tag (APIC frame, PIC in v2.2).
// Report-only, so it stays pure: on success *off/*len delimit the image bytes
// INSIDE tag, which the caller copies before its buffer goes away. A front
// cover (picture type 3) wins over any other type; otherwise the first JPEG
// found is used. Only JPEG is reported (the UI decodes nothing else).
bool tags_find_apic(const uint8_t *tag, size_t size, size_t *off, size_t *len);

