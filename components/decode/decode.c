// decode: adapt the byte-source callbacks to dr_mp3 / dr_flac and stream PCM
// to the audio output.
#include "decode.h"
#include "audio.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include "esp_log.h"

// Vendored single-header decoders, compiled here. No stdio: we only use the
// callback API.
#define DR_MP3_NO_STDIO
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_FLAC_NO_STDIO
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

// AAC: the .m4a (MP4) container is demuxed by the vendored minimp4 (Radio France
// .m4a keeps moov at the end, which only random access can parse); the AAC-LC
// frames are decoded by the esp_audio_codec component.
#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"
#include "esp_aac_dec.h"
#include "esp_heap_caps.h"

// Ogg (Opus / Vorbis) via the esp_audio_codec simple decoder: the OGG parser
// demuxes the container and hands frames to the registered Opus/Vorbis decoders.
#include "esp_opus_dec.h"
#include "esp_vorbis_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_simple_dec.h"

static const char *TAG = "decode";

#define DECODE_FRAMES 1024  // PCM frames decoded per chunk

// Playback progress, shared with the UI task. 32-bit reads/writes are atomic on
// this core, so plain volatile is enough for these. s_seek_ms is a one-shot
// request (-1 = none) set by the UI and consumed by the decode loop.
static volatile uint32_t s_pos_ms;
static volatile uint32_t s_dur_ms;
static volatile int32_t  s_seek_ms = -1;
static volatile uint32_t s_start_skip_ms;  // one-shot: skip this much at the next decode start
static uint64_t s_bytes_read;  // bytes pulled from the source, for MP3 duration estimate
static uint8_t  s_xing_toc[100];  // Xing/Info seek TOC (percent -> bytes/256), for MP3 seek
static bool     s_xing_toc_valid;

void decode_progress(uint32_t *pos_ms, uint32_t *dur_ms)
{
    if (pos_ms) *pos_ms = s_pos_ms;
    if (dur_ms) *dur_ms = s_dur_ms;
}

void decode_seek(uint32_t target_ms)
{
    s_seek_ms = (int32_t)target_ms;
}

void decode_set_start_skip_ms(uint32_t ms)
{
    s_start_skip_ms = ms;
}

// ---- Tag capture: ID3v2 (MP3) and Vorbis comments (FLAC) ----
// Written once at decoder init, read by the UI task during playback. Title/
// artist only; everything is bounded (tag data is untrusted).

#define META_MAX 64
static char s_title[META_MAX];
static char s_artist[META_MAX];
static char s_album[META_MAX];
static char s_album_artist[META_MAX];  // ALBUMARTIST / TPE2 (scanner only)
static int  s_track;  // track number, 0 if unknown

// Append one Unicode code point as UTF-8 (BMP only), bounded by size.
static void utf8_put(char *out, size_t size, size_t *pos, uint32_t cp)
{
    if (cp < 0x80) {
        if (*pos + 1 >= size) return;
        out[(*pos)++] = (char)cp;
    } else if (cp < 0x800) {
        if (*pos + 2 >= size) return;
        out[(*pos)++] = (char)(0xC0 | (cp >> 6));
        out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (*pos + 3 >= size) return;
        out[(*pos)++] = (char)(0xE0 | (cp >> 12));
        out[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
    }
}

static void latin1_to_utf8(char *out, size_t size, const uint8_t *s, size_t n)
{
    size_t pos = 0;
    for (size_t i = 0; i < n && s[i]; i++) utf8_put(out, size, &pos, s[i]);
    out[pos] = '\0';
}

static void utf16_to_utf8(char *out, size_t size, const uint8_t *s, size_t n, bool be)
{
    size_t pos = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t u = be ? ((uint32_t)s[i] << 8 | s[i + 1]) : ((uint32_t)s[i + 1] << 8 | s[i]);
        if (u == 0) break;
        if (u >= 0xD800 && u <= 0xDFFF) continue;  // skip surrogate pairs (no SMP)
        utf8_put(out, size, &pos, u);
    }
    out[pos] = '\0';
}

// Decode an ID3v2 text frame body (leading encoding byte + text) into UTF-8.
static void id3_text(char *out, size_t size, const uint8_t *p, size_t len)
{
    out[0] = '\0';
    if (len < 1) return;
    uint8_t enc = p[0];
    const uint8_t *t = p + 1;
    size_t tn = len - 1;
    switch (enc) {
    case 0:  // ISO-8859-1
        latin1_to_utf8(out, size, t, tn);
        break;
    case 3: {  // UTF-8
        size_t k = tn < size - 1 ? tn : size - 1, j = 0;
        while (j < k && t[j]) { out[j] = (char)t[j]; j++; }
        out[j] = '\0';
        break;
    }
    case 1: {  // UTF-16 with BOM
        bool be = false;
        if (tn >= 2 && t[0] == 0xFE && t[1] == 0xFF)      { be = true;  t += 2; tn -= 2; }
        else if (tn >= 2 && t[0] == 0xFF && t[1] == 0xFE) { be = false; t += 2; tn -= 2; }
        utf16_to_utf8(out, size, t, tn, be);
        break;
    }
    case 2:  // UTF-16BE
        utf16_to_utf8(out, size, t, tn, true);
        break;
    default:
        break;
    }
}

// Pull TIT2/TPE1 (or TT2/TP1 for ID3v2.2) from a raw ID3v2 tag. All offsets are
// bounded by size; the tag is untrusted.
static void parse_id3v2(const uint8_t *tag, size_t size)
{
    if (size < 10 || tag[0] != 'I' || tag[1] != 'D' || tag[2] != '3') return;
    uint8_t ver = tag[3];
    uint8_t flags = tag[5];
    bool v22 = (ver == 2);
    size_t hdrlen = v22 ? 6 : 10;  // frame id + size (+ flags for v2.3/2.4)
    size_t pos = 10;
    if (!v22 && (flags & 0x40)) {  // skip an extended header
        if (pos + 4 > size) return;
        size_t ext = (ver == 4)
            ? (((size_t)(tag[pos] & 0x7F) << 21) | ((tag[pos+1] & 0x7F) << 14) |
               ((tag[pos+2] & 0x7F) << 7) | (tag[pos+3] & 0x7F))
            : (((size_t)tag[pos] << 24) | ((size_t)tag[pos+1] << 16) |
               ((size_t)tag[pos+2] << 8) | tag[pos+3]);
        pos += 4 + ext;
    }
    while (pos + hdrlen <= size) {
        const uint8_t *id = tag + pos;
        if (id[0] == 0) break;  // reached padding
        size_t fsize;
        if (v22) {
            fsize = ((size_t)id[3] << 16) | ((size_t)id[4] << 8) | id[5];
        } else if (ver == 4) {
            const uint8_t *s = id + 4;
            fsize = ((size_t)(s[0] & 0x7F) << 21) | ((s[1] & 0x7F) << 14) |
                    ((s[2] & 0x7F) << 7) | (s[3] & 0x7F);
        } else {
            const uint8_t *s = id + 4;
            fsize = ((size_t)s[0] << 24) | ((size_t)s[1] << 16) | ((size_t)s[2] << 8) | s[3];
        }
        if (fsize == 0 || fsize > size - pos - hdrlen) break;  // wrap-safe: the
                                        // while-condition guarantees pos + hdrlen <= size
        const uint8_t *data = tag + pos + hdrlen;
        bool title  = v22 ? !memcmp(id, "TT2", 3)  : !memcmp(id, "TIT2", 4);
        bool artist = v22 ? !memcmp(id, "TP1", 3)  : !memcmp(id, "TPE1", 4);
        bool album  = v22 ? !memcmp(id, "TAL", 3)  : !memcmp(id, "TALB", 4);
        bool track  = v22 ? !memcmp(id, "TRK", 3)  : !memcmp(id, "TRCK", 4);
        if (title  && !s_title[0])  id3_text(s_title,  sizeof s_title,  data, fsize);
        if (artist && !s_artist[0]) id3_text(s_artist, sizeof s_artist, data, fsize);
        if (album  && !s_album[0])  id3_text(s_album,  sizeof s_album,  data, fsize);
        if (track  && !s_track) {  // text like "3" or "3/12"; take the leading number
            char t[META_MAX];
            id3_text(t, sizeof t, data, fsize);
            s_track = atoi(t);
        }
        pos += hdrlen + fsize;
    }
}

