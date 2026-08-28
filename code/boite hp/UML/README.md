# Station UML

This folder contains PlantUML source files for the bedside station firmware architecture.

Rules:

- Commit `.puml` source files.
- Update diagrams manually when architecture, state machines, module boundaries, ESP-NOW flow, Supabase flow, audio flow, fallback behavior, or fault handling changes.
- Do not regenerate diagrams automatically during normal PlatformIO builds.
- Generated image exports such as `.png` or `.svg` are optional local review artifacts; the `.puml` files are the source of truth.

Suggested starting diagrams:

- `station_state_machine.puml`
- `communication_flow.puml`
