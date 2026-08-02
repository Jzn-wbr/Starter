# AI Agent Guide: Documentation

## Role

This folder contains the manually maintained project report.

The report is intended for an engineer or developer who does not know the project and needs enough context to understand the codebase, architecture, contracts, and current implementation.

Read `../AGENTS.md` and `../ARCHITECTURE.md` before changing this folder.

## Source Of Truth

- `rapport.tex` is the report source.
- `rapport.pdf` is the compiled report delivered to readers.
- The PlantUML `.puml` files in each subsystem remain the UML source of truth.
- The generated `.png` UML renders are kept next to their `.puml` sources only so the PDF can include them.

## Manual Update Policy

Do not regenerate this documentation automatically during normal builds, firmware builds, app builds, tests, or agent runs.

Update `rapport.tex`, regenerate the UML PNG files, and compile `rapport.pdf` only when the user explicitly asks to update the documentation or report.

## UML Render Policy

When the report needs fresh UML diagrams, generate PNG files next to the `.puml` files that produced them:

- `../code/app mobile/UML/use_cases.png`
- `../code/boite hp/UML/station_state_machine.png`
- `../code/boite hp/UML/communication_flow.png`
- `../code/bracelet/UML/bracelet_firmware_flow.png`
- `../code/bracelet/UML/esp_now_packet_flow.png`

Do not move generated UML images into `doc/`. Keeping them beside their source makes the origin clear.

## Build Commands

From the repository root, generate UML PNG files with PlantUML:

```powershell
java -jar doc\tools\plantuml.jar -tpng "code\app mobile\UML\use_cases.puml" "code\boite hp\UML\station_state_machine.puml" "code\boite hp\UML\communication_flow.puml" "code\bracelet\UML\bracelet_firmware_flow.puml" "code\bracelet\UML\esp_now_packet_flow.puml"
```

Then compile the report:

```powershell
latexmk -pdf -interaction=nonstopmode -halt-on-error rapport.tex
```

Run the LaTeX command from inside `doc/`.

## Content Rules

- Keep the report in French unless the user asks otherwise.
- Separate implemented behavior from target v1 contracts when the distinction matters.
- State clearly that the current Supabase no-auth model is prototype-only and not production-ready.
- Do not describe recurring alarms, multi-user authentication, playlists, statistics, sleep-cycle analysis, or production account flows as v1 features.
- Do not commit private secrets, WiFi credentials, Supabase service-role keys, `.env`, `.env.local`, or `secrets.h`.
- If architecture contracts change, update `../ARCHITECTURE.md`, the affected subsystem `AGENTS.md`, the relevant `.puml` files, and this report together.

## Before Finishing

Report:

- files changed;
- UML PNG files regenerated;
- commands run;
- assumptions made;
- any skipped validation.