static void cb_mp3_meta(void *p, const drmp3_metadata *m)
{
    (void)p;
    if (m->type == DRMP3_METADATA_TYPE_ID3V2 && m->pRawData && m->rawDataSize >= 10) {
        parse_id3v2((const uint8_t *)m->pRawData, m->rawDataSize);
    } else if ((m->type == DRMP3_METADATA_TYPE_XING || m->type == DRMP3_METADATA_TYPE_VBRI) &&
               m->pRawData && m->rawDataSize >= 8) {
        // Xing/Info tag (dr_mp3 posts "Info" under the VBRI label, same layout):
        // capture the 100-byte seek TOC for the MP3 byte seek. After the 4-byte
        // ident: 4-byte BE flags, then FRAMES(4) and BYTES(4) when flagged, then
        // the TOC. Tag data is untrusted: bound every read against rawDataSize.
        const uint8_t *d = m->pRawData;
        uint32_t flags = (uint32_t)d[4] << 24 | (uint32_t)d[5] << 16 |
                         (uint32_t)d[6] << 8 | d[7];
        size_t pos = 8;
        if (flags & 0x01) pos += 4;  // FRAMES
        if (flags & 0x02) pos += 4;  // BYTES
        if ((flags & 0x04) && pos + 100 <= m->rawDataSize) {
            memcpy(s_xing_toc, d + pos, 100);
            s_xing_toc_valid = true;
        }
    }
}

static void copy_utf8(char *out, size_t size, const char *s, size_t n)
{
    size_t k = n < size - 1 ? n : size - 1;
    memcpy(out, s, k);
    out[k] = '\0';
}

// Vorbis comment values are UTF-8 by spec, so copy directly (bounded).
static void cb_flac_meta(void *p, drflac_metadata *m)
{
    (void)p;
    if (m->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;
    drflac_vorbis_comment_iterator it;
    drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount,
                                        m->data.vorbis_comment.pComments);
    drflac_uint32 len;
    const char *c;
    while ((c = drflac_next_vorbis_comment(&it, &len)) != NULL) {
        if (len > 6 && !strncasecmp(c, "TITLE=", 6) && !s_title[0]) {
            copy_utf8(s_title, sizeof s_title, c + 6, len - 6);
        } else if (len > 7 && !strncasecmp(c, "ARTIST=", 7) && !s_artist[0]) {
            copy_utf8(s_artist, sizeof s_artist, c + 7, len - 7);
        } else if (len > 6 && !strncasecmp(c, "ALBUM=", 6) && !s_album[0]) {
            copy_utf8(s_album, sizeof s_album, c + 6, len - 6);
        } else if (len > 12 && !strncasecmp(c, "TRACKNUMBER=", 12) && !s_track) {
            s_track = atoi(c + 12);  // value is digits (possibly "3/12")
        }
    }
}

void decode_metadata(char *title, size_t title_size, char *artist, size_t artist_size)
{
    if (title && title_size)   strlcpy(title, s_title, title_size);
    if (artist && artist_size) strlcpy(artist, s_artist, artist_size);
}

void decode_clear_metadata(void)
{
    s_title[0] = '\0';
    s_artist[0] = '\0';
    s_album[0] = '\0';
    s_album_artist[0] = '\0';
    s_track = 0;
}

// Bridge the dr_* callbacks to decode_source_t. pUserData is the source.
static size_t cb_read(void *p, void *buf, size_t bytes)
{
    const decode_source_t *s = p;
    size_t got = s->read(s->ctx, buf, bytes);
    s_bytes_read += got;
    return got;
}

static drmp3_bool32 cb_mp3_seek(void *p, int offset, drmp3_seek_origin origin)
{
    const decode_source_t *s = p;
    return (s->seek && s->seek(s->ctx, offset, (int)origin)) ? DRMP3_TRUE : DRMP3_FALSE;
}

static drmp3_bool32 cb_mp3_tell(void *p, drmp3_int64 *cursor)
{
    const decode_source_t *s = p;
    int64_t c = 0;
    if (!s->tell || !s->tell(s->ctx, &c)) {
        return DRMP3_FALSE;
    }
    *cursor = c;
    return DRMP3_TRUE;
}

static drflac_bool32 cb_flac_seek(void *p, int offset, drflac_seek_origin origin)
{
    const decode_source_t *s = p;
    return (s->seek && s->seek(s->ctx, offset, (int)origin)) ? DRFLAC_TRUE : DRFLAC_FALSE;
}

static drflac_bool32 cb_flac_tell(void *p, drflac_int64 *cursor)
{
    const decode_source_t *s = p;
    int64_t c = 0;
    if (!s->tell || !s->tell(s->ctx, &c)) {
        return DRFLAC_FALSE;
    }
    *cursor = c;
    return DRFLAC_TRUE;
}

// Route dr_mp3/dr_flac allocations to PSRAM: the decoder state (drmp3 embeds
// minimp3's ~7 KB tables) and the PCM buffers live for the whole track, and
// internal RAM is the scarce resource during HTTPS streaming. PSRAM is fast
// enough here (the Sendspin path already streams PCM from a PSRAM buffer).
static void *psram_malloc_cb(size_t sz, void *ud) { (void)ud; return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM); }
static void *psram_realloc_cb(void *p, size_t sz, void *ud) { (void)ud; return heap_caps_realloc(p, sz, MALLOC_CAP_SPIRAM); }
static void psram_free_cb(void *p, void *ud) { (void)ud; free(p); }
static const drmp3_allocation_callbacks MP3_PSRAM_CB =
    { NULL, psram_malloc_cb, psram_realloc_cb, psram_free_cb };
