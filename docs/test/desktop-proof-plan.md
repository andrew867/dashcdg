# Desktop TX/RX Proof Plan

## Scope

This tranche proves the architecture on computer hardware before MCU work:

- `apps/desktop-tx/main.c`: multicast/broadcast transmitter for bootstrap assets, live Opus frames, timed CD+G batches, snapshots, FEC parity, pause-screen state, and software-timestamped PTP-style sync traffic
- `apps/desktop-rx/main.c`: multicast/broadcast receiver with live network audio decode, bootstrap asset reconstruction, snapshot apply, bounded jitter/FEC handling, and OpenGL rendering
- `apps/desktop-player/main.c`: local non-network player for baseline regression checks

## Success criteria

- receiver can discover a session from `ANNOUNCE`
- receiver reconstructs the full CD+G asset from repeated `ASSET_CHUNK` packets
- receiver renders the rebuilt asset with deterministic seeking after late join
- receiver can also advance a live CD+G state from `CDG_BATCH`
- receiver can apply `CDG_SNAPSHOT` to start or recover live video before asset rebuild finishes
- receiver starts network Opus audio near the announced playout boundary
- receiver follows announce plus PTP clock traffic before the bootstrap asset is fully complete
- receiver tolerates bounded reordering on live audio and CD+G before declaring packets late
- receiver can attempt single-loss XOR repair within protected FEC groups
- pause/resume keeps the network session healthy and displays a TX-generated pause screen
- headless RX and TX status output make stalls and packet flow visible

## Observability requirements

- packet sequence numbers for capture/replay
- sender monotonic timestamps for drift analysis
- explicit asset size and chunk size in announces
- explicit live media counters for audio, timed CD+G, and sync traffic
- RX audio queue depth and decode-failure visibility
- RX jitter queue, skip, drop, and reorder visibility
- RX repair counters for recovered vs failed FEC attempts
- RX snapshot receive/apply visibility
- explicit startup-gate and clock-quality visibility, including holdover and step peaks
- deterministic test vectors for protocol parsing

## Impairment tests

The current automation step is the multicast relay in `scripts/desktop_impairment.py`, which injects and measures:

- packet loss
- reordering
- burst loss
- receiver late join after session start
- clock offset and jitter
- startup audio starvation and decoder bring-up behavior
- pause-screen continuity and resume behavior
- long-play queue pressure and subsystem heartbeat continuity
- track-switch-under-load recovery without multi-second TX pre-encode stalls

## Planned Refactor Validation

The threaded incremental-runtime refactor adds these required proof cases:

- long-play soak where packet ingress, audio progression, video progression, and render heartbeat all continue independently
- repeated track switches while TX is actively streaming, proving that a new session does not require whole-track audio pre-encode before network progress resumes
- impairment runs where RX continues ingesting traffic even while audio/video tasks are recovering from reorder, repair, or deadline skips
- renderer stress runs proving the render loop can miss frames without stalling network or media tasks
- pause/resume and late join runs while bootstrap replay is still incomplete

## Current Status

Implemented:

- live Opus on the wire
- live timed CD+G batches on the wire
- network-audio-only RX path for desktop receive mode
- bounded PTP round-trip timing with stale-exchange rejection
- bounded XOR FEC generation and single-loss recovery
- periodic CDG snapshots for fast late join and recovery anchors
- TX pause/resume with a generated pause screen
- default multicast endpoint plus explicit broadcast endpoint support
- HUD/headless observability for gates, sync, reorder, repair, and snapshot state
- threaded TX/RX runtime split with explicit queues for TX audio production and RX media progression/render publication
- measured isolated multicast soak where RX stayed in `running`, `live_applied` kept climbing, and audio buffering remained stable instead of regressing to a false long-play stall
- measured forced session restart to a different track, proving RX can drain an ended session and cleanly acquire a new announce/bootstrap/live cycle
- a separate bad-network redesign tranche has been specified in `docs/specs/bad-network-transport.md` so weak-Wi-Fi transport changes can advance without rewriting this proof document in place
- a separate portability/slimdown tranche has been specified in `docs/specs/tx-cdg-source-model.md` and companion portability docs so TX memory and platform-support planning can advance without pretending the refactor is already complete

The current proof document should now be read alongside two separate forward
tranches:

- `docs/specs/tx-cdg-source-model.md` for TX storage/runtime slimdown inside the
  existing proof behavior
- `docs/specs/bad-network-transport.md` plus
  `docs/specs/bad-network-audio-profiles.md` for the explicit protocol-v4
  weak-link redesign

Still to prove more deeply:

- repeated long impaired-network soak runs with captured logs
- quantified recovery thresholds under real burst loss
- operational limits for rapid repeated track switches under sustained live send pressure
- the full bad-network transport redesign, including new late-join audio startup behavior and lower-burst bootstrap rules
- the staged TX CD+G slimdown, especially removal of duplicated `CDG_BATCH` payload storage and proof that later source abstractions preserve late-join behavior

## Current Proof Limitations

- no hardware-timestamp or sub-millisecond PTP discipline yet
- no session catalog or richer operator control UI beyond the current keyboard controls
- no dedicated network metrics UI beyond HUD/stdout status lines
- long-duration impaired-network soak data for actual repair thresholds is still incomplete; use `docs/test/desktop-impairment-validation.md` for the current repeatable matrix
- startup can still show a small number of early Opus decode failures or deadline skips before the steady-state playout queue settles
- TX still preloads the `.cdg` asset and currently duplicates timed CD+G payload storage in prebuilt batches, so the portability/slimdown runtime is not yet complete
- the current proof is not the bad-network-optimized transport; that redesign is now tracked separately and is expected to introduce different packet, pacing, and startup rules
