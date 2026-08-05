# AI Agent Guide: Bracelet

## Role

This folder contains the bracelet firmware. The bracelet reports current user movement to the bedside station.

The bracelet does not own alarm state. The station decides whether alarm audio plays and when the fixed alarm window is complete.

Read `../../ARCHITECTURE.md` before changing bracelet telemetry, activity validation thresholds, battery readiness, WiFi UDP packet format, or station communication.

Read and maintain the PlantUML diagrams in `UML/` when changing bracelet firmware architecture. These diagrams are for human understanding and must evolve with the code.

## Current State

This is a PlatformIO Arduino ESP32-C3 project. The current firmware reads the
BMI270 at 50 Hz, calculates 200 ms energy windows, reports battery and fault
state, connects to the configured home WiFi networks, discovers its paired
station through UDP announcements, and pulses the vibration motor while
excluding motor noise from movement energy.

WiFi UDP protocol v4 retains the newest qualifying movement event for up to 10
seconds, retransmits it until the station acknowledges it, and confirms a
vibration request only after the motor output actually starts. Status packets
also report the RSSI of the currently connected WiFi network. The first valid
station is paired in NVS. Station and bracelet firmware must be flashed together.

## Target Behavior

The bracelet should:

- Read motion data from the BMI270.
- Detect current physical movement.
- Report activity state and movement events to the station over WiFi UDP.
- Report low-battery and fault states when available.
- Preserve battery life where possible.
- Stay dedicated to one station.

The anti-cheat goal is practical: make the first 10 minutes after the alarm time require movement to keep the station quiet. The goal is not to prove perfect walking, GPS-like displacement, or medical-grade activity detection.

## Validation Logic Direction

For v1, the bracelet reports current movement. The station owns the fixed 10-minute alarm activity window.

Future implementations should consider:

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

Target station link is binary UDP over the shared home WiFi network.

The bracelet should send enough telemetry for the station to decide alarm state, but the station remains authoritative.

WiFi UDP protocol v4 retains the newest qualifying 200 ms energy event for up to
10 seconds and retransmits it with its age until the station acknowledges it.
The bracelet also acknowledges a vibration request only after the motor has
actually started. The bracelet sends status every 10 seconds outside an active
alarm and every 200 ms while ringing or validating activity, with immediate
status for important changes. Station and bracelet firmware must be updated
together; older packet versions are not accepted.

The bracelet stays associated to WiFi, enables radio sleep outside the active
alarm, and disables it during the alarm. Battery life and room-to-room WiFi
coverage remain physical validation requirements.

## UML Documentation

Keep PlantUML source diagrams in `UML/`.

Update the relevant `.puml` files when changing:

- bracelet class/module boundaries;
- BMI270 sensor flow;
- activity scoring and validation flow;
- WiFi UDP telemetry format, pairing, discovery, or send behavior;
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
- Remaining sensor, WiFi UDP, range, battery, or hardware assumptions.