static const drflac_allocation_callbacks FLAC_PSRAM_CB =
    { NULL, psram_malloc_cb, psram_realloc_cb, psram_free_cb };

// Map target_ms to an absolute byte offset in the source for the MP3 byte
// seek: Xing TOC when the file carries one (bounds VBR error to ~1% of the
// duration), else linear interpolation over the audio-only byte span (exact
// for CBR; trimmed podcast files have no Xing but their displayed duration is
// the same linear byte estimate, so the landing stays self-consistent).
static uint64_t mp3_seek_byte_for_ms(const drmp3 *mp3, const decode_source_t *src,
                                     uint32_t target_ms, uint32_t dur_ms)
{
    uint64_t start = mp3->streamStartOffset;
    uint64_t end = (mp3->streamLength != DRMP3_UINT64_MAX) ? mp3->streamLength
                 : (src->total_bytes > 0 ? (uint64_t)src->total_bytes : 0);
    if (end <= start || dur_ms == 0) {
        return start;
    }
    uint64_t span = end - start;
    if (s_xing_toc_valid) {
        // x = percent * 256: index the TOC at the whole percent, interpolate
        // to the next entry with the fractional part. TOC values are untrusted
        // (kept monotonic) and scaled 0..255 over the stream span.
        uint64_t x = (uint64_t)target_ms * 100 * 256 / dur_ms;
        uint32_t p = (uint32_t)(x >> 8);
        uint32_t f = (uint32_t)(x & 0xFF);
        if (p > 99) { p = 99; f = 256; }
        uint32_t a = s_xing_toc[p];
        uint32_t b = (p < 99) ? s_xing_toc[p + 1] : 256;
        if (b < a) b = a;
        return start + span * ((uint64_t)a * 256 + (uint64_t)(b - a) * f) / 65536;
    }
    return start + (uint64_t)target_ms * span / dur_ms;
}

static esp_err_t run_mp3(const decode_source_t *src, uint32_t skip_ms)
{
    // Heap-allocate: the drmp3 struct embeds minimp3's large decoder (~7 KB);
    // keeping it off the task stack avoids overflowing the playback task.
    drmp3 *mp3 = heap_caps_calloc(1, sizeof(drmp3), MALLOC_CAP_SPIRAM);
    if (!mp3) {
        return ESP_ERR_NO_MEM;
    }
    // Non-seekable sources (HTTP streams) pass NULL seek/tell so dr_mp3 uses its
    // forward-only path instead of trying to rewind during init.
    drmp3_seek_proc seekcb = src->seek ? cb_mp3_seek : NULL;
    drmp3_tell_proc tellcb = src->tell ? cb_mp3_tell : NULL;
    if (!drmp3_init(mp3, cb_read, seekcb, tellcb, cb_mp3_meta, (void *)src, &MP3_PSRAM_CB)) {
        ESP_LOGE(TAG, "mp3 init failed");
        free(mp3);
        return ESP_FAIL;
    }
    uint32_t ch = mp3->channels;
    int16_t *pcm = heap_caps_malloc((size_t)DECODE_FRAMES * ch * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm) {
        drmp3_uninit(mp3);
        free(mp3);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = audio_open(mp3->sampleRate, 16, (uint8_t)ch);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "mp3 %" PRIu32 " Hz, %" PRIu32 " ch", mp3->sampleRate, ch);
        uint32_t rate = mp3->sampleRate ? mp3->sampleRate : 44100;
        // Stable duration. The old per-chunk byte-rate estimate kept changing on
        // screen (it drifted with VBR and stdio read-ahead). Instead:
        //  - exact when the file carries a Xing/Info/VBRI header (dr_mp3 fills in
        //    totalPCMFrameCount during init; most encoders write such a header),
        //  - otherwise estimate once from the decoder's own byte cursor after a
        //    short warmup, then lock it so the displayed total stops moving.
        bool dur_locked = false;
        if (mp3->totalPCMFrameCount != DRMP3_UINT64_MAX && mp3->totalPCMFrameCount > 0) {
            s_dur_ms = (uint32_t)(mp3->totalPCMFrameCount * 1000 / rate);
            dur_locked = true;
        }
        uint64_t cur = 0;
        // Skip a podcast intro by decoding and discarding the leading frames
        // (a byte seek needs the duration, unknown this early for files without
        // a Xing header). Used only when streaming; a downloaded MP3 is already
        // trimmed on disk.
        if (skip_ms > 0) {
            uint64_t target = (uint64_t)skip_ms * rate / 1000;
            while (cur < target) {
                drmp3_uint64 got = drmp3_read_pcm_frames_s16(mp3, DECODE_FRAMES, pcm);
                if (got == 0) break;  // file shorter than the skip
                cur += got;
            }
            s_pos_ms = (uint32_t)(cur * 1000 / rate);
        }
        for (;;) {
            // One-shot seek request from the UI: map the target time to a byte
            // offset (Xing TOC or linear), jump there and let minimp3 resync on
            // the next frame header. The position becomes the estimated target
            // (exact sample positions would need a full frame walk).
            int32_t seek = s_seek_ms;
            s_seek_ms = -1;
            if (seek >= 0 && src->seek && s_dur_ms > 0) {
                uint32_t target = (uint32_t)seek > s_dur_ms ? s_dur_ms : (uint32_t)seek;
                uint64_t off = mp3_seek_byte_for_ms(mp3, src, target, s_dur_ms);
                if (drmp3__on_seek_64(mp3, off, DRMP3_SEEK_SET)) {
                    drmp3_reset(mp3);  // flush input buffer + decoder state, clears atEnd
                    cur = (uint64_t)target * rate / 1000;
                    s_pos_ms = target;
                }
            }
            drmp3_uint64 got = drmp3_read_pcm_frames_s16(mp3, DECODE_FRAMES, pcm);
            if (got == 0) break;
            if (audio_write(pcm, (size_t)got * ch * sizeof(int16_t)) != ESP_OK) break;
            cur += got;
            s_pos_ms = (uint32_t)(cur * 1000 / rate);
            if (!dur_locked && s_pos_ms > 3000) {
                // streamLength/streamStartOffset/streamCursor exclude the ID3 and
                // Xing/tag bytes, so the audio-only byte ratio is far steadier than
                // a raw file-size ratio. Available when the source is seekable (SD).
                uint64_t total_audio =
                    (mp3->streamLength != DRMP3_UINT64_MAX && mp3->streamLength > mp3->streamStartOffset)
                        ? mp3->streamLength - mp3->streamStartOffset
                        : (uint64_t)(src->total_bytes > 0 ? src->total_bytes : 0);
                uint64_t done = (mp3->streamCursor > mp3->streamStartOffset)
                        ? mp3->streamCursor - mp3->streamStartOffset
                        : s_bytes_read;
                if (total_audio > 0 && done > 0) {
                    s_dur_ms = (uint32_t)((uint64_t)s_pos_ms * total_audio / done);
                    dur_locked = true;  // freeze: the total no longer jumps
                }
            }
        }
        audio_close();
    }
    free(pcm);
    drmp3_uninit(mp3);
    free(mp3);
    return ret;
}

