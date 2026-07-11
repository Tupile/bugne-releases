# Podcast manifest schema

This is a contract. The manifest is identical whether written by the on-device
yxml parser or by a future Python companion script. The player only ever reads
the manifest, never the raw RSS.

## Location

- One manifest per podcast on the SD card: `/sdcard/podcasts/<id>/manifest.json`,
  where `<id>` is the podcast `id` from the config (see config_schema.md).

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
| `generated_at` | string | ISO 8601 UTC timestamp of when the manifest was written. |
| `episodes` | array | At most 30, newest first. |
| `episodes[].title` | string | Episode title. |
| `episodes[].date` | string | ISO 8601 publication date. |
| `episodes[].duration_seconds` | int | Duration in seconds, 0 if unknown. |
| `episodes[].episode_url` | string | Remote audio URL, streamed over HTTPS if not cached. |
| `episodes[].cache_path` | string | Local SD path used when the episode is cached: `/sdcard/podcasts/<podcast title>/<episode title>.<ext>`. Both name parts are sanitized for FAT (illegal characters dropped, length bounded). |
| `episodes[].cached` | bool | Always written `false`. The reader ignores it and re-derives it from whether `cache_path` exists on the SD card, so it stays correct after a refresh rewrites the manifest. |

## Rules

- Cap at the 30 most recent episodes.
- All RSS text is untrusted input: bound every string length and escape before
  storing or displaying.
- Playback prefers the cached SD file. If `cached` is false or the SD is absent,
  stream `episode_url` over HTTPS.
- Caching to SD is an explicit, user-triggered, occasional operation. It never
  runs in the background during playback.
