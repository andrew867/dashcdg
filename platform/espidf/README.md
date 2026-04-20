# ESP-IDF Platform Stub

This directory is reserved for the future ESP-IDF transport, display, storage, and OTA adapters that will consume the portable `core/` and `proto/` libraries.

**Desktop prerequisite:** Implementations should mirror the stabilized **protocol v4** receiver and timing rules validated on **Windows desktop-rx** (session_info reconfigure, jitter priming, `playback_base_*` / clock ownership, codec hot-swap, cold join, pause/resume). See **[`AGENTS.md`](../../AGENTS.md)**, **[`docs/hardware/esp32-receiver-architecture.md`](../../docs/hardware/esp32-receiver-architecture.md)**, and **[`.cursor/plans/esp32_embedded_enterprise_plan_b3bda7b3.plan.md`](../../.cursor/plans/esp32_embedded_enterprise_plan_b3bda7b3.plan.md)**.

The intended module split is:

- `transport/`
- `display/`
- `storage/`
- `input/`
- `power/`
- `ota/`
