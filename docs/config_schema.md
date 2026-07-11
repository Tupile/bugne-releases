# Config schema

This is a contract the rest of the firmware depends on. Stored in internal
flash (LittleFS) at `/littlefs/config.json`. It is always present and works
without an SD card.

Secrets are never in this file. Wi-Fi credentials and the hashed config-page
password live in NVS, written by the config web page.

## Location

- LittleFS partition (see `partitions.csv`), path `/littlefs/config.json`.

## Schema (version 1)

```json
{
  "schema_version": 1,
  "device": {
    "name": "Bugne-A1B2"
  },
  "webradios": [
    { "id": 1, "name": "FIP", "url": "https://icecast.example/fip", "skip_preroll": 0 }
  ],
  "podcasts": [
    { "id": 1, "title": "Example Show", "rss_url": "https://example/feed.xml", "skip_seconds": 0 }
  ],
  "ui": {
    "volume": 60,
    "volume_max": 100,
    "screen_sleep_seconds": 30,
    "lang": "en",
    "orientation": 0,
    "dark": 1,
    "accent": 0,
    "game": 1,
    "tuner": 1,
    "tz": "CET-1CEST,M3.5.0,M10.5.0/3"
  },
  "alarms": [
    { "enabled": 0, "hour": 7, "minute": 0, "days": 127,
      "source": 0, "radio_id": 0, "sd_path": "", "sd_title": "", "volume": 50, "sunrise": 5 },
    { "enabled": 0, "hour": 7, "minute": 0, "days": 127,
      "source": 0, "radio_id": 0, "sd_path": "", "sd_title": "", "volume": 50, "sunrise": 5 },
    { "enabled": 0, "hour": 7, "minute": 0, "days": 127,
      "source": 0, "radio_id": 0, "sd_path": "", "sd_title": "", "volume": 50, "sunrise": 5 }
  ],
  "quiet": [
    { "enabled": 0, "start_hour": 19, "start_minute": 0,
      "end_hour": 7, "end_minute": 0, "days": 127 },
    { "enabled": 0, "start_hour": 13, "start_minute": 0,
      "end_hour": 14, "end_minute": 0, "days": 127 }
  ],
  "favorites": [
    { "type": 0, "radio_id": 1, "path": "", "title": "FIP" },
    { "type": 1, "radio_id": 0, "path": "Music/Album/track.mp3", "title": "Track" }
  ]
}
```

## Fields

