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
- Station -> Bracelet: send ESP-NOW control messages such as current alarm phase, acknowledgements, vibration requests, or threshold profile.
- App -> Station: no direct communication in v1.
- App -> Bracelet: no direct communication in v1.
- Bracelet -> Supabase: no direct communication in v1.

ESP-NOW is the target station/bracelet link. The station remains associated with the configured home WiFi for NTP, Supabase, and music streaming, while ESP-NOW shares the station's current 2.4 GHz channel. The bracelet does not join the router: it scans ESP-NOW channels until a valid paired station control is found, locks to that channel while controls continue, and resumes scanning after a control timeout. ESP-NOW range, channel reacquisition, latency, and battery life must be tested physically.

## Supabase V1 Schema

Use these exact table names and field names for v1 unless this file is updated first. The concrete Supabase SQL definition is in `database/supabase/v1_schema.sql`. The app and station must code against that SQL schema.

The v1 schema is prototype-only from a security perspective: anon clients may read and write the v1 tables and the public wake-up music bucket. This is acceptable only for the personal no-auth prototype, not for production.

### `alarm_plan`

Single active-row table for whether and when the next wake-up should happen.

- `id`: text, always `main` for v1.
- `enabled`: boolean.
- `alarm_date`: SQL `date`, local date for the next alarm.
- `alarm_time`: SQL `time without time zone`, `HH:MM` 24-hour local time at the app/station boundary.
- `timezone`: IANA timezone string, for example `Europe/Zurich`.
- `revision`: integer incremented by the app on each saved config.
- `updated_at`: SQL `timestamptz`, updated automatically by the database on row update.

### `alarm_audio_selection`

Single active-row table for what the next wake-up should play.

- `id`: text, always `main` for v1.
- `audio_source`: `track` or `fallback`; `fallback` means the local station fallback sound is intentionally selected.
- `selected_track_id`: text referencing `music_tracks.id`; required when `audio_source` is `track`, null when `audio_source` is `fallback`.
- `volume_percent`: integer `0..100`, playback volume requested for the next alarm.
- `updated_at`: SQL `timestamptz`, updated automatically by the database on row update.

Station rule: cache the last valid combined alarm configuration from `alarm_plan` and `alarm_audio_selection` locally. If Supabase is unreachable later but the cached config is still for the next alarm and the station has valid time, the station may use the cached config.

### `music_tracks`

Library of uploaded wake-up sounds.

- `id`: text primary key, generated as a UUID string by default.
- `title`: display name shown in the app.
- `storage_path`: Supabase Storage path.
- `public_url`: playable URL used by the station in the no-auth prototype.
- `is_available`: boolean.
- `created_at`: SQL `timestamptz`.

Use the Supabase Storage bucket `wake-up-music` for uploaded audio files. V1 allows `audio/mpeg`, `audio/wav`, `audio/ogg`, and `audio/mp4` up to 50 MiB per file.

Avoid storing duration, waveform, tags, statistics, playlist order, or user metadata before v1 needs them.

### `station_status`

Single station-published status row read by the app.

- `id`: text, always `main` for v1.
- `station_state`: one of the station states below.
- `problem_code`: one of the problem codes below, or `none`.
- `problem_message`: short human-readable diagnostic text.
- `active_alarm_revision`: latest `alarm_plan.revision` loaded by the station.
- `station_battery_voltage`: station battery terminal voltage in volts, or `null` if unknown.
- `updated_at`: SQL `timestamptz`, updated automatically by the database on row update.

The app reads this status. The app must not infer alarm authority from it.

### `bracelet_status`

Single station-published row for the latest bracelet state as seen by the station.

