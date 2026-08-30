# AI Agent Guide: Bedside Station

## Role

This folder contains the bedside speaker station firmware. The station is the central alarm authority:

- It owns alarm state.
- It synchronizes time.
- It fetches alarm configuration from Supabase.
- It plays the selected wake-up music.
- It receives bracelet movement telemetry over ESP-NOW.
- It decides when alarm audio should play and when the alarm window is complete.

The PWA is not the alarm authority. The bracelet does not decide final alarm state.

Read `../../ARCHITECTURE.md` before changing station state transitions, Supabase schema usage, fallback behavior, ESP-NOW packets, battery readiness, or station/bracelet communication.

Read and maintain the PlantUML diagrams in `UML/` when changing station firmware architecture. These diagrams are for human understanding and must evolve with the code.

## Current State

This is a PlatformIO Arduino ESP32 project.

The current firmware already:

- Connects to WiFi.
- Synchronizes local time through NTP.
- Loads, validates, and caches the next Supabase alarm plan.
- Uses `ESP32-audioI2S`.
- Loads the selected track from `alarm_audio_selection` and `music_tracks`.
- Outputs audio over I2S pins for the PCM5102A/PAM8403 audio chain.
- Runs the fixed alarm activity window and generated local fallback sound.
- Keeps bracelet vibration off for the first 15 seconds after the configured alarm time while leaving audio and movement handling active.
- Receives ESP-NOW protocol v6 movement events, acknowledges them, recovers
  their unelapsed mute time after packet loss, and confirms bracelet vibration
  before the pre-unmute warning starts.

The current code remains a physical prototype: range, battery measurements,
audio behavior, movement thresholds, and complete wake-up behavior still need
validation on the final hardware.

## Target Behavior

The station should:

- Synchronize clock time with NTP over WiFi.
- Fetch the next alarm configuration from Supabase.
- Stream the selected Supabase music file at alarm time.
- Use a small local fallback alarm sound if Supabase music is unreachable at wake-up time.
- Connect to the dedicated, NVS-paired bracelet over ESP-NOW while retaining home WiFi for backend and audio traffic.
- Track bracelet movement during the fixed 10-minute activity window after the configured alarm time.
- Play alarm audio during that window only while bracelet movement is absent, then stop once the window ends.
- Report WiFi, Supabase, audio, bracelet, and battery/fault states through logs and app-visible status when available.
- Cache the last valid alarm config locally when implementing Supabase config loading.

No normal stop or snooze feature should be added unless the user explicitly requests it.

## Data Flow

Target v1 data flow:

`PWA -> Supabase -> station -> bracelet movement -> station controls alarm window`

Do not move alarm authority into the PWA or bracelet without explicit approval.

The station should write its latest state and blocking problem state to Supabase when that contract is implemented, so the PWA can show what is wrong.

## UML Documentation

Keep PlantUML source diagrams in `UML/`.

Update the relevant `.puml` files when changing:

- station class/module boundaries;
- alarm state machine behavior;
- Supabase config/status flow;
- ESP-NOW bracelet communication, channel discovery, and pairing;
- audio player and fallback sound architecture;
- fault-handling paths.

Do not regenerate diagrams automatically on every PlatformIO run. Maintain the `.puml` source manually alongside meaningful code changes. Generated image exports are optional local artifacts; the `.puml` files are the committed source of truth.

## Hardware Assumptions

Respect the hardware list in `../../composants.txt`. Do not change component assumptions without asking first, especially:

- ESP32-32S/ESP32 station target.
- PCM5102A I2S DAC.
- PAM8403 amplifier.
- 4 ohm speaker.
- IP2312 LiPo charging modules.
- Pogo pins and PTC protection.
- 3.3 V LDO.

Pin changes, power changes, charging behavior, and audio-chain changes require careful justification.

## Failure Handling

Do not hide failures.

If WiFi, NTP, Supabase, audio streaming, or bracelet communication fails:

- Log a clear state.
- Keep behavior conservative.
- During an active 10-minute alarm window, publish `bracelet_missing` after 3
  seconds without bracelet packets. Keep audio audible when no confirmed mute
  exists, but honor the remaining deadline of a mute already granted for a
  confirmed movement event. Process retransmitted events only for the unelapsed
  part of their original 10-second interval. Do not mark the active alarm
  revision complete because of packet loss.
- Before resuming audio after a movement mute, require a matching bracelet
  vibration acknowledgement. Resume conservatively after 3 seconds if no
  acknowledgement arrives; never remain silent indefinitely on a missing link.
- Preserve the product rule that the alarm does not get an easy stop path.
- Expose a problem state to the app/backend when that interface exists.

If a blocking technical fault prevents normal operation, enter a visible fault state instead of starting a broken alarm flow. If only Supabase music streaming fails at wake-up time, use the local fallback alarm sound.

## Validation

Run a PlatformIO build for large or risky firmware changes. If the change is small and a build is skipped, state why.

Useful command from this folder:

```powershell
pio run
```

Before finishing, report:

- Files changed.
- UML files updated, or why no UML update was needed.
- Commands run.
- Whether firmware was built.
- Remaining hardware assumptions or untested behavior.
