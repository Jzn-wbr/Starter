# Supabase v1 schema

`v1_schema.sql` is the concrete database contract for the personal wake-up prototype.

Apply it in the Supabase SQL editor or through the Supabase CLI against the target project. It creates:

- `alarm_config`
- `music_tracks`
- `device_status`
- storage bucket `wake-up-music`
- prototype-only anon read/write policies

The anon policies are intentionally permissive for the no-auth v1 prototype. They are not production-ready security.

## V1 table split

The schema intentionally keeps one active `alarm_config` row instead of splitting alarm time, audio choice, and volume into separate tables. For v1 the station only needs one next-alarm contract to fetch and cache, so extra tables would add joins and synchronization rules without adding useful behavior.

## Mermaid overview

```mermaid
erDiagram
  MUSIC_TRACKS {
    text id PK
    text title
    text storage_path UK
    text public_url
    boolean is_available
    timestamptz created_at
  }

  ALARM_CONFIG {
    text id PK "always main"
    boolean enabled
    date alarm_date
    time alarm_time
    text timezone
    integer volume_percent
    audio_source_v1 audio_source
    text selected_track_id FK
    integer revision
    timestamptz updated_at
  }

  DEVICE_STATUS {
    text id PK "always main"
    station_state_v1 station_state
    bracelet_state_v1 bracelet_state
    problem_code_v1 problem_code
    text problem_message
    integer active_alarm_revision
    integer bracelet_battery_percent
    bigint bracelet_last_seen_ms
    timestamptz updated_at
  }

  MUSIC_TRACKS ||--o| ALARM_CONFIG : selected_by
```
