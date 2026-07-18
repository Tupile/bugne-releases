// memo: voice memos recorded with the onboard mic, stored on the SD card as
// WAV (PCM 16-bit mono 16 kHz), replayable locally or sent to another Bugne
// on the LAN over plain HTTP (POST /api/memo?from=<name>).
//
// File layout under /sdcard/memos/ (scan of this directory is the only state,
// no NVS, no wall-clock dependency):
//   my-NNN.wav              own kept memo
//   rx-<sender>-NNN.new.wav received, not listened to yet (unread)
//   rx-<sender>-NNN.wav     received, listened to
//   tk-NNN.wav              ephemeral walkie-talkie message (auto-played then
//                           deleted; memo_name_parse rejects it, so it never
//                           appears in the list, badge, or 20-memo cap)
//   .rec.wav / *.part       in-flight temporaries, ignored by every scan
//
// memo_wav.c and memo_name.c are pure (no ESP includes), host-tested in
// test/host/test_memo_wav.c and test_memo_name.c.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MEMO_RATE_HZ         16000
#define MEMO_MAX_MS          60000                // recording cap
#define MEMO_MAX_COUNT       20                   // stored memos cap (own + received)
#define MEMO_RX_MAX_BYTES    (2 * 1024 * 1024)    // 60 s @ 32 kB/s + headers
#define MEMO_DIR             "memos"              // relative to the SD root
#define MEMO_ABS_DIR         "/sdcard/" MEMO_DIR
#define MEMO_REC_NAME        ".rec.wav"           // finalized capture awaiting Keep/Send
#define MEMO_TK_PREFIX       "tk-"                // ephemeral walkie-talkie files
#define MEMO_SENDER_MAX      24
#define MEMO_NAME_MAX        64
#define MEMO_WAV_HEADER_BYTES 44

// ---- memo_wav.c (pure) ----

// Write the canonical 44-byte PCM WAV header (16-bit mono MEMO_RATE_HZ).
void memo_wav_header(uint8_t out[MEMO_WAV_HEADER_BYTES], uint32_t data_bytes);

// Lenient parse of a WAV prefix (walks RIFF chunks, so ffmpeg files with a
// LIST chunk pass too). True only for PCM 16-bit mono MEMO_RATE_HZ; fills the
// byte offset and size of the data chunk.
bool memo_wav_parse(const uint8_t *buf, size_t len, uint32_t *data_off, uint32_t *data_len);

// Normalize a finished memo WAV in place to a comfortable level: scan the
// data chunk's peak, then scale every sample so the peak lands near -1 dBFS.
// Gain is capped at x16 so silence or room noise is never blown up; a file
// already at level is left untouched. f must be open "r+b"; buf (>= 512
// bytes, even size) is the caller's work buffer. False on I/O/parse errors.
bool memo_wav_normalize(FILE *f, uint8_t *buf, size_t buf_len);

// ---- memo_name.c (pure) ----

// Keep [A-Za-z0-9_-], map anything else to '-' (runs collapsed), truncate.
// Empty input becomes "peer". dst holds at most MEMO_SENDER_MAX chars.
void memo_sanitize_sender(char *dst, size_t size, const char *src);

// Build the stored file names.
void memo_name_mine(char *dst, size_t size, int seq);                    // my-NNN.wav
void memo_name_rx(char *dst, size_t size, const char *sender, int seq);  // rx-<sender>-NNN.new.wav

// Parse a stored name. False for anything else (temporaries, foreign files).
bool memo_name_parse(const char *name, bool *is_mine, char *sender, size_t sender_size,
                     int *seq, bool *unread);

// ---- memo_store.c (SD layer) ----

typedef struct {
    char name[MEMO_NAME_MAX];      // file name inside /sdcard/memos
    char sender[MEMO_SENDER_MAX];  // empty for own memos
    bool is_mine;
    bool unread;
    int  seq;
    int  duration_s;
} memo_entry_t;

// Absolute path of a stored memo.
void memo_abs_path(char *dst, size_t size, const char *name);

// Fill out (up to max) sorted newest-first (seq descending). Returns the
// number of entries written; 0 when the directory is missing or SD absent.
int memo_list(memo_entry_t *out, int max);

int memo_count(void);
int memo_unread_count(void);

// Rename the finalized capture (.rec.wav) to my-NNN.wav. Returns seq or -1.
int memo_keep_rec(void);

// Allocate a receive slot: opens "<final_abs>.part" for exclusive writing and
// fills both paths. sender must already be sanitized. NULL when SD is absent
// or no name can be allocated.
FILE *memo_rx_create(const char *sender, char *final_abs, size_t final_size,
                     char *part_abs, size_t part_size);

// Allocate an ephemeral walkie-talkie slot (tk-NNN.wav), same contract as
// memo_rx_create. Exempt from the 20-memo cap by construction.
FILE *memo_tk_create(char *final_abs, size_t final_size,
                     char *part_abs, size_t part_size);

// Delete leftover temporaries (*.part, .rec.wav). Call once after SD mount.
void memo_clean_parts(void);

// Delete every ephemeral walkie-talkie file (tk-*). Call after SD mount and
// when a talkie session ends. Never touches .rec.wav (a send may hold it).
void memo_clean_talkie(void);

// ---- memo_send.c ----

// Stream a stored WAV to a peer as POST http://<ip>:<port>/api/memo?from=<from>
// (&talkie=1 appended when talkie). from must already be sanitized. pct (may be
// NULL) gets 0..100 progress; http_status (may be NULL) gets the peer's answer.
// ESP_OK only when the whole file was sent and the peer answered 200 or 202
// (202 = talkie message stored as a normal memo, receiver not in talkie mode).
esp_err_t memo_send(const char *ip, uint16_t port, const char *from,
                    const char *abs_path, bool talkie, int *http_status,
                    volatile int *pct);

#ifdef __cplusplus
}
#endif
