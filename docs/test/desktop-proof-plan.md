# Desktop TX/RX Proof Plan

## Scope

This tranche proves the architecture on computer hardware before MCU work:

- `apps/desktop-tx/main.c`: multicast transmitter for CD+G assets and clock beacons
- `apps/desktop-rx/main.c`: multicast receiver with local MP3 playback and OpenGL rendering
- `apps/desktop-player/main.c`: local non-network player for baseline regression checks

## Success criteria

- receiver can discover a session from `ANNOUNCE`
- receiver reconstructs the full CD+G asset from repeated `ASSET_CHUNK` packets
- receiver renders the rebuilt asset with deterministic seeking
- receiver starts local audio close to the advertised session boundary
- receiver follows network beacons before local audio is available

## Observability requirements

- packet sequence numbers for capture/replay
- sender monotonic timestamps for drift analysis
- explicit asset size and chunk size in announces
- deterministic test vectors for protocol parsing

## Impairment tests

The next automation step should inject:

- packet loss
- reordering
- burst loss
- receiver late join after session start
- clock offset and jitter

## Current proof limitations

- receiver multicast address is fixed in code
- no retransmission or FEC
- no session catalog or operator control UI
- no network metrics UI yet