static esp_err_t run_flac(const decode_source_t *src, uint32_t skip_ms)
{
    drflac_seek_proc seekcb = src->seek ? cb_flac_seek : NULL;
    drflac_tell_proc tellcb = src->tell ? cb_flac_tell : NULL;
    drflac *flac = drflac_open_with_metadata(cb_read, seekcb, tellcb, cb_flac_meta, (void *)src,
                                             &FLAC_PSRAM_CB);
    if (!flac) {
        ESP_LOGE(TAG, "flac open failed");
        return ESP_FAIL;
    }
    uint32_t ch = flac->channels;
    int16_t *pcm = heap_caps_malloc((size_t)DECODE_FRAMES * ch * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm) {
        drflac_close(flac);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = audio_open(flac->sampleRate, 16, (uint8_t)ch);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "flac %" PRIu32 " Hz, %" PRIu32 " ch", flac->sampleRate, ch);
        uint32_t rate = flac->sampleRate ? flac->sampleRate : 44100;
        if (flac->totalPCMFrameCount > 0) {
            s_dur_ms = (uint32_t)(flac->totalPCMFrameCount * 1000 / rate);
        }
        uint64_t cur = 0;
        // Skip a podcast intro: FLAC is seekable, so reuse the seek path. Only
        // effective on a seekable source (cached file); a stream has no seek.
        if (skip_ms > 0) s_seek_ms = (int32_t)skip_ms;
        for (;;) {
            int32_t seek = s_seek_ms;
            s_seek_ms = -1;
            if (seek >= 0 && drflac_seek_to_pcm_frame(flac, (uint64_t)seek * rate / 1000)) {
                cur = (uint64_t)seek * rate / 1000;
            }
            drflac_uint64 got = drflac_read_pcm_frames_s16(flac, DECODE_FRAMES, pcm);
            if (got == 0) break;
            if (audio_write(pcm, (size_t)got * ch * sizeof(int16_t)) != ESP_OK) break;
            cur += got;
            s_pos_ms = (uint32_t)(cur * 1000 / rate);
        }
        audio_close();
    }
    free(pcm);
    drflac_close(flac);
    return ret;
}

// ---- AAC (.m4a / MP4 via minimp4, ADTS direct), decoded by esp_audio_codec ----

// minimp4 reads via this callback; we back it with the seekable byte source.
// Returns 0 on success, non-zero on failure (minimp4's convention).
static int mp4_read_cb(int64_t offset, void *buffer, size_t size, void *token)
{
    const decode_source_t *s = token;
    if (!s->seek || !s->seek(s->ctx, (int)offset, 0 /* SEEK_SET */)) return -1;
    uint8_t *p = buffer;
    size_t left = size;
    while (left > 0) {
        size_t got = s->read(s->ctx, p, left);
        if (got == 0) return -1;  // short read / EOF
        p += got;
        left -= got;
    }
    return 0;
}

#define AAC_PCM_CAP_INIT 16384  // one AAC frame: 1024 (LC) or 2048 (HE) samples * 2ch * 2B

// MP4 path: demux the container, decode each AAC frame. Requires a seekable
// source (the moov sample table needs random access). Honors skip_ms and seek.
static esp_err_t run_aac_mp4(const decode_source_t *src, uint32_t skip_ms)
{
    if (!src->seek || src->total_bytes <= 0) {
        ESP_LOGW(TAG, "aac: .m4a needs a seekable source (download the episode to play it)");
        return ESP_ERR_NOT_SUPPORTED;
    }
    MP4D_demux_t mp4 = {0};
    if (!MP4D_open(&mp4, mp4_read_cb, (void *)src, src->total_bytes)) {
        ESP_LOGE(TAG, "aac: mp4 demux failed");
        return ESP_FAIL;
    }
    // Pick the AAC audio track (object type 0x40 = MPEG-4 Audio); else track 0.
    unsigned trk = 0;
    bool found = false;
    for (unsigned i = 0; i < mp4.track_count; i++) {
        if (mp4.track[i].object_type_indication == 0x40) { trk = i; found = true; break; }
    }
    if (!found && mp4.track_count == 0) { MP4D_close(&mp4); return ESP_FAIL; }
    MP4D_track_t *tr = &mp4.track[trk];
    uint32_t rate = tr->SampleDescription.audio.samplerate_hz;
    uint32_t ch   = tr->SampleDescription.audio.channelcount;
    uint32_t ts   = tr->timescale ? tr->timescale : (rate ? rate : 44100);
    if (rate == 0) rate = 44100;
    if (ch == 0 || ch > 2) ch = 2;

    esp_err_t ret = ESP_FAIL;
    void *dec = NULL;
    esp_aac_dec_cfg_t acfg = {
        .sample_rate = (int)rate, .channel = (uint8_t)ch,
        .bits_per_sample = 16, .no_adts_header = true, .aac_plus_enable = false,
    };
    if (esp_aac_dec_open(&acfg, sizeof(acfg), &dec) != ESP_AUDIO_ERR_OK || !dec) {
        ESP_LOGE(TAG, "aac: decoder open failed");
        MP4D_close(&mp4);
        return ESP_FAIL;
    }
    uint8_t *inbuf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    uint32_t in_cap = 8192;
    uint32_t pcm_cap = AAC_PCM_CAP_INIT;
    uint8_t *pcm = heap_caps_malloc(pcm_cap, MALLOC_CAP_SPIRAM);
    if (!inbuf || !pcm) { ret = ESP_ERR_NO_MEM; goto done; }

    // Exact duration from the sample table.
    if (tr->sample_count > 0) {
        unsigned fb, t0, d0;
        MP4D_frame_offset(&mp4, trk, tr->sample_count - 1, &fb, &t0, &d0);
        s_dur_ms = (uint32_t)(((uint64_t)t0 + d0) * 1000 / ts);
    }
    if (audio_open(rate, 16, (uint8_t)ch) != ESP_OK) { ret = ESP_FAIL; goto done; }
    ret = ESP_OK;

    // Starting sample: skip the intro by jumping to the first frame at/after skip_ms
    // (AAC-LC frames are independently decodable).
    unsigned ns = 0;
    if (skip_ms > 0) {
        for (; ns < tr->sample_count; ns++) {
            unsigned fb, t, d;
            MP4D_frame_offset(&mp4, trk, ns, &fb, &t, &d);
            if ((uint64_t)t * 1000 / ts >= skip_ms) break;
        }
    }
    for (; ns < tr->sample_count; ns++) {
        int32_t seek = s_seek_ms;
        s_seek_ms = -1;
        if (seek >= 0) {  // drag-to-seek: jump to the frame at that time
            unsigned k = 0;
            for (; k < tr->sample_count; k++) {
                unsigned fb, t, d;
                MP4D_frame_offset(&mp4, trk, k, &fb, &t, &d);
                if ((uint64_t)t * 1000 / ts >= (uint32_t)seek) break;
            }
            ns = (k < tr->sample_count) ? k : ns;
            esp_aac_dec_reset(dec);
        }
        unsigned fbytes = 0, tstamp = 0, dur = 0;
        MP4D_file_offset_t off = MP4D_frame_offset(&mp4, trk, ns, &fbytes, &tstamp, &dur);
        if (fbytes == 0) continue;
        if (fbytes > in_cap) {
            uint8_t *nb = heap_caps_realloc(inbuf, fbytes, MALLOC_CAP_SPIRAM);
            if (!nb) break;
            inbuf = nb; in_cap = fbytes;
        }
        if (!src->seek(src->ctx, (int)off, 0)) break;
        uint32_t rd = 0;
        while (rd < fbytes) {
            size_t g = src->read(src->ctx, inbuf + rd, fbytes - rd);
            if (g == 0) break;
            rd += g;
        }
        if (rd < fbytes) break;  // stop requested or truncated
        esp_audio_dec_in_raw_t raw = { .buffer = inbuf, .len = fbytes };
        esp_audio_dec_out_frame_t out = { .buffer = pcm, .len = pcm_cap };
        esp_audio_dec_info_t info = {0};
        esp_audio_err_t e = esp_aac_dec_decode(dec, &raw, &out, &info);
        if (e == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > pcm_cap) {
            uint8_t *nb = heap_caps_realloc(pcm, out.needed_size, MALLOC_CAP_SPIRAM);
            if (!nb) break;
            pcm = nb; pcm_cap = out.needed_size;
            out.buffer = pcm; out.len = pcm_cap;
            e = esp_aac_dec_decode(dec, &raw, &out, &info);
        }
        if (e != ESP_AUDIO_ERR_OK) continue;  // skip a bad frame, keep going
        if (out.decoded_size > 0 && audio_write(pcm, out.decoded_size) != ESP_OK) break;
        s_pos_ms = (uint32_t)((uint64_t)tstamp * 1000 / ts);
    }
    audio_close();
done:
    if (dec) esp_aac_dec_close(dec);
    free(inbuf);
    free(pcm);
    MP4D_close(&mp4);
    return ret;
}

