# AI Agent Guide

## Project Vision

This project is a personal wake-up system designed to make going back to sleep difficult. The final product has three active parts:

- `code/boite hp`: bedside speaker station and central alarm authority.
- `code/bracelet`: dedicated wearable that validates sustained user activity.
- `code/app mobile`: Vue/Vite PWA used to upload music, select wake-up audio, and configure the next alarm.

Supabase is the v1 backend for music storage and alarm configuration. The configuration path is:

`PWA -> Supabase -> bedside station`

The bracelet is dedicated to the station. The station decides when the alarm starts and when it stops. The PWA is not the alarm authority.

For intersystem contracts, states, communication paths, Supabase schema, ESP-NOW packets, failure policy, and secrets rules, read `ARCHITECTURE.md`.

## Current Repository Map

- `vision.txt`: early product vision. Useful context, but not always fully up to date.
- `ARCHITECTURE.md`: canonical contracts between app, station, bracelet, and Supabase.
- `.gitignore`: root ignore rules for secrets, build artifacts, generated files, and local tool state.
- `composants.txt`: hardware component list. Treat listed components as fixed unless the user approves a change.
- `code/app mobile`: future Vue/Vite PWA. Currently the app area.
- `code/app mobile/UML`: PlantUML diagrams for app use cases and user-facing flows.
- `code/boite hp`: PlatformIO Arduino ESP32 station firmware.
- `code/boite hp/UML`: PlantUML diagrams for station firmware architecture.
- `code/bracelet`: PlatformIO Arduino ESP32-C3 bracelet firmware.
- `code/bracelet/UML`: PlantUML diagrams for bracelet firmware architecture.
- `schema/bracelet`: KiCad source files for bracelet electronics.

Keep the current folder names, including spaces in `app mobile` and `boite hp`, unless the user explicitly asks to rename them.

## V1 Definition

V1 is complete when:

- The PWA uploads music to Supabase.
- The PWA lists and selects wake-up music.
- The PWA configures one alarm for tomorrow.
- The station synchronizes time with NTP over WiFi.
- The station fetches alarm configuration from Supabase.
- The station streams the selected Supabase audio at alarm time.
- The station uses a small local fallback alarm sound if Supabase music is unreachable at wake-up time.
- The bracelet validates sustained movement.
- The station stops only after valid bracelet activity.

Do not add advanced features before this v1 is working: recurring alarms, multi-user accounts, sleep cycle analysis, playlists, statistics, or production authentication.

## Non-Negotiable Product Rules

- The station remains the central controller.
- Supabase is used for music files and alarm configuration in v1.
- Bracelet-to-station communication uses ESP-NOW in v1, not BLE, unless physical range tests force a redesign.
- The bracelet is dedicated to the station.
- The alarm must not gain a normal stop or snooze button unless the user explicitly requests it.
- The only normal stop condition is bracelet-validated sustained activity.
- Blocking technical faults should put the system in a visible fault state instead of starting a broken alarm flow.
- Music streaming failure should fall back to a small local alarm sound.

## Hardware Rules

The component choices in `composants.txt` are fixed unless the user approves a change. Do not replace or redesign around these without asking first:

- ESP32 station controller.
- ESP32-C3 bracelet controller.
- BMI270 motion sensor.
- PCM5102A I2S DAC.
- PAM8403 audio amplifier.
- IP2312 LiPo charging modules.
- 1S LiPo batteries.
- Pogo pins.
- PTC resettable protection.
- 3.3 V LDOs.

For hardware, PCB, charging, battery, connector, or enclosure decisions, ask before making large changes.

## Security And Backend Rules

Supabase without authentication is acceptable only as a personal prototype shortcut for v1. Do not present it as production security.

If touching backend access, storage policies, URLs, secrets, or device identity:

- State whether the result is prototype-only or production-ready.
- Do not commit private secrets.
- Keep example secrets in example files only.
- Keep `secrets.h`, `.env`, `.env.local`, WiFi credentials, and private Supabase keys out of git.
- Ask before introducing production auth, multi-user accounts, or device ownership flows.

## Working Method For Agents

Before coding:

- Read the relevant local `AGENTS.md`.
- Inspect the current implementation instead of relying only on old notes.
- Keep changes scoped to the requested subsystem.
- Inspect existing `.puml` diagrams before changing app use cases, user-facing flows, station architecture, or bracelet architecture.

During coding:

- Prefer existing project conventions and toolchains.
- Avoid broad refactors unless required.
- Ask before changing architecture, hardware assumptions, security model, data flow, or folder structure.
- Ask before changing any contract defined in `ARCHITECTURE.md`.
- Update relevant PlantUML diagrams when changing app use cases, firmware architecture, class boundaries, state machines, or inter-module communication.
- Update the relevant `AGENTS.md` when project rules, architecture decisions, commands, or subsystem responsibilities change.
- Preserve user changes and unrelated files.

Before finishing:

- Report files changed.
- Report UML files updated, or explain why no diagram update was needed.
- Report commands run.
- Report assumptions made.
- Report remaining risks or skipped validation.

## Validation Expectations

- PWA changes: run build and local preview when possible.
- Station firmware changes: run PlatformIO build for large or risky changes.
- Bracelet firmware changes: run PlatformIO build for large or risky changes.
- Documentation changes: keep all `AGENTS.md`, `ARCHITECTURE.md`, and relevant PlantUML files consistent.

## UML Documentation Rule

Project behavior and firmware architecture must remain understandable to a human reader. The app, station, and bracelet projects each keep PlantUML source files in a root-level `UML` folder:

- `code/app mobile/UML`
- `code/boite hp/UML`
- `code/bracelet/UML`

The app must keep at least one simple UML use-case diagram. Do not regenerate diagrams automatically on every run or build. Instead, update the `.puml` source files manually as part of each meaningful product, flow, or architecture change. Generated images such as `.png` or `.svg` may be produced locally for review, but the `.puml` files are the source of truth.
