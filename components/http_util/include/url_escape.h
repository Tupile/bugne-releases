// url_escape: make an untrusted URL acceptable to esp_http_client.
//
// Feeds put raw UTF-8 and spaces in enclosure URLs (e.g. "fusée.mp3"), and the
// strict http_parser inside esp_http_client rejects them ("Error parse url").
// Percent-encode exactly the bytes that are not legal in a URL, byte for byte:
// the UTF-8 bytes are kept as written (no NFC/NFD normalisation, servers match
// the exact byte form), and existing %XX escapes are left untouched.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// True if `c` must be percent-encoded: controls, space, DEL, non-ASCII bytes,
// and the characters RFC 3986 never allows raw (" < > \ ^ ` { | }).
bool url_escape_needed(unsigned char c);

// Length of the escaped form of `in`, without the terminating NUL.
size_t url_escape_len(const char *in);

// Write the escaped form of `in` into out[cap]. Returns false (out holds an
// empty string when cap > 0) if it does not fit.
bool url_escape(const char *in, char *out, size_t cap);

// Heap copy of the escaped form (caller frees), or NULL on allocation failure
// or a NULL input.
char *url_escape_dup(const char *in);

#ifdef __cplusplus
}
#endif