// ADTS path: raw AAC with ADTS headers (AAC web radio, .aac files). Forward-only;
// `sniff`/`sniff_len` are the bytes already consumed for format detection.
static esp_err_t run_aac_adts(const decode_source_t *src, uint32_t skip_ms,
                              const uint8_t *sniff, size_t sniff_len)
{
    void *dec = NULL;
    if (esp_aac_dec_open(NULL, 0, &dec) != ESP_AUDIO_ERR_OK || !dec) {
        ESP_LOGE(TAG, "aac: adts decoder open failed");
        return ESP_FAIL;
    }
    uint32_t in_cap = 8192, pcm_cap = AAC_PCM_CAP_INIT;
    uint8_t *inbuf = heap_caps_malloc(in_cap, MALLOC_CAP_SPIRAM);
    uint8_t *pcm   = heap_caps_malloc(pcm_cap, MALLOC_CAP_SPIRAM);
    esp_err_t ret = ESP_FAIL;
    if (!inbuf || !pcm) { ret = ESP_ERR_NO_MEM; goto done; }

    uint32_t avail = 0;
    if (sniff_len) { memcpy(inbuf, sniff, sniff_len); avail = sniff_len; }
    bool opened = false;
    uint32_t rate = 44100, ch = 2;
    uint64_t cur = 0, skip_target = 0;  // in samples

    for (;;) {
        // Top up the input buffer.
        if (avail < in_cap) {
            size_t g = src->read(src->ctx, inbuf + avail, in_cap - avail);
            avail += g;
            if (g == 0 && avail == 0) break;  // EOF
        }
        esp_audio_dec_in_raw_t raw = { .buffer = inbuf, .len = avail };
        esp_audio_dec_out_frame_t out = { .buffer = pcm, .len = pcm_cap };
        esp_audio_dec_info_t info = {0};
        esp_audio_err_t e = esp_aac_dec_decode(dec, &raw, &out, &info);
        if (e == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > pcm_cap) {
            uint8_t *nb = heap_caps_realloc(pcm, out.needed_size, MALLOC_CAP_SPIRAM);
            if (!nb) break;
            pcm = nb; pcm_cap = out.needed_size;
            continue;
        }
        if (e == ESP_AUDIO_ERR_DATA_LACK) {
            // Need more bytes; if the source is exhausted, stop.
            size_t g = src->read(src->ctx, inbuf + avail, in_cap - avail > 0 ? in_cap - avail : 0);
            if (g == 0) break;
            avail += g;
            continue;
        }
        if (e != ESP_AUDIO_ERR_OK) break;
        uint32_t consumed = raw.consumed ? raw.consumed : 1;
        if (consumed > avail) consumed = avail;
        memmove(inbuf, inbuf + consumed, avail - consumed);
        avail -= consumed;

        if (!opened && info.sample_rate) {
            rate = info.sample_rate;
            ch = info.channel ? info.channel : 2;
            if (audio_open(rate, 16, (uint8_t)ch) != ESP_OK) break;
            opened = true;
            skip_target = (uint64_t)skip_ms * rate / 1000;
            ret = ESP_OK;
        }
        if (out.decoded_size > 0) {
            uint32_t frames = out.decoded_size / (ch * sizeof(int16_t));
            if (cur >= skip_target) {
                if (audio_write(pcm, out.decoded_size) != ESP_OK) break;
            }  // else discard (still inside the intro)
            cur += frames;
            if (rate) s_pos_ms = (uint32_t)(cur * 1000 / rate);
        }
    }
    if (opened) audio_close();
done:
    if (dec) esp_aac_dec_close(dec);
    free(inbuf);
    free(pcm);
    return ret;
}

static esp_err_t run_aac(const decode_source_t *src, uint32_t skip_ms)
{
    // Sniff the container: an MP4 (.m4a) begins with an `ftyp` box; otherwise we
    // assume raw ADTS. On a seekable source rewind after sniffing; on a stream
    // keep the sniffed bytes to feed the ADTS decoder.
    uint8_t sniff[12];
    size_t n = 0;
    while (n < sizeof(sniff)) {
        size_t g = src->read(src->ctx, sniff + n, sizeof(sniff) - n);
        if (g == 0) break;
        n += g;
    }
    bool is_mp4 = (n >= 8 && memcmp(sniff + 4, "ftyp", 4) == 0);
    if (is_mp4) {
        if (src->seek) src->seek(src->ctx, 0, 0);  // rewind; MP4 path re-reads
        return run_aac_mp4(src, skip_ms);
    }
    return run_aac_adts(src, skip_ms, sniff, n);
}

