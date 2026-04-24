# Embedded implementation handoff

This folder is the firmware handoff for implementing a dashcdg receiver on an embedded target such as ESP32 with FreeRTOS. The Windows desktop implementation is the current executable reference for protocol v4 behavior, timing, codec switching, GDI/GL rendering, and soak-test observability.

Start here:

| Document | Purpose |
| --- | --- |
| [`esp-idf-setup.md`](esp-idf-setup.md) | Clone ESP-IDF locally, build `dashcdg_badge`, flash UART (COM), offline/toolchain notes. |
| [`windows-desktop-reference.md`](windows-desktop-reference.md) | Current Windows TX/RX implementation map, threads, data ownership, and important functions. |
| [`protocol-v4-porting-guide.md`](protocol-v4-porting-guide.md) | Wire protocol v4 packet families, receiver state machine, timing, FEC, and embedded parsing rules. |
| [`freertos-esp32-implementation-plan.md`](freertos-esp32-implementation-plan.md) | FreeRTOS task model, queues, memory budgets, scheduler priorities, and staged implementation plan. |
| [`codec-rendering-portability-matrix.md`](codec-rendering-portability-matrix.md) | Codec support matrix, DSP cost notes, renderer choices, and low-end CPU lessons from WinXP/P3. |

Related existing documents:

| Document | Why it matters |
| --- | --- |
| [`../specs/v4-audio-codecs.md`](../specs/v4-audio-codecs.md) | Canonical v4 codec IDs and desktop codec mappings. |
| [`../specs/v4-codec-switching-contract.md`](../specs/v4-codec-switching-contract.md) | Runtime codec switching behavior and receiver obligations. |
| [`../specs/v4-live-video-playout.md`](../specs/v4-live-video-playout.md) | Live CD+G anchor/delta playout behavior. |
| [`../specs/v4-display-audio-sync.md`](../specs/v4-display-audio-sync.md) | Audio/display sync policy. |
| [`../specs/audio-jitter-playout-boundary.md`](../specs/audio-jitter-playout-boundary.md) | Core audio jitter buffer contract. |
| [`../specs/cdg-batch-jitter-playout-boundary.md`](../specs/cdg-batch-jitter-playout-boundary.md) | CDG jitter and skip behavior. |
| [`../hardware/esp32-receiver-architecture.md`](../hardware/esp32-receiver-architecture.md) | Earlier ESP-IDF architecture notes. |

## Porting rule

Port from protocol and core boundaries first, not from the Windows UI. The desktop application is the reference for behavior, but firmware should preserve its observable semantics with a smaller task graph and less dynamic allocation.

Required embedded behavior:

- Parse v4 packets from UDP multicast or broadcast.
- Apply `v4_session_info` as the authoritative session and codec contract.
- Use sender-clock timing from `v4_clock_sync` and PTP-like exchanges.
- Reconstruct video from v4 anchors plus video deltas.
- Buffer audio in a jitter queue before decoding.
- Recover from late join, track switch, pause/unpause, and packet loss without restarting firmware.

Initial embedded cut:

- Video-only v4 RX is valid and useful.
- Audio decode may start with fixed-point NB-IMA (`audio_codec_id = 2`) before AMR/QCELP/Opus.
- RX stats may be disabled on constrained systems unless required for lab instrumentation.