| Field | Type | Notes |
| --- | --- | --- |
| `schema_version` | int | Schema version. Currently 1. Bump on breaking changes. |
| `device.name` | string | Friendly display name. The unique device ID is derived from the MAC, not stored here. |
| `webradios[].id` | int | Stable small integer, unique within the list. |
| `webradios[].name` | string | Display name. |
| `webradios[].url` | string | Stream or playlist URL (`.m3u`/`.pls` allowed). |
| `webradios[].skip_preroll` | int | 0/1, default 0. When 1, open a short decoy connection before playing so a server-inserted pre-roll ad is absorbed by the decoy instead of being heard. |
| `podcasts[].id` | int | Stable small integer, unique within the list. Names the flash manifest file (`/littlefs/podcasts/<id>.json`). Max 50 entries (`CFG_MAX_PODCASTS`); entries beyond that are dropped on load. |
| `podcasts[].title` | string | Display title. |
| `podcasts[].rss_url` | string | Podcast RSS feed URL. |
| `podcasts[].skip_seconds` | int | Intro/ads to skip (0 = none). Trimmed off each MP3 at download, and skipped at the start of streamed playback. |
| `ui.volume` | int | 0 to 100. |
| `ui.volume_max` | int | Volume ceiling 1 to 100 (child-ear protection); every volume request is clamped to it. Default 100. |
| `ui.screen_sleep_seconds` | int | Idle seconds before the screen sleeps. |
| `ui.lang` | string | UI language ISO code: `en` (default) or `fr`. |
| `ui.orientation` | int | 0 = portrait (default), 1 = landscape. |
| `ui.dark` | int | Theme mode: 0 light, 1 dark (default). |
| `ui.accent` | int | Button/accent color, 0 to 4: 0 Blue (default), 1 Ocean, 2 Pink, 3 Forest, 4 Orange. |
| `ui.game` | int | Times-tables game on the home screen: 1 shown (default), 0 hidden. |
| `ui.tuner` | int | Instrument tuner on the home screen: 1 shown (default), 0 hidden. |
| `ui.tz` | string | POSIX TZ string, used for the wall clock and the alarm. Default `CET-1CEST,M3.5.0,M10.5.0/3` (Paris). Set from the web Settings page, live-applied (no reboot). |
| `alarms[]` | array | Up to 3 alarms (`CFG_MAX_ALARMS`), e.g. weekday / weekend / free use. Each entry has the fields below. A legacy single `alarm` object (pre-B3 firmware) is still read forever and maps to `alarms[0]`; it is parsed BEFORE `alarms`, so if a config carries both, the array wins. Only `alarms` is written back. |
| `alarms[].enabled` | int | 0 (default) or 1. |
| `alarms[].hour` | int | 0 to 23. Default 7. |
| `alarms[].minute` | int | 0 to 59. Default 0. |
| `alarms[].days` | int | Weekday bitmask: bit0 = Monday .. bit6 = Sunday. 0 or missing is coerced to 127 (every day) on load, so the alarm is never silently disabled by an empty mask. |
| `alarms[].source` | int | 0 = web radio (default), 1 = SD track. |
| `alarms[].radio_id` | int | The stable `webradios[].id` to play, not the array index (survives reorder/delete on the web page). An id that no longer resolves falls back to the beep tone at fire time. |
| `alarms[].sd_path` | string | SD track path, relative to the SD root. Chosen on the web page. Empty when unset. |
| `alarms[].sd_title` | string | Display title of the chosen SD track. Empty when unset. |
| `alarms[].volume` | int | Alarm ramp target, 5 to 100. Runtime-clamped by `ui.volume_max` when it fires. Default 50. |
| `alarms[].sunrise` | int | Sunrise light: minutes of progressive backlight ramp before the fire time. 0 = off, else 1 to 15. Default 5; a missing field loads as 5 (same policy as the other alarm defaults). Only acts while the screen is asleep and nothing is playing; see the sunrise paragraph below. |
| `quiet[].enabled` | int | 0 (default) or 1. |
| `quiet[].start_hour` / `quiet[].start_minute` | int | 0 to 23 / 0 to 59. Start of the window. |
| `quiet[].end_hour` / `quiet[].end_minute` | int | 0 to 23 / 0 to 59. End of the window. |
| `quiet[].days` | int | Weekday bitmask: bit0 = Monday .. bit6 = Sunday, the day the window STARTS. 0 or missing is coerced to 127 (every day) on load. |
| `favorites[].type` | int | 0 = web radio (by stable id), 1 = SD track path. Max 12 entries. |
| `favorites[].radio_id` | int | The stable `webradios[].id` (type 0), not the array index. An id that no longer resolves shows as unavailable on the device. |
| `favorites[].path` | string | SD path relative to the SD root (type 1). Required for type 1, empty otherwise. |
| `favorites[].title` | string | Display title. |

The alarm engine (ui.c, 1 Hz tick) checks all 3 alarms every second. When
several are due on the same minute, the lowest index fires (the others wait
for their next scheduled day); only one alarm can ring or be snoozed at a
time (`s_alarm_active_idx` records which). Firing: wakes the screen, plays
the configured source (or a generated beep if the source cannot play), ramps
the volume up over 60 s, and shows a dedicated ringing screen with Stop and
Snooze (+10 min). Snooze is RAM-only (an epoch timestamp, forgotten on
reboot). There is no catch-up firing: a missed alarm during a reboot, or
while ringing or snoozed, is not replayed; the next scheduled day fires
normally.

Sunrise light: during the `sunrise` minutes before an alarm fires, if the
screen is asleep and nothing is playing, the device turns the panel back on
with a minimal dark clock screen and ramps the backlight (LEDC PWM)
linearly from 5% to 100% until the fire time, when the ringing screen takes
over. A touch cancels the ramp (normal wake to home; the same occurrence
does not re-enter). Disabling the alarm mid-ramp restores the sleeping
screen; playback starting mid-ramp also cancels. Quiet hours never block it
(the alarm exemption applies).

Quiet hours are up to 2 parental no-playback windows. A window is half-open:
it blocks from the start time up to, but not including, the end time. A
window that crosses midnight (start later than end) belongs to its start
day: the part after midnight checks the previous day's bit, not the current
day's. A window where start equals end is off. Before the first SNTP sync
nothing is blocked, since there is no reliable time yet. The alarm and its
beep fallback always sound, even during a quiet window. Podcast downloads
and auto-maintenance keep running during quiet hours. Quiet hours are
configured on the web page only, the device never writes this object.

Favorites are up to 12 quick-play entries shown behind a Favorites home tile
(the tile is hidden when the list is empty). The device adds and removes the
currently playing content through the star button on the now-playing screen
(`config_store_favorite_add`/`config_store_favorite_remove`); the web Play
tab lists, reorders and deletes them through the full-config save.

## Rules

- The config web page and the firmware both read and write this file.
- Validate on read: missing optional fields fall back to defaults, an unknown
  `schema_version` is rejected.
- Bound all string lengths and list sizes when parsing.