// ---- Ogg (Opus / Vorbis), decoded by esp_audio_codec's simple decoder ----

// Register the Opus and Vorbis frame decoders (into the common decoder registry)
// and the OGG simple decoder (which owns the Ogg parser). Done once; the registry
// is process-global. The OGG parser detects Opus vs Vorbis from the stream, so no
// per-stream codec configuration is needed.
static bool ogg_register_once(void)
{
    static bool done;
    if (done) return true;
    if (esp_opus_dec_register() != ESP_AUDIO_ERR_OK) { ESP_LOGE(TAG, "opus register failed"); return false; }
    if (esp_vorbis_dec_register() != ESP_AUDIO_ERR_OK) { ESP_LOGE(TAG, "vorbis register failed"); return false; }
    if (esp_ogg_dec_register() != ESP_AUDIO_ERR_OK) { ESP_LOGE(TAG, "ogg register failed"); return false; }
    done = true;
    return true;
}

#define OGG_IN_CAP       16384         // input buffer start; grown when a page needs more lookahead
#define OGG_IN_CAP_MAX   (256 * 1024)  // grow bound; one Ogg page alone can reach ~64 KB (PSRAM)
// Long Vorbis blocks (up to 8192 samples) and 120 ms Opus (5760 samples) at
// stereo 16-bit: size the initial PCM buffer to hold either without a realloc.
#define OGG_PCM_CAP_INIT 32768

// Forward-only decode of an Ogg stream (works for SD files and HTTP streams).
// No seek support (v1): the podcast intro skip discards decoded PCM instead.
static esp_err_t run_ogg(const decode_source_t *src, uint32_t skip_ms)
{
    if (!ogg_register_once()) return ESP_FAIL;

    esp_audio_simple_dec_cfg_t dcfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_OGG,
        .dec_cfg = NULL,       // the Ogg parser reads the codec config from the stream
        .cfg_size = 0,
        .use_frame_dec = false,  // let the parser split frames out of the container
    };
    esp_audio_simple_dec_handle_t dec = NULL;
    if (esp_audio_simple_dec_open(&dcfg, &dec) != ESP_AUDIO_ERR_OK || !dec) {
        ESP_LOGE(TAG, "ogg: decoder open failed");
        return ESP_FAIL;
    }

    uint32_t in_cap = OGG_IN_CAP, pcm_cap = OGG_PCM_CAP_INIT;
    uint8_t *inbuf = heap_caps_malloc(in_cap, MALLOC_CAP_SPIRAM);
    uint8_t *pcm   = heap_caps_malloc(pcm_cap, MALLOC_CAP_SPIRAM);
    esp_err_t ret = ESP_FAIL;
    if (!inbuf || !pcm) { ret = ESP_ERR_NO_MEM; goto done; }

    uint32_t avail = 0;
    bool opened = false, eof = false;
    uint32_t rate = 0, ch = 0;
    uint64_t cur = 0, skip_target = 0;  // in samples per channel
    // Chained Ogg (web radio): Icecast starts a NEW logical bitstream (fresh
    // BOS page + codec headers, new serial) at every track change. The parser
    // silently discards a chain it did not start on, so the stream would go
    // mute at the first song boundary (observed live: ~80 s then frozen).
    // Handle it here: detect a mid-stream BOS page ("OggS", version 0,
    // header_type 0x02), feed the parser only up to it, then restart the
    // parser exactly at the boundary so it reads the new chain's headers.
    uint64_t buf_base = 0;    // absolute stream offset of inbuf[0]
    int64_t  chain_at = -1;   // absolute offset of a pending chain BOS, -1 none
    uint64_t chain_done = 0;  // BOS at or below this offset already handled (0 = stream start)
    bool     chain_new = false;  // parser restarted: re-check rate/ch on next frame

    for (;;) {
        if (!eof && avail < in_cap) {
            size_t g = src->read(src->ctx, inbuf + avail, in_cap - avail);
            if (g == 0) eof = true;
            else avail += g;
        }
        if (avail == 0 && eof) break;  // fully drained

        // Scan the buffered bytes for a chain boundary. chain_done skips the
        // initial stream start and any boundary already handled (or a restart
        // would re-detect its own BOS at offset 0 forever).
        if (chain_at < 0) {
            for (uint32_t i = 0; i + 6 <= avail; i++) {
                if (inbuf[i] == 'O' && inbuf[i+1] == 'g' && inbuf[i+2] == 'g' &&
                    inbuf[i+3] == 'S' && inbuf[i+4] == 0 && inbuf[i+5] == 0x02 &&
                    buf_base + i > chain_done) {
                    chain_at = (int64_t)(buf_base + i);
                    break;
                }
            }
        }
        uint32_t feed = avail;
        if (chain_at >= 0) {
            uint32_t b = (uint32_t)(chain_at - buf_base);
            if (b == 0) {  // boundary reached: restart the parser on the new chain
                esp_audio_simple_dec_close(dec);
                dec = NULL;
                if (esp_audio_simple_dec_open(&dcfg, &dec) != ESP_AUDIO_ERR_OK || !dec) break;
                chain_done = (uint64_t)chain_at;
                chain_at = -1;
                chain_new = true;
                ESP_LOGI(TAG, "ogg: chained stream, parser restarted");
            } else {
                feed = b;  // decode the rest of the old chain first
            }
        }

        esp_audio_simple_dec_raw_t raw = { .buffer = inbuf, .len = feed, .eos = eof };
        esp_audio_simple_dec_out_t out = { .buffer = pcm, .len = pcm_cap };
        esp_audio_err_t e = esp_audio_simple_dec_process(dec, &raw, &out);
        if (e == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > pcm_cap) {
            uint8_t *nb = heap_caps_realloc(pcm, out.needed_size, MALLOC_CAP_SPIRAM);
            if (!nb) break;
            pcm = nb; pcm_cap = out.needed_size;
            continue;  // retry the same input with the bigger buffer
        }
        if (e == ESP_AUDIO_ERR_DATA_LACK || e == ESP_AUDIO_ERR_OK) {
            uint32_t consumed = (e == ESP_AUDIO_ERR_OK) ? raw.consumed : 0;
            if (consumed > feed) consumed = feed;
            if (consumed) {
                memmove(inbuf, inbuf + consumed, avail - consumed);
                avail -= consumed;
                buf_base += consumed;
            }

            if (e == ESP_AUDIO_ERR_OK && out.decoded_size > 0) {
                if (!opened) {
                    esp_audio_simple_dec_info_t info = {0};
                    esp_audio_simple_dec_get_info(dec, &info);
                    rate = info.sample_rate ? info.sample_rate : 48000;
                    ch = info.channel ? info.channel : 2;
                    ESP_LOGI(TAG, "ogg %" PRIu32 " Hz, %" PRIu32 " ch, %u bits",
                             rate, ch, (unsigned)info.bits_per_sample);
                    if (audio_open(rate, 16, (uint8_t)ch) != ESP_OK) break;
                    opened = true;
                    skip_target = (uint64_t)skip_ms * rate / 1000;
                    ret = ESP_OK;
                } else if (chain_new) {
                    // First frame of a new chain: reopen the output if the
                    // format changed (a station may switch rate or channels).
                    esp_audio_simple_dec_info_t info = {0};
                    esp_audio_simple_dec_get_info(dec, &info);
                    uint32_t nr = info.sample_rate ? info.sample_rate : rate;
                    uint32_t nc2 = info.channel ? info.channel : ch;
                    if (nr != rate || nc2 != ch) {
                        ESP_LOGI(TAG, "ogg: chain format change %" PRIu32 "/%" PRIu32
                                 " -> %" PRIu32 "/%" PRIu32, rate, ch, nr, nc2);
                        audio_close();
                        if (audio_open(nr, 16, (uint8_t)nc2) != ESP_OK) break;
                        rate = nr; ch = nc2;
                    }
                }
                chain_new = false;
                uint32_t frames = out.decoded_size / (ch * sizeof(int16_t));
                if (cur >= skip_target) {
                    if (audio_write(pcm, out.decoded_size) != ESP_OK) break;
                }  // else discard (still inside the podcast intro)
                cur += frames;
                if (rate) s_pos_ms = (uint32_t)(cur * 1000 / rate);
            }

            if (consumed == 0 && out.decoded_size == 0) {  // no forward progress
                if (chain_at >= 0) {
                    // Tail of the old chain the parser will not take (partial
                    // page): drop it and restart at the boundary next round.
                    uint32_t b = (uint32_t)(chain_at - buf_base);
                    if (b > 0) {
                        memmove(inbuf, inbuf + b, avail - b);
                        avail -= b;
                        buf_base += b;
                    }
                    continue;
                }
                if (eof) break;
                if (avail == in_cap) {  // buffer full: grow so a large page fits
                    if (in_cap >= OGG_IN_CAP_MAX) {
                        ESP_LOGE(TAG, "ogg: no progress with a full %d B buffer, giving up",
                                 OGG_IN_CAP_MAX);
                        break;
                    }
                    uint32_t nc = in_cap * 2 > OGG_IN_CAP_MAX ? OGG_IN_CAP_MAX : in_cap * 2;
                    uint8_t *nb = heap_caps_realloc(inbuf, nc, MALLOC_CAP_SPIRAM);
                    if (!nb) break;
                    inbuf = nb; in_cap = nc;
                    ESP_LOGI(TAG, "ogg: input buffer grown to %" PRIu32, in_cap);
                }
            }
            continue;
        }
        // Any other error: corrupt/unsupported data, stop cleanly (partial
        // playback is fine).
        ESP_LOGW(TAG, "ogg: decode error %d", (int)e);
        break;
    }
    if (opened) audio_close();
