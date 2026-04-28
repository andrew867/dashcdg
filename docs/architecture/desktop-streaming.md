# Desktop Streaming Architecture

## Purpose

This document is the developer handoff for the active desktop TX/RX proof. Read this first if you need to understand how the repo's networked karaoke path actually works today, what is portable, and what is still proof-grade.

## High-Level Topology

```mermaid
flowchart LR
    lib[Song library<br/>CDG plus optional MP3] --> tx[desktop-tx]
    tx --> net[UDP multicast or IPv4 broadcast]
    net --> rx[desktop-rx or<br/>desktop-gdi-rx]
    rx --> aud[PortAudio playout]
    rx --> vid[OpenGL or<br/>Win32 GDI CDG view]
```

The sender and receiver share one protocol and one timeline:

- TX publishes session metadata, live media, repair packets, and timing traffic.
- RX rebuilds enough session state to start quickly, then continues toward deterministic late-join completeness in the background.
- Audio becomes the playout master once steady-state playback starts.

## Repository Roles

- `core/src/cdg.c`: deterministic CD+G packet application, snapshotting, and seek primitives
- `core/src/media_clock.c`: bounded remote/local time discipline helpers
- `proto/src/protocol.c`: packet serialization and parsing for protocol v4 (with legacy v3 compatibility paths)
- `platform/desktop/src/app_tx.c`: TX state machine, scheduler, playlist logic, pause screen, PTP master behavior, and FEC generation
- `platform/desktop/src/app_rx.c`: RX session state, jitter queues, FEC recovery, snapshot apply, PTP slave behavior, and render/audio startup gates
- `platform/desktop/src/desktop_audio.c`: queue-driven PortAudio backend used by RX network playback
- `platform/desktop/src/opus_codec.c`: Opus encode/decode wrapper for the desktop proof
- `platform/desktop/src/gl_renderer.c`: desktop CDG rendering path and HUD drawing (GL RX / TX preview)
- `platform/desktop/src/win32_gdi_view.c`: Win32 **GDI** window + DIBSection blit for `desktop-gdi-rx.exe` (and retro GDI RX)

## End-to-End Packet Flow

```mermaid
flowchart LR
    prep[Track load plus prepare] --> v4meta[V4_SESSION_INFO]
    prep --> v4clock[V4_CLOCK_SYNC]
    prep --> va[V4_VIDEO_ANCHOR]
    prep --> vd[V4_VIDEO_DELTA]
    prep --> ac[V4_AUDIO_CHUNK]
    vd --> rw[V4_REPAIR_WINDOW]
    ac --> rw
    v4meta --> rxstate[RX session bootstrap]
    v4clock --> rxstate
    va --> rxstate
    ac --> jitter[Audio jitter queue]
    vd --> jitter2[CDG jitter queue]
    rw --> repair[FEC repair attempt]
    repair --> jitter
    repair --> jitter2
    jitter --> decode[Codec decode]
    decode --> pa[PortAudio stream]
    jitter2 --> live[Live CDG state]
    pa --> sync[Audio-led playout time]
    sync --> render[OpenGL or GDI<br/>CDG presentation]
    live --> render
```

On **Windows**, `desktop-rx` uses FreeGLUT + OpenGL by default; the same session
logic can be linked as **`desktop-gdi-rx.exe`**, which always presents through
GDI (no GL stack). **`desktop-retro-rx.exe`** is a separate minimal link: GDI
view + SBC-like audio path without Opus (see
`docs/specs/desktop-platform-support.md`).

## TX Runtime

### Startup and Track Load

When TX starts or changes tracks, it:

1. resolves a song or directory entry
2. loads the `.cdg` asset for bootstrap replay
3. pairs an `.mp3` if present
4. prepares a bounded audio-production queue for live send
5. opens a new session with a default `1000 ms` warmup
6. lets the dedicated TX audio thread stream MP3 decode, resample to `48 kHz`,
   downmix to mono, and encode rolling `20 ms` Opus frames just ahead of send

Important current behavior:

- TX still performs CD+G batch schedule preparation synchronously on the
  track-change hot path.
- TX audio production is no longer whole-track pre-encode; a dedicated producer
  thread fills a bounded `audio_ready_queue` during live send.
- default TX wire send can now use a file-backed random-access CDG source;
  preview mode still uses a whole-memory fallback reader
- TX emits **protocol v4** by default (session info, loading screens, anchors,
  bounded per-pass pacing for anchors/audio/video/backfill). **`--v3`** selects
  the legacy v3-only sender loop. **`--badnet-v4`** sets v4 **resilience** and
  current desktop default codec **`amr-wb`**; **`--badnet-v4-sbc`** and
  **`--badnet-v4-qcelp8k`** pick explicit alternates. **`--v4-audio-codec=`**
  selects the session `audio_codec_id` without changing transport version.
- Console output now prints a preparation line immediately, but some media
  preparation cost is still on the hot path.
- Directory playback defaults to the local `cdg/` folder when no TX source path is provided.
- TX shuffles on initial directory load and reshuffles when the playlist wraps.

### Scheduler Responsibilities

`app_tx.c` is responsible for:

- periodic `V4_SESSION_INFO` and `V4_CLOCK_SYNC`
- live `V4_AUDIO_CHUNK` send
- live `V4_VIDEO_DELTA` send
- bounded `V4_REPAIR_WINDOW` generation
- periodic `V4_VIDEO_ANCHOR` generation
- pause-screen snapshot generation
- preview HUD and terminal stats
- operator commands for pause, next, back, restart, rebroadcast, and visibility

Current threading split inside the desktop proof:

- TX audio production happens on `dashcdg_tx_audio_thread_main()`
- TX packet pacing and control traffic still run in the main TX scheduler loop
- CD+G batch creation is still done up front as schedule metadata
- the canonical CDG source may now be file-backed for send paths while preview
  remains memory-backed

### Pause Mode

Pause is not just a local UI stop:

- TX sets the paused flag in packet headers
- normal audio/CDG advancement is suppressed
- TX repeatedly sends a generated pause-screen snapshot
- RX treats that snapshot as healthy session state and keeps the window/headless status alive
- resume returns to live scheduling from the paused media position

## RX Runtime

### Session Bring-Up

RX combines bootstrap and live playout:

1. `V4_SESSION_INFO` prepares or resets session state
2. RX anchors the sender clock immediately from a fresh announce on new sessions or track changes
3. `V4_VIDEO_ANCHOR` can immediately seed the live framebuffer before full steady-state deltas
4. `V4_AUDIO_CHUNK` and `V4_VIDEO_DELTA` enter bounded jitter queues
5. `V4_REPAIR_WINDOW` is used to recover bounded losses in protected groups when possible
7. PortAudio startup happens asynchronously so GUI/headless status does not stall

### Clocking

The desktop proof uses software-timestamped PTP-style exchanges:

- `PTP_SYNC`
- `PTP_FOLLOW_UP`
- `PTP_DELAY_REQ`
- `PTP_DELAY_RESP`

Current RX behavior:

- fresh announces re-anchor after track changes
- stale PTP exchanges are discarded after a bounded age window
- clock telemetry tracks offset, path delay, step size, peak correction, and holdover age
- once audio is flowing steadily, audio playout time becomes the render master

This is good enough for desktop proof work, but it is not hardware-timestamped PTP.

### Jitter and Recovery

RX keeps bounded pending queues for both live streams:

- audio frames can arrive slightly out of order and still be recovered/applied
- CDG batches can arrive slightly out of order and still be recovered/applied
- overdue gaps are skipped instead of wedging the session forever
- per-pass media drain is bounded so render/status threads remain responsive

Current FEC behavior:

- XOR parity is generated across short groups
- one missing media payload in a fully covered group can be reconstructed
- burst loss beyond one member per group is expected to exceed the current repair model

## Late Join and Recovery Model

Late join now combines three layers:

1. repeated `V4_SESSION_INFO` for session discovery and config
2. periodic `V4_VIDEO_ANCHOR` for deterministic visual bootstrap/repair
3. steady `V4_VIDEO_DELTA` + repair windows for ongoing state convergence

That means:

- a newly started RX can show live video quickly from snapshot state
- the full asset continues to rebuild behind the scenes
- once the asset is complete, RX regains deterministic seek/backfill behavior

Audio does not yet have an equivalent standalone keyframe concept; audio recovery is currently based on playout delay, jitter queues, and bounded FEC repair rather than a separate audio-state snapshot.

## Observability

Both TX and RX expose proof telemetry in the HUD and/or stdout:

- asset/bootstrap progress
- playout gates such as `wait-ptp`, `wait-preroll`, and running state
- audio and live CDG packet counters
- reorder and skip counts
- FEC group, parity, repair, and failure counters
- snapshot receive/apply counters
- PTP offset, path delay, step, peak, and holdover values

Use `docs/test/desktop-impairment-validation.md` for the repeatable impaired-network matrix and `docs/test/desktop-proof-plan.md` for the intended proof claims.

## What Is Portable vs Desktop-Specific

Portable/reusable today:

- CDG decode semantics
- media clock math
- wire protocol framing

Desktop-specific today:

- socket setup and network threads
- PortAudio stream management
- OpenGL renderer and preview HUD
- track preparation, playlist controls, and current TX scheduler policy
- pause-screen generator art

## Current Gaps

- TX track preparation is still synchronous for CD+G asset and batch setup even
  though audio encode is now incremental.
- TX no longer requires a full `.cdg` preload for default wire send, but the
  preview path still keeps a whole-memory fallback.
- The clock loop is software timestamped and millisecond scale, not venue-grade hardware timestamping.
- FEC is intentionally bounded and only repairs one missing payload per group.
- Long impaired-network soak validation is still incomplete.
- Embedded receiver implementation exists (`platform/espidf/projects/dashcdg_badge/main/badge_rx.c`) and tracks the desktop v4 contract with platform-specific constraints.

## Suggested Reading Order

1. `README.md`
2. `docs/README.md`
3. `docs/architecture/desktop-streaming.md`
4. `docs/specs/desktop-platform-support.md`
5. `docs/specs/transport-protocol.md`
6. `docs/test/desktop-proof-plan.md`
7. `docs/test/desktop-impairment-validation.md`
8. `docs/architecture/portable-core.md`
