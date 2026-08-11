# Config schema

This file is a contract. The rest of the firmware depends on it. The device
stores it in internal flash (LittleFS), at `/littlefs/config.json`. It is
always present, and it works without an SD card.

This file holds no secret. NVS holds the Wi-Fi credentials, the hashed web page
password and the Home Assistant token. NVS also holds the state that is not
configuration:

- the usage counter of the day (`use_day` and `use_sec`),
- the best score of the game and its Leitner levels (`leitner`, 100 bytes),
- the resume position of the last episode,
- the pending download job.

## Location

- LittleFS partition (see `partitions.csv`), path `/littlefs/config.json`.

## Schema (version 1)

```json
{
  "schema_version": 1,
  "device": {
    "name": "Bugne-A1B2"
  },
  "ha": {
    "url": "http://homeassistant.local:8123",
    "entity_id": "light.kids_room"
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
    "memo_rx": 1,
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
  "daily_limit": { "enabled": 0, "minutes": 120 },
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
| `ha.url` | string | Home Assistant base URL (e.g., `http://homeassistant.local:8123`). The Long-Lived Access Token is stored securely in NVS, not in this file. |
| `ha.entity_id` | string | Home Assistant entity ID to toggle (e.g., `light.ceiling`). The Lamp tile appears if this and the URL are set (experimental feature). |
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
| `ui.tuner` | int | Instrument tuner on the home screen (experimental feature): 1 shown (default), 0 hidden. |
| `ui.memo_rx` | int | Accept voice memos from other Bugnes: 1 yes (default), 0 refuse. The receive route `POST /api/memo?from=<name>` needs no authentication on the LAN, because a peer does not know the password. It is bounded instead: the content length is required and capped at 2 MB, the device stores 20 memos at most (own plus received, and it refuses the next ones instead of deleting any), it checks the WAV format (PCM 16-bit mono 16 kHz), it sanitizes the sender name, and it chooses the storage path itself (`/sdcard/memos/`). |
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
| `daily_limit.enabled` | int | 0 (default) or 1. Parental daily usage limit. |
| `daily_limit.minutes` | int | Maximum usage minutes per local day, 5 to 720. Default 120. |
| `favorites[].type` | int | 0 = web radio (by stable id), 1 = SD track path. Max 12 entries. |
| `favorites[].radio_id` | int | The stable `webradios[].id` (type 0), not the array index. An id that no longer resolves shows as unavailable on the device. |
| `favorites[].path` | string | SD path relative to the SD root (type 1). Required for type 1, empty otherwise. |
| `favorites[].title` | string | Display title. |

The alarm engine (ui.c, 1 Hz tick) checks the 3 alarms every second. When
several alarms are due in the same minute, the lowest index fires. The others
wait for their next scheduled day. Only one alarm rings or is snoozed at a
time, and `s_alarm_active_idx` records which one. To fire, the engine wakes the
screen and plays the configured source. It then raises the volume over 60 s and
shows the ringing screen, with Stop and Snooze (+10 min). It plays a generated
beep when the source cannot play. Snooze lives in RAM only: it is an epoch timestamp, and
a reboot forgets it. The engine never catches up. It does not replay an alarm
missed during a reboot, or missed while another alarm rings or is snoozed. The
next scheduled day fires normally.

Sunrise light: during the `sunrise` minutes before an alarm fires, the device
lights the panel again with a minimal dark clock screen. It then ramps the
backlight (LEDC PWM) linearly from 5% to 100%, until the fire time. The ringing
screen then takes over. The ramp starts only when the screen sleeps and nothing
plays. A touch cancels the ramp: the device wakes to the home screen and the
same occurrence does not start again. Disabling the alarm during the ramp
restores the sleeping screen. A playback that starts during the ramp also
cancels it. Quiet hours never block the ramp, because the alarm exemption
applies.

Quiet hours are up to 2 parental no-playback windows. A window is half-open: it
blocks from the start time, up to but not including the end time. A window that
crosses midnight (start later than end) belongs to its start day. Its part
after midnight reads the bit of the previous day, not the bit of the current
day. A window where start equals end is off. Nothing is blocked before the
first SNTP sync, because the time is not reliable yet. The alarm and its beep
fallback always sound, also during a quiet window. Podcast downloads and
auto-maintenance also continue during a quiet window. The web page is the only
writer of this object: the device never writes it.

The daily usage limit caps the time the child uses the device per local day.
The device counts one second of usage in two cases. The first is audio that
plays audibly: SD card, web radio, podcast or Music Assistant. The second is
the game screen open with the display awake. The device counts nothing while
the audio pauses, during the alarm and its beep, and during the tuner. At the
end of the configured minutes it blocks the playback and the game until local
midnight, exactly like quiet hours. The tiles turn grey and a message explains
the refusal. Another message warns the child 5 minutes before that point. The
alarm always rings and never consumes the quota. The child reads the counted
time and the time left on the device, under Settings, then "Listening time".
This file does not hold the counter. NVS holds it, in `use_day` and `use_sec`,
written about once per minute of usage, so a power cut cannot reset it. The
device counts and blocks nothing before the first SNTP sync. Like quiet hours,
the web page is the only writer of this object.

Favorites are up to 12 quick-play entries, behind a Favorites home tile. The
tile is hidden while the list is empty. The device adds and removes the content
that plays through the star button of the now-playing screen
(`config_store_favorite_add` and `config_store_favorite_remove`). The web Play
tab lists, reorders and deletes them through the full-config save.

## Rules

- The web page and the firmware both read and write this file.
- Validate on read. A missing optional field falls back to its default. An
  unknown `schema_version` is rejected.
- Bound every string length and every list size at parse time.
- A field that the parser reads must also go into the serializer. A device-side
  setter rewrites the whole file from memory. So the next tap on the device
  drops a field that only the parser knows.
  `test/host/check_config_parity.py` checks this.
