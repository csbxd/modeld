# HTTP API 2

All paths below are relative to the configured API origin. JSON objects use the exact documented fields; unknown fields are errors. The service currently implements `rife-4.26-heavy-fp32`, fixed 60 fps SDR output, even dimensions from 32×32 through 1920×1080 or 1080×1920.

## Routes

| Method | Path | Authentication | Result |
|---|---|---|---|
| GET | `/healthz` | none | Readiness, application version and protocol number |
| GET | `/api/v2/models` | Bearer | Implemented models and capabilities |
| GET | `/api/v2/status` | Bearer | Worker/queue counters and occupied session slots |
| GET | `/api/v2/sessions` | Bearer | Active session states |
| POST | `/api/v2/sessions` | Bearer | 201 with ID, lease interval and media manifest |
| GET | `/api/v2/sessions/{id}` | Bearer | Lease, pause state, progress, cache and media |
| PUT | `/api/v2/sessions/{id}/lease` | Bearer | Renew activity and report user pause state |
| DELETE | `/api/v2/sessions/{id}` | Bearer | Idempotent cancellation; 204 without a body |
| GET | `/api/v2/sessions/{id}/media` | session capability | JSON manifest |
| GET | `/api/v2/sessions/{id}/media/init` | session capability | Matroska initialization bytes |
| GET | `/api/v2/sessions/{id}/media/segments/{index}` | session capability | Matroska clusters for the zero-based segment |

A media URL's session ID is a private read capability. It does not authorize control operations. The daemon listens on the configured address; the deployment uses NetBird.

## Create

```json
{
  "model": "rife-4.26-heavy-fp32",
  "video": {
    "urls": ["https://media.example/video.mp4"],
    "headers": {"Referer": "https://www.bilibili.com/"}
  },
  "audio": {
    "urls": ["https://media.example/audio.m4a", "https://backup.example/audio.m4a"],
    "headers": {}
  },
  "tracks": {"audio": true, "subtitles": true}
}
```

`model` and `video` are required. `audio` is an optional separate input; omitting it uses the video's embedded audio. `tracks` defaults to both true. An audio input cannot be supplied with `tracks.audio=false`.

Each input has 1–9 URLs for the same encoded media, in preference order. Headers default to an empty object. Accepted names are Referer, User-Agent, Cookie, Origin, Authorization, Accept, Accept-Language and X-Forwarded-For; names are case insensitive and duplicates, newlines and transport-control headers are rejected. Limits are 16 headers and 16 KiB per input, and 64 KiB for the whole request. The daemon receives resolved media URLs, not webpage URLs.

A successful response has exactly:

```json
{
  "id": "0123456789abcdef0123456789abcdef",
  "lease_seconds": 120,
  "manifest": {
    "schema": 2,
    "media": {
      "width": 1920,
      "height": 1080,
      "duration_seconds": 120.0,
      "source_fps": 30.0,
      "output_fps": 60,
      "segment_seconds": 2,
      "segment_count": 60,
      "tracks": [
        {"id": 1, "kind": "video", "source": "video", "source_index": 0,
         "codec": "hevc", "language": "", "title": ""},
        {"id": 1, "kind": "audio", "source": "audio", "source_index": 0,
         "codec": "aac", "language": "und", "title": ""}
      ]
    },
    "init": "/api/v2/sessions/0123456789abcdef0123456789abcdef/media/init",
    "segment_template": "/api/v2/sessions/0123456789abcdef0123456789abcdef/media/segments/{index}"
  }
}
```

Track IDs are numbered within each kind. `kind` is video, audio, subtitle or attachment; `source` identifies the video or separate audio input. `source_index` is the original FFmpeg stream index within that input.

The final segment retains a fractional final frame: frame count is `ceil(duration_seconds × 60)`. Each segment starts with an independently decodable video keyframe and retains absolute timestamps. Prefix each segment with initialization bytes when opening it independently. mpv uses its native EDL DASH prefix mechanism to expose one continuous main timeline.

## Lease and status

```json
{"paused": true}
```

Send this to the lease route periodically, within `lease_seconds`. Media reads also renew activity. `paused` suppresses unsolicited server prefetch; explicit reads still finish for mpv's paused forward caching and seeking. Status/list requests are observational.

A session state contains `progress.rendering`, `last_requested`, `last_completed`, `completed_segments` and `last_error`, plus cached and pending segment indexes. Counts are cumulative successful render operations, including regeneration after eviction. Queue-full or session-limit errors return 429; the client can retry later.

## Errors

```json
{
  "error": {
    "code": "queue_full",
    "message": "GPU queue is full",
    "retryable": true
  }
}
```

Codes include `invalid_request`, `unauthorized`, `not_found`, `unknown_model`, `session_limit`, `queue_full`, `source_timeout`, `source_failed`, `unsupported_media`, `session_closed`, `scheduler_stopped` and `backend_failed`. A failed segment can be requested again. Source I/O is bounded and cancellation is cooperative; loading a new GPU resolution profile can add startup time.

Successful segment headers include `X-Modeld-Frames` and `X-Modeld-Render-Seconds`. Audio/subtitle packet data is copied; only the interpolated video is encoded. Unsupported source formats fail explicitly.
