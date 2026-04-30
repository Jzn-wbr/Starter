# AI Agent Guide: Bracelet Hardware

## Role

This folder contains KiCad source files for the bracelet electronics. Treat these files as hardware source of truth for the bracelet design.

Coordinate hardware changes with:

- `../../composants.txt`
- `../../ARCHITECTURE.md`
- `../../code/bracelet`
- The physical constraints of a compact wearable bracelet

## Current State

This area contains KiCad project files for the bracelet. Inspect the schematic and PCB files before making any assumptions about what is already implemented.

Do not assume the schematic is complete just because a component appears in `composants.txt`.

## Hardware Constraints

The bracelet target includes:

- ESP32-C3 controller.
- BMI270 accelerometer/gyroscope.
- 1S LiPo battery.
- Pogo-pin charging contacts.
- PTC resettable protection.
- 3.3 V regulation.
- ESP-NOW communication with the station.
- Compact 3D-printed enclosure.
- Elastic strap.

The component choices in `../../composants.txt` are fixed unless the user approves a change.

## Change Rules

Ask before changing:

- MCU/module choice.
- Sensor choice.
- Battery capacity or chemistry.
- Charging topology.
- Pogo-pin connector strategy.
- Protection components.
- Regulator topology.
- Footprints.
- Board outline or mechanical assumptions.
- Any decision that affects enclosure design or firmware pinout.
- Any antenna, enclosure, or layout decision that could reduce ESP-NOW range.

Do not make silent schematic changes that require firmware pin changes.

## Design Expectations

When working on the schematic or PCB:

- Keep power, charging, and protection behavior explicit.
- Keep signal names readable and aligned with firmware expectations.
- Document assumptions that require physical testing.
- Prefer simple, buildable prototype hardware over unnecessary complexity.

## Validation

Before finishing hardware work, report:

- Files changed.
- Electrical assumptions.
- Firmware impacts.
- Mechanical/enclosure impacts.
- Checks performed or skipped.

If ERC/DRC or KiCad exports are run, report the exact command/tool and result.
