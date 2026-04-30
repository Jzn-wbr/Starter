# AI Agent Guide: Mobile PWA

## Role

This folder is for the mobile PWA. The target stack is Vue/Vite.

The app is the user interface for:

- Uploading wake-up music.
- Listing available music.
- Selecting the music for the next alarm.
- Configuring one alarm for tomorrow.
- Displaying station, bracelet, backend, and fault states.

The app is not the alarm authority. It configures the system through Supabase; the station owns alarm behavior.

Read `../../ARCHITECTURE.md` before changing Supabase tables/fields, status display, alarm config shape, or intersystem assumptions.

Read and maintain the PlantUML diagrams in `UML/` when changing app use cases or user-facing flows. The app must keep at least a simple UML use-case diagram.

## Current State

This folder may be empty or early-stage. Inspect it before assuming a framework is already installed.

If creating the app, use Vue/Vite unless the user explicitly chooses another stack.

## Target Data Flow

V1 target:

`PWA -> Supabase -> station`

The app should write music files and alarm configuration to Supabase. The station reads the selected alarm configuration from Supabase.

Do not make the app directly responsible for triggering or stopping the alarm.

The app should also read station-published problem/status state from Supabase when that contract is implemented. It does not talk directly to the station or bracelet in v1.

## Supabase And Auth

Supabase is required in v1 for:

- Audio file storage.
- Music listing.
- Selected wake-up track.
- Next-alarm configuration.
- Station and bracelet status as reported by the station.

No authentication is accepted only as a personal prototype shortcut. Always document this as a known security limitation, not a production-ready design.

Do not add production auth, multi-user accounts, device ownership, or complex access control unless the user asks for it.

Never commit real Supabase secrets. Use environment examples for public configuration and keep private keys out of source control.

Follow the root `.gitignore`: commit `.env.example` if needed, but never commit `.env`, `.env.local`, service-role keys, or private tokens.

## UI Scope

Build the actual app experience, not a landing page.

For v1, keep scope focused:

- Music upload.
- Music selection.
- Tomorrow alarm time.
- Status/problem display.

Do not add recurring alarms, playlists, sleep-cycle analysis, statistics, account management, or unrelated dashboards before v1 is complete.

## UML Documentation

Keep PlantUML source diagrams in `UML/`.

The app must include a use-case diagram for the v1 user workflow. Update the relevant `.puml` files when changing:

- app use cases;
- music upload or selection flow;
- next-alarm configuration flow;
- station/status problem display;
- Supabase-backed app behavior.

Do not regenerate diagrams automatically on every dev-server run or build. Maintain the `.puml` source manually alongside meaningful app changes. Generated image exports are optional local artifacts; the `.puml` files are the committed source of truth.

## Validation

For app code changes, run build and local preview when possible.

Typical commands, depending on the generated project:

```powershell
npm install
npm run build
npm run dev
```

Before finishing, report:

- Files changed.
- UML files updated, or why no UML update was needed.
- Commands run.
- Local preview URL if started.
- Any skipped validation and why.