- `id`: text, always `main` for v1.
- `bracelet_state`: latest bracelet state as seen by the station, or `unknown`.
- `problem_code`: one of the problem codes below, or `none`.
- `problem_message`: short human-readable diagnostic text.
- `bracelet_battery_voltage`: bracelet battery terminal voltage in volts, or `null` if unknown.
- `bracelet_last_seen_ms`: station uptime timestamp for the last bracelet packet, or `null`.
- `bracelet_energy`: latest 200 ms energy window reported by the bracelet.
- `bracelet_energy_valid_ms`: measured time inside that 200 ms window, excluding vibration and settling time.
- `bracelet_energy_threshold`: station threshold used to mute alarm audio for v1. The current firmware default is `1000000`.
- `energy_mute_remaining_ms`: remaining station-side mute time produced by bracelet energy.
- `bracelet_vibrating`: whether the bracelet vibration motor is currently active.
- `updated_at`: SQL `timestamptz`, updated automatically by the database on row update.

The app reads this status. The bracelet does not write to Supabase in v1.

### Problem Codes

Use these v1 problem codes:

- `none`
- `supabase_unavailable`
- `time_unknown`
- `no_valid_alarm_config`
- `music_stream_failed`
- `fallback_audio_failed`
- `bracelet_missing`
- `bracelet_low_battery`
- `sensor_fault`

## System States

The station should expose one clear state at a time:

- `idle`: no alarm armed.
- `armed`: next alarm is configured and valid.
- `ringing`: alarm audio is active.
- `validating_activity`: alarm is active and bracelet activity is being evaluated.
- `stopped`: alarm window ended after bracelet activity monitoring.
- `fault`: alarm cannot run correctly because of a blocking issue.

The bracelet should expose:

- `ready`: enough battery and sensor link is usable.
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
- `ringing -> validating_activity`: bracelet energy reaches `1000000` during the alarm window, so alarm audio is muted for 10 seconds while monitoring continues.
- `ringing -> ringing`: bracelet packets are lost for more than `3s`; publish `bracelet_missing`, keep alarm audio playing, and resume movement detection automatically when packets return.
- `ringing -> fault`: station audio output fails.
- `ringing -> stopped`: the 10-minute alarm activity window has ended.
- `validating_activity -> ringing`: the 10-second energy mute and the 3-second pre-unmute bracelet warning expire before the 10-minute window has ended.
- `validating_activity -> stopped`: the 10-minute alarm activity window has ended.
- `validating_activity -> validating_activity`: bracelet packets are lost for more than `3s`; publish `bracelet_missing` but honor the remaining confirmed movement mute.
- `validating_activity -> ringing`: the confirmed movement mute expires and no vibration acknowledgement or new movement event prevents audio from resuming.
- `validating_activity -> fault`: station audio output fails.
- `stopped -> idle`: alarm cycle is complete.
- `fault -> idle`: user fixes the issue and station reloads a valid disabled/no-alarm state.
- `fault -> armed`: user fixes the issue and station reloads a valid enabled alarm.

Do not add a normal stop or snooze transition from `ringing` or `validating_activity`. The v1 alarm stops only when the fixed 10-minute activity window ends; bracelet energy only mutes alarm audio during that window.

## Failure Policy

The product is designed by the lucid user from the evening for the tired user in the morning. Battery preparation and normal readiness are the user's responsibility: the bracelet should be placed on the station and the station should be plugged in overnight.

If there is a blocking bug or technical fault that prevents normal operation, the station should not create a fake sense of reliability. It should enter `fault`, avoid starting a broken alarm flow, and publish a clear problem state for the app.

Blocking faults before alarm start include:

- No valid combined `alarm_plan` and `alarm_audio_selection` can be loaded and no valid cached config exists.
- Bracelet is missing or not ready when bracelet validation is required.
- Bracelet battery is below the required threshold.
- Bracelet reports `fault`.
- Station cannot determine time.
- Station audio output cannot be initialized.

Music streaming failure is not a reason to skip the alarm. If the selected Supabase music cannot be streamed at wake-up time, the station must use a small local fallback alarm sound that fits ESP memory constraints.

Network or Supabase failures should be visible in station logs and in app-readable status when possible.

If the bracelet disappears for more than `3s` while ringing, publish `bracelet_missing` and keep alarm audio playing. If a confirmed movement mute is already active, honor its original deadline instead of cancelling it; once it expires, resume audio after the vibration acknowledgement policy below. When packets return, clear the missing problem and process any still-valid retransmitted movement event. Do not mark the alarm revision complete because of bracelet packet loss. If station audio itself fails, prefer `fault` over a fake alarm state.

