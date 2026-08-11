# Podcast manifest schema

This file is a contract. The manifest is the same whether the on-device yxml
parser writes it, or a future Python companion script writes it. The player
reads the manifest only. It never reads the raw RSS.

## Location

- One manifest per podcast, in internal flash:
  `/littlefs/podcasts/<id>.json`, where `<id>` is the podcast `id` from the
  config (see config_schema.md). The manifest is in flash, not on the SD card,
  so the podcast list works without an SD card.
- One optional cover image per podcast, on the SD card:
  `/sdcard/podcasts/<folder>/cover.jpg`, where `<folder>` is the sanitized
  podcast title. The device downloads it after a refresh writes the manifest.
  The manifest does not name that file: the reader builds the path.

## Schema (version 1)

```json
{
  "schema_version": 1,
  "podcast_title": "Example Show",
  "rss_url": "https://example/feed.xml",
  "generated_at": "2026-06-21T10:00:00Z",
  "episodes": [
    {
      "title": "Episode 12",
      "date": "2026-06-18T06:00:00Z",
      "duration_seconds": 1832,
      "episode_url": "https://example/ep12.mp3",
      "cache_path": "/sdcard/podcasts/Example Show/Episode 12.mp3",
      "cached": false
    }
  ]
}
```

## Fields

| Field | Type | Notes |
| --- | --- | --- |
| `schema_version` | int | Schema version. Currently 1. |
| `podcast_title` | string | Podcast title from the feed. |
| `rss_url` | string | Source feed URL. |
| `generated_at` | string | ISO 8601 UTC timestamp. It records when the writer wrote the manifest. |
| `episodes` | array | Newest first. The writer emits one episode at a time and the reader parses one at a time, so the list is not bounded by RAM. `PODCAST_MAX_EPISODES` (300) is a safety cap against a pathological feed. |
| `episodes[].title` | string | Episode title. |
| `episodes[].date` | string | ISO 8601 publication date. |
| `episodes[].duration_seconds` | int | Duration in seconds. 0 means unknown. |
| `episodes[].episode_url` | string | Remote audio URL. The device streams it over HTTPS when the episode is not cached. |
| `episodes[].cache_path` | string | Local SD path of the cached episode: `/sdcard/podcasts/<podcast title>/<episode title>.<ext>`. The writer sanitizes both name parts for FAT: it drops the illegal characters and bounds the length. |
| `episodes[].cached` | bool | The writer always writes `false`. The reader ignores this field and derives the answer from a `stat()` of `cache_path`, so a refresh that rewrites the manifest cannot make it wrong. |

## Rules

- All RSS text is untrusted input. Bound every string length, and escape the
  text before you store it or show it.
- Playback prefers the cached SD file. It streams `episode_url` over HTTPS when
  the file is absent, or when there is no SD card.
- A download to the SD card never runs during playback. The download engine
  starts only after the device stays idle, and it stops as soon as a play
  starts. The web page starts a download job, and the idle auto-maintenance
  starts one too.
- The cover download is best effort. A failure there must never cost the
  episode list.
