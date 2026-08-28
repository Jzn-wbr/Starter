# Bracelet UML

This folder contains PlantUML source files for the bracelet firmware architecture.

Rules:

- Commit `.puml` source files.
- Update diagrams manually when architecture, state machines, module boundaries, BMI270 flow, activity validation, ESP-NOW telemetry, battery readiness, or fault handling changes.
- Do not regenerate diagrams automatically during normal PlatformIO builds.
- Generated image exports such as `.png` or `.svg` are optional local review artifacts; the `.puml` files are the source of truth.

Suggested starting diagrams:

- `bracelet_firmware_flow.puml`
- `esp_now_packet_flow.puml`
