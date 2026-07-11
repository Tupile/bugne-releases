// decode: MP3 (dr_mp3) and FLAC (dr_flac) decoding to the audio output.
//
// A source provides bytes through the callbacks below. decode_run() detects the
// format's sample rate and channels, opens the audio output, writes PCM until
// EOF, then closes. The SD source and the stream source share this code and
// differ only in the byte callbacks they supply.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    DECODE_FORMAT_MP3,
    DECODE_FORMAT_FLAC,
    DECODE_FORMAT_AAC,   // AAC-LC: .m4a (MP4 container) or raw ADTS
    DECODE_FORMAT_OGG,   // Ogg container: Opus or Vorbis (esp_audio_codec simple dec)
} decode_format_t;

// Byte-source callbacks. origin: 0 = start, 1 = current, 2 = end.
typedef struct {
    size_t (*read)(void *ctx, void *buf, size_t bytes);   // bytes read, 0 at EOF
    bool   (*seek)(void *ctx, int offset, int origin);    // true on success
    bool   (*tell)(void *ctx, int64_t *cursor);           // current position
    void   *ctx;
    int64_t total_bytes;  // total size of the source if known, else 0 (used to
                          // estimate MP3 duration; FLAC reads it from the header)
} decode_source_t;

// Decode the whole stream to the audio output. Blocking until EOF or error.
esp_err_t decode_run(decode_format_t fmt, const decode_source_t *src);

// Current playback position and total duration of the running decode, in ms.
// dur_ms is 0 when unknown (e.g. a live stream, or an MP3 before the duration
// estimate settles). Safe to call from another task.
void decode_progress(uint32_t *pos_ms, uint32_t *dur_ms);

// Request a seek to target_ms in the running decode. Honored on the next decode
// iteration; only effective for a seekable source (SD files). No-op if nothing
// is decoding.
void decode_seek(uint32_t target_ms);

// Skip the first ms of audio of the NEXT decode_run() (one-shot, consumed and
// cleared at the start of that run). Used to skip a podcast intro when streaming
// (MP3: decode and discard the leading frames; FLAC: seek). Set 0 to disable.
void decode_set_start_skip_ms(uint32_t ms);

// Copy the current track's title/artist tags (UTF-8) into the buffers, each
// NUL-terminated. Empty when the file carries no tags or none parsed yet (tags
// are read at decoder init, i.e. early in decode_run). title/artist may be NULL.
// Safe to call from another task.
void decode_metadata(char *title, size_t title_size, char *artist, size_t artist_size);

// Clear the captured tags. Call when switching tracks so the previous track's
// tags do not show before the new one is parsed.
void decode_clear_metadata(void);

// Tags read from a file without decoding audio (for the SD library index).
typedef struct {
    char title[64];
    char artist[64];
    char album_artist[64];  // ALBUMARTIST/TPE2 (empty if absent); use for album grouping
    char album[64];
    int  track;   // track number, 0 if unknown
} decode_tags_t;

// Read only the tags from a source (opens the decoder, fires the metadata
// callbacks, closes; no audio). Fields are empty/0 when a tag is absent.
// Returns ESP_OK if the file opened as the given format.
esp_err_t decode_read_tags(decode_format_t fmt, const decode_source_t *src, decode_tags_t *out);
