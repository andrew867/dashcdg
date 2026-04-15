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

Current protocol v2 coverage includes:

- `ANNOUNCE`
- `ASSET_CHUNK`
- `CLOCK_BEACON`
- `AUDIO_FRAME`
- `CDG_BATCH`
- `PTP_SYNC`
- `PTP_FOLLOW_UP`
- `PTP_DELAY_REQ`
- `PTP_DELAY_RESP`
- declared but not yet active desktop use of `FEC_PARITY`

## Core correctness strategy

- unit tests drive packet semantics and binary framing
- keyframes are built from semantic state, not renderer internals
- backward seeks restore from the nearest snapshot and replay forward
- transparency and scroll offsets are treated as first-class state, not renderer-only quirks

## Future extraction steps

The next logical refactor after this commit is:

1. move desktop TX/RX socket handling into a reusable transport adapter
2. split a pure playout/jitter-buffer abstraction from the desktop PortAudio implementation
3. split a pure render-surface abstraction from the desktop OpenGL implementation
4. add a second renderer backend that rasterizes to a host-side RGBA buffer for golden tests and headless CI
