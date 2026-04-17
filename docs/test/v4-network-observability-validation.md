# Test plan: V4 network observability and sync

## Scope

Validate **v4 rx-stats** reporting, **timing consistency** between TX preview and RX, and (when built) **safe adaptation** policies.

## Preconditions

- Two hosts on the same L2/L3 segment (or routed multicast).
- **v4** session with Opus or another codec; TX and RX built from the same tree.
- Optional: **Clumsy**, **netem**, or Wi‑Fi distance to inject loss/delay.

## Automated / build-time

- `make test` — includes `dashcdg_protocol_serialize_v4_rx_stats` round-trip in `tests/test_core.c`.

## Manual QA checklist

### 1. Stats channel health

- Start TX, then RX with `--rx-stats-ms 2000` (or a short interval, e.g. 500) for testing.
- Confirm TX process does **not** glitch audio when stats are enabled (compare with `--rx-stats-ms 0`).
- On TX, observe `v4_rx_stats_packets_received` increasing (debugger) or add temporary logging — counter increments in `dashcdg_tx_ptp_thread_main`.

### 2. Field sanity

- **audio_buffer_ms** moves when RX ring sizing or load changes (see `dashcdg_rx_network_stream_ring_ms`).
- **jitter_rms_ms** rises under bursty scheduling (CPU load) or synthetic delay variation.
- **clock_offset_estimate_ms** tracks HUD “off” / sender offset over time.

### 3. Clock offset

- On stable LAN, `clock_offset_estimate_ms` should stay within a **few ms** of steady-state (platform-dependent); document per-release if publishing SLOs.

### 4. Adaptation (future)

- When automated FEC/bitrate/playout control lands, re-run: loss injection → bounded-time response → no oscillation (hysteresis).

### 5. Display–audio

- **RX:** CDG must not lead audio after drain-order + ring fixes; spot-check lyrics vs vocal.
- **TX:** With `--tx-preview-delay-ms auto`, preview should sit closer to **remote** experience than with `0`; with explicit **N**, verify seek lag ≈ **N** ms vs encoder timeline.

### 6. Retro / MCU

- Stats optional; `--rx-stats-ms 0` must not touch the stats socket path (no extra sockets).

## Exit criteria (baseline)

- `make test` passes on **Windows amd64** CI/local.
- Manual smoke: TX + RX, stats enabled, no parse errors, TX counter increases.
