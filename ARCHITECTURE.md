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

## Failure Policy

The product is designed by the lucid user from the evening for the tired user in the morning. Battery preparation and normal readiness are the user's responsibility: the bracelet should be placed on the station and the station should be plugged in overnight.

If there is a blocking bug or technical fault that prevents normal operation, the station should not create a fake sense of reliability. It should enter `fault`, avoid starting a broken alarm flow, and publish a clear problem state for the app.

Blocking faults include:

- No valid alarm plan can be loaded and no usable local fallback schedule exists.
- Bracelet is missing or not ready when bracelet validation is required.
- Bracelet reports `fault`.
- Station cannot determine time.
- Station audio output cannot be initialized.

Music streaming failure is not a reason to skip the alarm. If the selected Supabase music cannot be streamed at wake-up time, the station must use a small local fallback alarm sound that fits ESP memory constraints.

Network or Supabase failures should be visible in station logs and in app-readable status when possible.

## Supabase Contract

Keep the Supabase model minimal and useful. Do not overfit it with analytics, playlists, accounts, or detailed metadata before v1 works.

The v1 backend needs these logical records:

- `alarm_plan`: the next alarm for tomorrow, including enabled flag, wake time, timezone, selected music reference, and last update time.
- `music_library`: user-uploaded audio entries, including display name, storage path or public URL, and active/deleted state.
- `device_report`: latest station-visible state, including station state, bracelet state as last seen by station, current blocking problem if any, and last update time.

Agents may choose exact table names and field names only when implementing the backend, but must keep this logical contract stable and document the chosen schema.

V1 may use no authentication as a personal prototype shortcut. Any agent touching Supabase policies, keys, storage, or URLs must state clearly whether the change is prototype-only or production-ready.

## ESP-NOW Contract

Bracelet -> Station messages should cover:

- Bracelet identity.
- Sequence number or timestamp-like counter.
- Bracelet state.
- Activity score or activity level.
- Whether sustained activity is validated.
- Battery state if available.
- Fault code if available.

Station -> Bracelet messages may cover:

- Station identity.
- Alarm phase, such as idle, armed, ringing, validating.
- Optional threshold/profile selection.
- Optional time sync hint if useful.

The station remains authoritative. Bracelet validation is input to the station, not a direct stop command.

## Fallback Sound Contract

The station must have a local fallback alarm sound for cases where Supabase audio is unreachable at wake-up time.

The fallback should:

- Use very little memory.
- Be loud and recognizable enough to wake the user.
- Not depend on network availability.
- Be documented in the station firmware.

The fallback sound does not replace Supabase music as the normal path.

## Implementation Order

Recommended order for agents:

1. Keep this architecture contract current.
2. Define the concrete Supabase schema from the logical contract.
3. Implement station time sync, alarm state machine, and fallback sound.
4. Implement app upload, music selection, alarm plan editing, and status display.
5. Implement bracelet BMI270 activity detection.
6. Implement ESP-NOW bracelet/station telemetry.
7. Integrate and validate the complete wake-up flow.

