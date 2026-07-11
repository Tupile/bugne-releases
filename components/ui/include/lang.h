// lang: tiny UI internationalization (i18n) for the on-screen text.
//
// String IDs index a [language][id] table. T(id) returns the string in the current
// language. Adding a language later = add a column in lang.c (a new lang_def_t with
// its strings) and bump nothing else; adding a string = add an STR_* id plus its
// translations. Log messages are NOT translated (they stay English).
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STR_NOW_PLAYING,
    STR_WEBRADIOS,
    STR_PODCASTS,
    STR_LIBRARY,
    STR_ARTISTS,
    STR_ALBUMS,
    STR_EPISODES,
    STR_SDCARD,
    STR_SETTINGS,
    STR_CONFIG_PAGE,
    STR_SETUP_WIFI,
    STR_MUSIC_LIBRARY,
    STR_BY_ARTIST,
    STR_BY_ALBUM,
    STR_CONFIG_PAGE_QR,
    STR_SETUP_HOTSPOT_QR,
    STR_SYNC_LIBRARY,
    STR_REFRESH_PODCASTS,
    STR_NO_SD,
    STR_NO_EPISODES,
    STR_EMPTY_LIBRARY,
    STR_REFRESHING_FEED,
    STR_REFRESHING_ALL,
    STR_REFRESHING,
    STR_SCAN_JOIN_WIFI,
    STR_CONFIG_AFTER_JOIN,
    STR_SCAN_CONFIG_PAGE,
    STR_SCAN_HOTSPOT,
    STR_ACTIVE_DURING_SETUP,
    STR_SCANNING_SD,
    STR_SYNC_NOW,
    STR_REFRESH_ALL_FEEDS,
    STR_STOP_PLAYBACK_FIRST,
    STR_REFRESH_DONE_FMT,   // printf with one %u (feed count)
    STR_SETUP_HEADER_FMT,   // printf with one %s (device id)
    STR_LANGUAGE,
    STR_NO_WEBRADIOS,
    STR_NO_PODCASTS,
    STR_EMPTY_FOLDER,
    STR_TRACKS_INDEXED_FMT, // printf with one %u (track count)
    STR_PODCAST_FEEDS_FMT,  // printf with one %u (feed count)
    STR_CONNECTING,
    STR_RECONNECTING,
    STR_STREAM_FAILED,
    STR_PLAYBACK_FAILED,
    STR_REFRESH_FAILED,
    STR_END_OF_LIST,
    STR_WAITING_WIFI,
    STR_GAME,
    STR_SCORE_FMT,
    STR_BEST_FMT,
    STR_CORRECT_FMT,
    STR_TRY_AGAIN,
    STR_NEW_BEST,
    STR_GAME_PICK_TABLES,   // game setup screen title
    STR_GAME_ALL,           // game setup: check every table
    STR_GAME_NONE,          // game setup: uncheck every table
    STR_GAME_PICK_ONE,      // toast: tried to start with no table checked
    STR_THEME,
    STR_THEME_LIGHT,
    STR_THEME_DARK,
    STR_THEME_BLUE,
    STR_THEME_OCEAN,
    STR_THEME_PINK,
    STR_THEME_FOREST,
    STR_THEME_ORANGE,
    STR_ALARM,
    STR_ALARM_ENABLED,
    STR_ALARM_VOLUME,
    STR_ALARM_SOURCE,
    STR_ALARM_SD_UNSET,
    STR_ALARM_BEEP,
    STR_ALARM_N_FMT,        // printf with one %d (1-based alarm index), e.g. "Alarm %d"
    STR_ALARM_OFF,          // alarm list row: shown instead of the day letters when disabled
    STR_QUIET_HOURS,
    STR_DAY_LETTERS,        // 7 chars, one per weekday, Monday first
    STR_SNOOZE_UNTIL_FMT,   // printf with two %02d (hour, minute)
    STR_TIME_NOT_SET,
    STR_SLEEP_OFF,          // toast: sleep timer turned off
    STR_SLEEP_SET_FMT,      // toast: printf with one %d (minutes armed)
    STR_SLEEP_SET_EOT,      // toast: sleep timer armed for end-of-track
    STR_SLEEP_REMAIN_FMT,   // now-playing label: printf with one %d (minutes left)
    STR_SLEEP_REMAIN_EOT,   // now-playing label: end-of-track mode
    STR_SLEEP_STOPPED,      // toast: the sleep timer just stopped playback
    STR_FAVORITES,          // home tile + favorites screen title
    STR_NO_FAVORITES,       // favorites screen empty state
    STR_FAV_ADDED,          // toast: current content added to favorites
    STR_FAV_REMOVED,        // toast: current content removed from favorites
    STR_FAV_LIST_FULL,      // toast: favorites list at CFG_MAX_FAVORITES
    STR_FAV_MISSING,        // toast: favorite's radio/file no longer exists
    STR_FAV_OFFLINE,        // toast: radio favorite tapped without Wi-Fi
    STR_STREAK_FMT,
    STR_MAX_STREAK_FMT,
    STR_RESET_MSG,
    STR_TUNER,              // home tile + tuner screen title
    STR_TUNER_PLAY_NOTE,    // tuner idle state: waiting for a note
    STR__COUNT
} str_id_t;

// The translated string for id in the current language.
const char *T(str_id_t id);

// Select the active language by ISO code ("en", "fr"). Unknown codes fall back to
// the first language (English). Safe to call from the UI task.
void lang_set_code(const char *code);

// The active language's ISO code.
const char *lang_code(void);

// Languages available, for building a selector.
size_t lang_count(void);
const char *lang_code_at(size_t i);   // "en", "fr"
const char *lang_name_at(size_t i);   // "English", "Francais" (in their own language)

#ifdef __cplusplus
}
#endif