## Battery Readiness Contract

Use these v1 defaults unless physical testing proves they are wrong:

- Bracelet `ready`: battery is at least `30%` and sensor init succeeds.
- Bracelet `low_battery`: battery is below `30%`.
- Bracelet blocking low battery: battery is below `20%` before alarm start.
- Station should publish `bracelet_low_battery` before bedtime if seen.
- Station should enter `fault` before alarm start if bracelet battery is below the blocking threshold.

If battery voltage cannot be measured yet, report `null` and document the limitation. Do not fake battery precision.

Supabase stores raw battery voltage for station and bracelet status. User-facing battery percentages are derived by the PWA from the voltage, so changing the display curve does not require a database migration.

Station battery voltage is measured on station GPIO34 through a 2:1 voltage divider: `Vbat = 2 * Vadc`.

## Bracelet/Station ESP-NOW Contract

Use compact binary packets. All packets start with:

- `magic`: uint32 value identifying Starter traffic before any packet is parsed.
- `protocol_version`: `6`. Older packets are rejected; station and bracelet firmware must be updated together.
- `message_type`: enum below.
- `sender_role`: `station` or `bracelet`.
- `device_id`: 6-byte MAC address.
- `sequence`: uint32 incrementing counter.
- `uptime_ms`: uint32 sender uptime.

### Message Types

- `1`: `bracelet_status`, bracelet -> station.
- `2`: `station_control`, station -> bracelet.

### ESP-NOW `bracelet_status` Packet

Fields:

- `bracelet_state`: enum from System States.
- `battery_voltage_mv`: uint16 bracelet battery terminal voltage in millivolts, or `0` if unknown.
- `fault_code`: problem code enum, or `none`.
- `vibrating`: boolean indicating whether the vibration motor is currently active.
- `energy`: uint32 energy measured over the latest 200 ms window.
- `energy_valid_ms`: uint16 measured milliseconds in that window, excluding bracelet vibration/settling.
- `boot_session_id`: uint32 random non-zero identifier regenerated at bracelet boot.
- `movement_event_id`: uint32 identifier of the latest unacknowledged qualifying energy window, or `0`.
- `movement_event_uptime_ms`: bracelet uptime when that energy window ended.
- `movement_event_energy`: uint32 energy of the retained event.
- `movement_event_valid_ms`: valid measured milliseconds of the retained event.
- `vibration_ack_id`: latest vibration request identifier for which the motor actually started.

Both devices register an unencrypted ESP-NOW broadcast peer. Default send rate:

- Bracelet status is sent every `10s` outside an active alarm and at `5 Hz` while ringing or validating activity. Qualifying movement during the alarm, vibration acknowledgement, first contact, faults, and important state changes force an immediate status. The station must not mirror high-rate telemetry to Supabase.
- Station control is sent every `5s` outside an alarm, every `700ms` during an alarm, and immediately on important state changes.

Station timeout rules:

- If no bracelet packet for `25s` while `armed`, publish `bracelet_missing`.
- If no bracelet packet for `3s` while `ringing`, publish `bracelet_missing` and keep alarm audio playing.
- If no bracelet packet for `3s` during an existing movement mute, publish `bracelet_missing` but preserve the mute deadline. A delayed event can recover only the unelapsed part of its original 10-second interval.
- After a Supabase HTTPS failure, the station waits `30s` before trying another Supabase request. This avoids repeated TLS allocation failures during low-memory recovery after streaming.

### `station_control`

Fields:

- `station_state`: enum from System States.
- `vibration_request`: boolean; forced false for the first `15s` after the configured alarm time, then true while the station is producing audible alarm audio and during the 3-second pre-unmute warning before audio resumes after an energy mute. Audio, movement processing, mute timing, and the fixed activity window remain active during the initial vibration-free interval.
- `acknowledged_boot_session_id`: bracelet boot session associated with the acknowledged movement event.
- `acknowledged_movement_event_id`: latest movement event accepted or intentionally ignored by the station.
- `vibration_request_id`: non-zero identifier regenerated whenever vibration changes from not requested to requested.

