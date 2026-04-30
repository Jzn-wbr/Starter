# System Architecture And Contracts

This file defines the shared contracts between the three code areas:

- `code/app mobile`: Vue/Vite PWA.
- `code/boite hp`: bedside station firmware.
- `code/bracelet`: bracelet firmware.

Each subsystem should be developed independently against these contracts. Do not change a contract without updating this file and the affected `AGENTS.md` files.

## Core Architecture

The station is the central alarm authority. The app configures the alarm through Supabase. The bracelet only reports activity to the station.

Target v1 communication paths:

- App -> Supabase: upload music, select music, configure next alarm.
- Station -> Supabase: read alarm plan, read selected music URL, write station/problem state.
- Bracelet -> Station: send activity and battery/fault telemetry over ESP-NOW.
- Station -> Bracelet: optional ESP-NOW control messages such as current alarm phase or threshold profile.
- App -> Station: no direct communication in v1.
- App -> Bracelet: no direct communication in v1.
- Bracelet -> Supabase: no direct communication in v1.

ESP-NOW is the target station/bracelet link because both devices are ESP32-family boards and the bracelet should avoid WiFi-heavy behavior. Agents must treat room-to-room range as an assumption to test physically. If ESP-NOW is unreliable across the needed house distance, ask before changing transport.

## Supabase V1 Schema

Use these exact logical table names for v1 unless this file is updated first. Field names may be implemented as SQL columns or JSON fields, but the app and station must agree on the same wire shape before coding against it.

### `alarm_config`

Single active-row table for the next wake-up only.

- `id`: text, always `main` for v1.
- `enabled`: boolean.
- `alarm_date`: ISO date string, local date for the next alarm.
- `alarm_time`: `HH:MM` 24-hour local time.
- `timezone`: IANA timezone string, for example `Europe/Zurich`.
- `selected_track_id`: text or UUID referencing `music_tracks.id`; nullable only if fallback sound is intentionally selected.
- `revision`: integer incremented by the app on each saved config.
- `updated_at`: ISO timestamp.

Station rule: cache the last valid `alarm_config` locally. If Supabase is unreachable later but the cached config is still for the next alarm and the station has valid time, the station may use the cached config.

### `music_tracks`

Library of uploaded wake-up sounds.

- `id`: text or UUID.
- `title`: display name shown in the app.
- `storage_path`: Supabase Storage path.
- `public_url`: playable URL used by the station in the no-auth prototype.
- `is_available`: boolean.
- `created_at`: ISO timestamp.

Avoid storing duration, waveform, tags, statistics, playlist order, or user metadata before v1 needs them.

### `device_status`

Single station-published status row read by the app.

- `id`: text, always `main` for v1.
- `station_state`: one of the station states below.
- `bracelet_state`: latest bracelet state as seen by the station, or `unknown`.
- `problem_code`: one of the problem codes below, or `none`.
- `problem_message`: short human-readable diagnostic text.
- `active_alarm_revision`: latest `alarm_config.revision` loaded by the station.
- `bracelet_battery_percent`: integer `0..100`, or `null` if unknown.
- `bracelet_last_seen_ms`: station uptime timestamp for the last bracelet packet, or `null`.
- `updated_at`: ISO timestamp.

The app reads this status. The app must not infer alarm authority from it.

### Problem Codes

Use these v1 problem codes:

- `none`
- `wifi_unavailable`
- `supabase_unavailable`
- `time_unknown`
- `no_valid_alarm_config`
- `music_stream_failed`
- `fallback_audio_failed`
- `bracelet_missing`
- `bracelet_low_battery`
- `bracelet_fault`
- `sensor_fault`
- `audio_fault`
- `unknown_fault`

## System States

The station should expose one clear state at a time:

- `idle`: no alarm armed.
- `armed`: next alarm is configured and valid.
- `ringing`: alarm audio is active.
- `validating_activity`: alarm is active and bracelet activity is being evaluated.
- `stopped`: alarm stopped after valid bracelet activity.
- `fault`: alarm cannot run correctly because of a blocking issue.

The bracelet should expose:

- `charging`: bracelet is on the station contacts or charging input.
- `ready`: enough battery and sensor link is usable.
- `active`: activity is currently detected.
- `validated`: sustained activity requirement has been met.
- `low_battery`: battery may be insufficient for reliable wake-up validation.
- `fault`: sensor, power, or firmware state prevents reliable validation.

The app should display station and bracelet states without becoming the alarm authority.

## Station State Transitions

Allowed station transitions for v1:

- `idle -> armed`: app has saved a valid enabled alarm and station loaded it.
- `idle -> fault`: blocking configuration, time, audio, or bracelet readiness problem.
- `armed -> idle`: app disables the next alarm.
- `armed -> ringing`: alarm time is reached and prerequisites are valid.
- `armed -> fault`: a blocking problem appears before alarm time.
- `ringing -> validating_activity`: bracelet packets are present and activity evaluation is active.
- `ringing -> fault`: a blocking technical fault prevents normal validation or audio output.
- `validating_activity -> stopped`: sustained bracelet activity is validated.
- `validating_activity -> fault`: bracelet link, bracelet sensor, or station audio becomes invalid.
- `stopped -> idle`: alarm cycle is complete.
- `fault -> idle`: user fixes the issue and station reloads a valid disabled/no-alarm state.
- `fault -> armed`: user fixes the issue and station reloads a valid enabled alarm.