done:
    if (dec) esp_audio_simple_dec_close(dec);
    free(inbuf);
    free(pcm);
    return ret;
}

esp_err_t decode_run(decode_format_t fmt, const decode_source_t *src)
{
    if (!src || !src->read) {
        return ESP_ERR_INVALID_ARG;
    }
    s_pos_ms = 0;
    s_dur_ms = 0;
    s_seek_ms = -1;
    s_bytes_read = 0;
    s_xing_toc_valid = false;
    uint32_t skip = s_start_skip_ms;  // one-shot, consume it
    s_start_skip_ms = 0;
    decode_clear_metadata();
    switch (fmt) {
    case DECODE_FORMAT_MP3:  return run_mp3(src, skip);
    case DECODE_FORMAT_FLAC: return run_flac(src, skip);
    case DECODE_FORMAT_AAC:  return run_aac(src, skip);
    case DECODE_FORMAT_OGG:  return run_ogg(src, skip);
    default:                 return ESP_ERR_INVALID_ARG;
    }
}

// Lightweight tag reader for the library scanner. Unlike the playback path
// (dr_mp3/dr_flac with onMeta, which loads the WHOLE tag including embedded cover
// art into a heap buffer per file), this parses the metadata incrementally and
// fseek-skips large frames (ID3 APIC, FLAC PICTURE). Per-file memory stays tiny
// and constant, so scanning thousands of files does not fragment the heap.

static bool src_read_n(const decode_source_t *s, void *buf, size_t n)
{
    size_t got = 0;
    uint8_t *p = buf;
    while (got < n) {
        size_t r = s->read(s->ctx, p + got, n - got);
        if (r == 0) return false;
        got += r;
    }
    return true;
}

static bool src_skip(const decode_source_t *s, long n)
{
    if (n <= 0) return true;
    if (s->seek) return s->seek(s->ctx, (int)n, 1 /* SEEK_CUR */);
    uint8_t tmp[256];
    while (n > 0) {
        size_t c = n < (long)sizeof(tmp) ? (size_t)n : sizeof(tmp);
        size_t r = s->read(s->ctx, tmp, c);
        if (r == 0) return false;
        n -= r;
    }
    return true;
}

