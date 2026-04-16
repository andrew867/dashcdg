# Portable Core Extraction Plan

## Module boundary

The portable core is the union of:

- `core/src/cdg.c`
- `core/src/media_clock.c`
- `proto/src/protocol.c`

The core is intentionally free of renderer, audio-device, and socket runtime dependencies.

## Responsibilities

### `cdg`

- decode raw CD+G packets
- maintain deterministic palette/framebuffer state
- support forward replay and backward seek through keyframes
- expose enough render metadata for smooth scrolling and transparency

### `media_clock`

- provide a monotonic timestamp source
- maintain a bounded remote/local offset estimate
- let receivers gradually discipline to transport timing packets without large jumps

Current state:

- this now includes software-timestamped `PTP_SYNC` / `PTP_FOLLOW_UP` / `PTP_DELAY_REQ` / `PTP_DELAY_RESP`
- it is still a bounded software estimator rather than a hardware-timestamped or sub-millisecond discipline loop

### `protocol`

- define versioned packet framing
- serialize and parse announce, asset, clock, live media, and future-repair packet types
- provide a fixed binary contract for desktop and future ESP-IDF transports

Current protocol v3 coverage includes:

- `ANNOUNCE`
- `ASSET_CHUNK`
- `CLOCK_BEACON`
- `AUDIO_FRAME`
- `CDG_BATCH`
- `CDG_SNAPSHOT`
- `PTP_SYNC`
- `PTP_FOLLOW_UP`
- `PTP_DELAY_REQ`
- `PTP_DELAY_RESP`
- active desktop use of `FEC_PARITY` for bounded single-loss repair groups

Important boundary note:

- wire framing and packet semantics are portable
- current jitter-buffer policy, snapshot scheduling policy, pause-screen generation, and playlist/operator behavior still live in the desktop app layer rather than the portable core

## Core correctness strategy

- unit tests drive packet semantics and binary framing
- keyframes are built from semantic state, not renderer internals
- backward seeks restore from the nearest snapshot and replay forward
- transparency and scroll offsets are treated as first-class state, not renderer-only quirks

## Future extraction steps

The next logical refactor after the current desktop proof tranche is:

1. **Transport adapter (RX UDP)** — **In progress / landed:** see
   [`../specs/transport-udp-boundary.md`](../specs/transport-udp-boundary.md) and
   [`transport-and-playout-modules.md`](transport-and-playout-modules.md).
2. **Playout / jitter (audio)** — **In progress / landed:** pure module in core; see
   [`../specs/audio-jitter-playout-boundary.md`](../specs/audio-jitter-playout-boundary.md).
   PortAudio remains the device boundary in `desktop_audio.c`; jitter no longer
   depends on it.
3. **Render-surface abstraction** — **Partial:** CPU RGBA contract is normative; see
   [`../specs/cpu-rgba-raster-contract.md`](../specs/cpu-rgba-raster-contract.md).
   OpenGL path consumes the same buffer (single pixel source of truth). Optional
   GDI/D3D backends remain future work.
4. **Golden / headless** — satisfied by `dashcdg_cdg_state_to_rgba8` unit tests in
   [`../test/cpu-rgba-raster-validation.md`](../test/cpu-rgba-raster-validation.md).