The bracelet scans WiFi channels 1 through 13 every `300ms` until it receives a valid `station_control`, then locks to that channel while controls continue. After `15s` without a valid control, it resumes channel scanning. Reliable movement delivery and confirmed pre-unmute vibration use the acknowledgements carried by subsequent station control packets.
When `vibration_request` is true, the bracelet pulses its motor instead of holding it continuously on. The current firmware default is `500ms` on every `4s`, with BMI270 energy ignored while the motor is active and during the short settling time after it stops. When the motor first starts for a request, the bracelet echoes its identifier in `vibration_ack_id` and forces a status response.
The station broadcasts `station_control` over ESP-NOW on its current home-WiFi channel. Controls are sent every `5s` outside an alarm, every `700ms` during an alarm, and immediately for important state changes. ESP-NOW loss is handled by the existing event retention, identifiers, retransmission, and application acknowledgements.

The first valid exchange pairs the two device MAC identifiers in NVS. Later packets from another device identifier are rejected. Replacing a paired device requires erasing NVS/flash before reflashing. If the station changes home-WiFi channel after reconnecting, the bracelet reacquires it through channel scanning without clearing the pairing.

The station remains authoritative. Bracelet energy is input to the station, not a direct stop command.

## Activity Window Contract

Use these v1 defaults unless physical testing proves they are wrong:

- The station opens a fixed `10min` activity window at the configured alarm time.
- The station starts alarm audio at the configured time but does not request bracelet vibration during the first `15s`. Once those `15s` have elapsed, the existing vibration behavior resumes; all movement and audio-muting rules continue throughout the delay.
- During that window, the station plays alarm audio when bracelet energy is below threshold or missing.
- If a bracelet energy window reaches the station threshold, currently `1000000`, the bracelet retains it as a movement event for up to `10s` and retransmits it until acknowledged. The station revalidates the event and mutes alarm output with station `XSMT` on GPIO26 for the unelapsed part of the original 10-second interval. For example, an event received at age `4s` produces `6s` of mute.
- Duplicate, out-of-order, pre-alarm, invalid, or at-least-10-second-old movement events do not restart the mute. A newer valid event may extend the existing deadline.
- When the mute expires, the station keeps `XSMT` muted and requests vibration with a new identifier. After matching `vibration_ack_id`, it gives the user a 3-second pre-unmute warning plus the `500ms` jitter grace. Without acknowledgement after `3s`, it resumes alarm audio conservatively. A new valid movement event during either wait restarts only its own remaining interval.
- When the 10-minute window ends, the station stops alarm audio and marks the alarm revision complete, regardless of energy history.

The bracelet computes energy over 200 ms windows and retains threshold candidates for reliable transport. The station revalidates every event and remains responsible for whether audio should play and when the alarm revision is complete.

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

## UML Documentation Contract

The app, station, and bracelet projects must keep human-readable PlantUML diagrams:

- `code/app mobile/UML`
- `code/boite hp/UML`
- `code/bracelet/UML`

Agents must update the relevant `.puml` files when changing:

- app use cases or user-facing flows;
- class/module boundaries;
- state machines;
- ESP-NOW packet flow, channel discovery, and pairing;
- Supabase/config flow in station firmware;
- activity validation flow in bracelet firmware;
- fallback or fault-handling architecture.

The app must keep at least a simple UML use-case diagram. The `.puml` files are maintained manually with code changes. They should not be regenerated automatically by PlatformIO builds, app dev servers, or normal agent runs.

## Implementation Order

Recommended order for agents:

1. Keep this architecture contract current.
2. Apply and validate the concrete Supabase schema in `database/supabase/v1_schema.sql`.
3. Implement station time sync, alarm state machine, and fallback sound.
4. Implement app upload, music selection, alarm plan editing, and status display.
5. Implement bracelet BMI270 activity detection.
6. Implement ESP-NOW bracelet/station telemetry.
7. Integrate and validate the complete wake-up flow.