// ID3v2: read frame headers, decode the wanted text frames, skip the rest.
static void scan_id3v2(const decode_source_t *s)
{
    uint8_t h[10];
    if (!src_read_n(s, h, 10)) return;
    if (h[0] != 'I' || h[1] != 'D' || h[2] != '3') return;  // no ID3v2 at the start
    uint8_t ver = h[3], flags = h[5];
    uint32_t tagsize = ((uint32_t)(h[6] & 0x7F) << 21) | ((h[7] & 0x7F) << 14) |
                       ((h[8] & 0x7F) << 7) | (h[9] & 0x7F);
    bool v22 = (ver == 2);
    uint32_t pos = 0;
    if (!v22 && (flags & 0x40)) {  // skip extended header
        uint8_t e[4];
        if (!src_read_n(s, e, 4)) return;
        pos += 4;
        uint32_t ext = (ver == 4)
            ? (((uint32_t)(e[0] & 0x7F) << 21) | ((e[1] & 0x7F) << 14) | ((e[2] & 0x7F) << 7) | (e[3] & 0x7F))
            : (((uint32_t)e[0] << 24) | ((uint32_t)e[1] << 16) | ((uint32_t)e[2] << 8) | e[3]);
        if (!src_skip(s, ext)) return;
        pos += ext;
    }
    size_t hdrlen = v22 ? 6 : 10;
    while (pos + hdrlen <= tagsize) {
        uint8_t fh[10];
        if (!src_read_n(s, fh, hdrlen)) return;
        pos += hdrlen;
        if (fh[0] == 0) break;  // padding
        uint32_t fsize;
        if (v22)            fsize = ((uint32_t)fh[3] << 16) | ((uint32_t)fh[4] << 8) | fh[5];
        else if (ver == 4)  fsize = ((uint32_t)(fh[4] & 0x7F) << 21) | ((fh[5] & 0x7F) << 14) |
                                    ((fh[6] & 0x7F) << 7) | (fh[7] & 0x7F);
        else                fsize = ((uint32_t)fh[4] << 24) | ((uint32_t)fh[5] << 16) |
                                    ((uint32_t)fh[6] << 8) | fh[7];
        if (fsize == 0 || pos + fsize > tagsize) break;
        bool title   = v22 ? !memcmp(fh, "TT2", 3) : !memcmp(fh, "TIT2", 4);
        bool artist  = v22 ? !memcmp(fh, "TP1", 3) : !memcmp(fh, "TPE1", 4);
        bool aartist = v22 ? !memcmp(fh, "TP2", 3) : !memcmp(fh, "TPE2", 4);  // album artist
        bool album   = v22 ? !memcmp(fh, "TAL", 3) : !memcmp(fh, "TALB", 4);
        bool track   = v22 ? !memcmp(fh, "TRK", 3) : !memcmp(fh, "TRCK", 4);
        bool want = (title && !s_title[0]) || (artist && !s_artist[0]) || (aartist && !s_album_artist[0]) ||
                    (album && !s_album[0]) || (track && !s_track);
        if (want) {
            uint8_t buf[256];
            uint32_t toread = fsize < sizeof(buf) ? fsize : sizeof(buf);
            if (!src_read_n(s, buf, toread)) return;
            if (fsize > toread && !src_skip(s, fsize - toread)) return;
            if      (title   && !s_title[0])        id3_text(s_title,        sizeof(s_title),        buf, toread);
            else if (artist  && !s_artist[0])       id3_text(s_artist,       sizeof(s_artist),       buf, toread);
            else if (aartist && !s_album_artist[0]) id3_text(s_album_artist, sizeof(s_album_artist), buf, toread);
            else if (album   && !s_album[0])        id3_text(s_album,        sizeof(s_album),        buf, toread);
            else if (track   && !s_track) {
                char t[64];
                id3_text(t, sizeof(t), buf, toread);
                s_track = atoi(t);
            }
        } else if (!src_skip(s, fsize)) {  // skip APIC and everything else
            return;
        }
        pos += fsize;
        if (s_title[0] && s_artist[0] && s_album_artist[0] && s_album[0] && s_track) break;
    }
}

// Parse a Vorbis comment block already read into memory.
static void parse_vorbis(const uint8_t *p, uint32_t n)
{
    uint32_t off = 0;
    if (off + 4 > n) return;
    uint32_t vlen = p[off] | (p[off+1] << 8) | (p[off+2] << 16) | ((uint32_t)p[off+3] << 24);
    if (vlen > n - off - 4) return;  // wrap-safe: skip vendor string within bounds
    off += 4 + vlen;
    if (off + 4 > n) return;
    uint32_t cnt = p[off] | (p[off+1] << 8) | (p[off+2] << 16) | ((uint32_t)p[off+3] << 24);
    off += 4;
    for (uint32_t i = 0; i < cnt; i++) {
        if (off + 4 > n) return;
        uint32_t clen = p[off] | (p[off+1] << 8) | (p[off+2] << 16) | ((uint32_t)p[off+3] << 24);
        off += 4;
        if (clen > n - off) return;  // wrap-safe (off <= n here)
        const char *c = (const char *)(p + off);
        if      (clen > 6  && !strncasecmp(c, "TITLE=", 6)  && !s_title[0])  copy_utf8(s_title,  sizeof(s_title),  c + 6,  clen - 6);
        else if (clen > 12 && !strncasecmp(c, "ALBUMARTIST=", 12) && !s_album_artist[0]) copy_utf8(s_album_artist, sizeof(s_album_artist), c + 12, clen - 12);
        else if (clen > 7  && !strncasecmp(c, "ARTIST=", 7) && !s_artist[0]) copy_utf8(s_artist, sizeof(s_artist), c + 7,  clen - 7);
        else if (clen > 6  && !strncasecmp(c, "ALBUM=", 6)  && !s_album[0])  copy_utf8(s_album,  sizeof(s_album),  c + 6,  clen - 6);
        else if (clen > 12 && !strncasecmp(c, "TRACKNUMBER=", 12) && !s_track) {
            char t[16];
            copy_utf8(t, sizeof(t), c + 12, clen - 12);
            s_track = atoi(t);
        }
        off += clen;
    }
}

// FLAC: walk metadata blocks, read only VORBIS_COMMENT, skip PICTURE etc.
static void scan_flac(const decode_source_t *s)
{
    uint8_t m[4];
    if (!src_read_n(s, m, 4) || memcmp(m, "fLaC", 4) != 0) return;
    bool last = false;
    while (!last) {
        uint8_t bh[4];
        if (!src_read_n(s, bh, 4)) return;
        last = bh[0] & 0x80;
        uint8_t type = bh[0] & 0x7F;
        uint32_t len = ((uint32_t)bh[1] << 16) | ((uint32_t)bh[2] << 8) | bh[3];
        if (type == 4) {  // VORBIS_COMMENT (small; cap defensively)
            uint32_t cap = len < 8192 ? len : 8192;
            uint8_t *vc = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
            if (!vc) { src_skip(s, len); return; }
            if (!src_read_n(s, vc, cap)) { free(vc); return; }
            if (len > cap) src_skip(s, len - cap);
            parse_vorbis(vc, cap);
            free(vc);
        } else if (!src_skip(s, len)) {  // skip PICTURE (art) and the rest
            return;
        }
    }
}

esp_err_t decode_read_tags(decode_format_t fmt, const decode_source_t *src, decode_tags_t *out)
{
    if (!src || !src->read || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    decode_clear_metadata();
    if      (fmt == DECODE_FORMAT_MP3)  scan_id3v2(src);
    else if (fmt == DECODE_FORMAT_FLAC) scan_flac(src);
    else if (fmt == DECODE_FORMAT_OGG)  { /* no Vorbis-comment parse in v1: empty tags */ }
    else return ESP_ERR_INVALID_ARG;
    out->track = s_track;
    strlcpy(out->title,        s_title,        sizeof(out->title));
    strlcpy(out->artist,       s_artist,       sizeof(out->artist));
    strlcpy(out->album_artist, s_album_artist, sizeof(out->album_artist));
    strlcpy(out->album,        s_album,        sizeof(out->album));
    return ESP_OK;
}