Do not add a transition from `ringing` or `validating_activity` to `stopped` unless it is caused by bracelet validation.

## Failure Policy

The product is designed by the lucid user from the evening for the tired user in the morning. Battery preparation and normal readiness are the user's responsibility: the bracelet should be placed on the station and the station should be plugged in overnight.

If there is a blocking bug or technical fault that prevents normal operation, the station should not create a fake sense of reliability. It should enter `fault`, avoid starting a broken alarm flow, and publish a clear problem state for the app.

Blocking faults before alarm start include:

- No valid `alarm_config` can be loaded and no valid cached config exists.
- Bracelet is missing or not ready when bracelet validation is required.
- Bracelet battery is below the required threshold.
- Bracelet reports `fault`.
- Station cannot determine time.
- Station audio output cannot be initialized.

Music streaming failure is not a reason to skip the alarm. If the selected Supabase music cannot be streamed at wake-up time, the station must use a small local fallback alarm sound that fits ESP memory constraints.

Network or Supabase failures should be visible in station logs and in app-readable status when possible.

If a blocking fault appears while already ringing, prefer `fault` over an endless alarm that cannot be validated. This is a product safety rule for bugs and broken technical states, not a user-facing stop feature.

## Battery Readiness Contract

Use these v1 defaults unless physical testing proves they are wrong:

- Bracelet `ready`: battery is at least `30%` and sensor init succeeds.
- Bracelet `low_battery`: battery is below `30%`.
- Bracelet blocking low battery: battery is below `20%` before alarm start.
- Station should publish `bracelet_low_battery` before bedtime if seen.
- Station should enter `fault` before alarm start if bracelet battery is below the blocking threshold.

If battery percentage cannot be measured yet, report `null` and document the limitation. Do not fake battery precision.

## ESP-NOW Contract

Use compact binary packets. All packets start with:

- `protocol_version`: `1`.
- `message_type`: enum below.
- `sender_role`: `station` or `bracelet`.
- `device_id`: 6-byte MAC address.
- `sequence`: uint32 incrementing counter.
- `uptime_ms`: uint32 sender uptime.

### Message Types

- `1`: `bracelet_status`, bracelet -> station.
- `2`: `station_control`, station -> bracelet.
- `3`: `pairing_probe`, reserved for future explicit pairing.
- `4`: `debug_event`, development only.

### `bracelet_status`

Fields:

- `bracelet_state`: enum from System States.
- `activity_score`: uint8 `0..100`.
- `validated`: boolean.
- `battery_percent`: uint8 `0..100`, or `255` if unknown.
- `fault_code`: problem code enum, or `none`.
- `flags`: bitmask for `charging`, `sensor_ready`, `motion_present`.

Default send rate:

- Ready/idle: every `5s`.
- Alarm armed within 10 minutes: every `2s`.
- Ringing/validating: `5 Hz`.
- Fault: every `2s`.

Station timeout rules:

- If no bracelet packet for `15s` while `armed`, publish `bracelet_missing`.
- If no bracelet packet for `5s` while `ringing` or `validating_activity`, enter `fault` with `bracelet_missing`.

### `station_control`

Fields:

- `station_state`: enum from System States.
- `alarm_revision`: current loaded `alarm_config.revision`.
- `activity_required`: boolean.
- `threshold_profile`: `normal` for v1.

Station control messages are optional in v1. The bracelet must still be able to send status without first receiving station control.

The station remains authoritative. Bracelet validation is input to the station, not a direct stop command.

## Activity Validation Contract

Use these v1 defaults unless physical testing proves they are wrong:

- Validation requires `60s` of sustained activity.
- Evaluate activity in a rolling window.
- Isolated shake spikes must not validate alone.
- Short pauses under `3s` are tolerated.
- Activity score should represent recent movement intensity on a `0..100` scale.

The bracelet computes activity and sends `validated=true` only after the sustained requirement is met. The station still decides whether this stops the current alarm.

## Fallback Sound Contract

The station must have a local fallback alarm sound for cases where Supabase audio is unreachable at wake-up time.

The fallback should:

- Use very little memory.
- Be loud and recognizable enough to wake the user.
- Not depend on network availability.
- Be documented in the station firmware.
- Prefer generated tones or a very small embedded sample over a large stored audio file.

The fallback sound does not replace Supabase music as the normal path.

## Secrets And Environment Contract

Do not commit real secrets.

Allowed:

- `secrets.example.h`
- `.env.example`
- documented placeholder values

Forbidden:

- `secrets.h`
- `.env`
- `.env.local`
- Supabase service-role keys
- WiFi passwords
- private storage tokens

The PWA may use Supabase public anon configuration for the no-auth prototype, but agents must label that setup as prototype-only.

## Implementation Order

Recommended order for agents:

1. Keep this architecture contract current.
2. Define the concrete Supabase schema from the logical contract.
3. Implement station time sync, alarm state machine, and fallback sound.
4. Implement app upload, music selection, alarm plan editing, and status display.
5. Implement bracelet BMI270 activity detection.
6. Implement ESP-NOW bracelet/station telemetry.
7. Integrate and validate the complete wake-up flow.
