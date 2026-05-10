# AI Agent Guide: Bracelet

## Role

This folder contains the bracelet firmware. The bracelet validates sustained user activity and reports that validation to the bedside station.

The bracelet does not own alarm state. The station decides whether the alarm stops.

Read `../../ARCHITECTURE.md` before changing bracelet telemetry, activity validation thresholds, battery readiness, ESP-NOW packet format, or station communication.

Read and maintain the PlantUML diagrams in `UML/` when changing bracelet firmware architecture. These diagrams are for human understanding and must evolve with the code.

## Current State

This is a PlatformIO Arduino ESP32-C3 project.

The current firmware is a minimal template. Do not assume BMI270 support, ESP-NOW communication, battery monitoring, power management, or activity validation exists unless the code proves it.

## Target Behavior

The bracelet should:

- Read motion data from the BMI270.
- Detect roughly continuous physical activity.
- Report activity state and validation events to the station over ESP-NOW.
- Report low-battery and fault states when available.
- Preserve battery life where possible.
- Stay dedicated to one station.

The anti-cheat goal is practical: prevent the user from immediately going back to sleep by requiring sustained activity. The goal is not to prove perfect walking, GPS-like displacement, or medical-grade activity detection.

## Validation Logic Direction

Prefer a validation model based on sustained movement over time, not a single shake spike.

Future implementations should consider:

- Minimum activity duration.
- Movement continuity.
- Rejection of isolated bumps.
- Sensor fault detection.
- Battery state.
- Clear telemetry for debugging thresholds.

Do not over-engineer sleep analysis or advanced health metrics before v1 works.

## Hardware Assumptions

Respect the hardware list in `../../composants.txt` and the KiCad source in `../../schema/bracelet`.

Do not change these assumptions without asking:

- ESP32-C3 bracelet controller.
- BMI270 accelerometer/gyroscope.
- 1S LiPo battery.
- Pogo-pin charging interface.
- PTC resettable protection.
- 3.3 V LDO.
- Compact wearable form factor.

## Communication

Target station link is ESP-NOW.

The bracelet should send enough telemetry for the station to decide alarm state, but the station remains authoritative.

Do not introduce WiFi-heavy bracelet behavior unless explicitly requested; bracelet power consumption matters.

Room-to-room ESP-NOW range is an assumption that must be physically tested. If it is not reliable enough for the target house distance, ask before changing the transport.

## UML Documentation

Keep PlantUML source diagrams in `UML/`.

Update the relevant `.puml` files when changing:

- bracelet class/module boundaries;
- BMI270 sensor flow;
- activity scoring and validation flow;
- ESP-NOW telemetry format or send behavior;
- battery readiness logic;
- fault-handling paths.

Do not regenerate diagrams automatically on every PlatformIO run. Maintain the `.puml` source manually alongside meaningful code changes. Generated image exports are optional local artifacts; the `.puml` files are the committed source of truth.

## Validation

Run a PlatformIO build for large or risky firmware changes. If the change is small and a build is skipped, state why.

Useful command from this folder:

```powershell
pio run
```

Temporary BMI270 movement-threshold measurement firmware:

```powershell
pio run -e bracelet-thresholds -t upload
pio device monitor -b 115200
```

The threshold firmware is a development tool only. It measures immobile and real-use movement data, then suggests `ACTIVE_ACCEL_DELTA_G` and `ACTIVE_GYRO_DPS` values for the product firmware. The older `bracelet-calibration` environment remains as an alias for this same threshold tool.

Before finishing, report:

- Files changed.
- UML files updated, or why no UML update was needed.
- Commands run.
- Whether firmware was built.
- Remaining sensor, ESP-NOW, range, or hardware assumptions.
