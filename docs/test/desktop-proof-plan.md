# Desktop TX/RX Proof Plan

## Scope

This tranche proves the architecture on computer hardware before MCU work:

- `apps/desktop-tx/main.c`: multicast transmitter for bootstrap assets, live Opus frames, timed CD+G batches, and basic PTP-style sync traffic
- `apps/desktop-rx/main.c`: multicast receiver with live network audio decode, bootstrap asset reconstruction, and OpenGL rendering
- `apps/desktop-player/main.c`: local non-network player for baseline regression checks

## Success criteria

- receiver can discover a session from `ANNOUNCE`
- receiver reconstructs the full CD+G asset from repeated `ASSET_CHUNK` packets
- receiver renders the rebuilt asset with deterministic seeking after late join
- receiver can also advance a live CD+G state from `CDG_BATCH`
- receiver starts network Opus audio near the announced playout boundary
- receiver follows network clock traffic before the bootstrap asset is fully complete
- headless RX and TX status output make stalls and packet flow visible

## Observability requirements

- packet sequence numbers for capture/replay
- sender monotonic timestamps for drift analysis
- explicit asset size and chunk size in announces
- explicit live media counters for audio, timed CD+G, and sync traffic
- RX audio queue depth and decode-failure visibility
- deterministic test vectors for protocol parsing

## Impairment tests

The next automation step should inject and measure:

- packet loss
- reordering
- burst loss
- receiver late join after session start
- clock offset and jitter
- startup audio starvation and decoder bring-up behavior

## Current proof limitations

- no active `FEC_PARITY` generation or recovery
- no `PTP_DELAY_REQ` / `PTP_DELAY_RESP` round-trip clock discipline yet
- no session catalog or operator control UI
- no dedicated network metrics UI beyond HUD/stdout status lines
- RX still supports a legacy optional local-MP3 fallback path, so the codebase currently contains both network-audio and local-audio bring-up paths
- startup can still show a small number of early Opus decode failures before the steady-state playout queue settles
