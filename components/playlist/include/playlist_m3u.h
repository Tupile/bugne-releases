// Playlist file format: one extended M3U file per playlist. No ESP dependency,
// host-tested in test/host/test_playlist_m3u.c.
//
//     #EXTM3U
//     #EXTINF:-1,<title>
//     <path relative to the SD root>
//
// A file may come from a PC, so the reader is tolerant (CRLF, BOM, blank
// lines, unknown directives, missing EXTINF, absolute or backslash paths) and
// every entry is untrusted: a refused entry is skipped and the rest loads.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLAYLIST_NAME_MAX   49   // 48 bytes + NUL; + ".m3u" fits SOURCE_SD_NAME_MAX
#define PLAYLIST_TITLE_MAX  64   // = LIB_NAME_MAX: entries are played as library tracks
#define PLAYLIST_PATH_MAX   256  // = LIB_PATH_MAX
#define PLAYLIST_MAX_TRACKS 128  // = the UI's LIB_DISP_MAX
#define PLAYLIST_MAX_LISTS  20

// Whether a playlist name can be a file name: 1 to 48 bytes, none of
// / \ : * ? " < > |, no control character, no leading dot, no trailing dot or
// space. Bytes above 0x7F (UTF-8) are allowed.
bool playlist_name_valid(const char *name);

// Turn one path of a playlist into a path relative to the SD root: '\' becomes
// '/', a leading "/sdcard/" or '/' is dropped, THEN the checks run. Returns
// false (out untouched or partial) for an empty path, a URL, a control
// character, a dots-only segment (".."), a non-playable extension, or a result
// that does not fit in size.
bool playlist_path_normalize(const char *in, char *out, size_t size);

// Read a whole playlist from f. Fills at most max entries and returns the
// count. *truncated (optional) is set when a valid entry was left out because
// max was reached.
size_t playlist_m3u_read(FILE *f, char (*titles)[PLAYLIST_TITLE_MAX],
                         char (*paths)[PLAYLIST_PATH_MAX], size_t max, bool *truncated);

// Write a whole playlist to f (header, then one EXTINF + path pair per entry,
// LF line ends). Returns false on a stream error or when a path would be
// refused by the reader, so a file this function wrote always reads back
// whole. The caller still has to check fclose().
bool playlist_m3u_write(FILE *f, char (*titles)[PLAYLIST_TITLE_MAX],
                        char (*paths)[PLAYLIST_PATH_MAX], size_t count);

#ifdef __cplusplus
}
#endif
