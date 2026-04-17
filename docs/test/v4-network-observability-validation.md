# Test plan: V4 network observability and sync

## Scope

Validate **v4 rx-stats** reporting (default **2000 ms** interval; **0** disables), **timing consistency** between TX preview and RX, and (when built) **safe adaptation** policies. Multi-receiver **aggregation** behaviour is specified in [`../specs/v4-receiver-stats-aggregation-and-adaptation.md`](../specs/v4-receiver-stats-aggregation-and-adaptation.md) and validated here once counters and controller logic exist.

## Preconditions

- Two hosts on the same L2/L3 segment (or routed multicast).
- **v4** session with Opus or another codec; TX and RX built from the same tree.
- Optional: **Clumsy**, **netem**, or Wi‑Fi distance to inject loss/delay.

## Automated / build-time

- `make test` — includes `dashcdg_protocol_serialize_v4_rx_stats` round-trip in `tests/test_core.c`.

## Manual QA checklist

### 1. Stats channel health

- Start TX, then RX (default sends stats every **2000 ms**); use `--rx-stats-ms 500` for faster manual iteration when needed.
- Confirm TX process does **not** glitch audio when stats are enabled (compare with `--rx-stats-ms 0`).
- On TX, observe `v4_rx_stats_packets_received` increasing (debugger) or add temporary logging — counter increments in `dashcdg_tx_ptp_thread_main`.

### 2. Field sanity

- **audio_buffer_ms** moves when RX ring sizing or load changes (see `dashcdg_rx_network_stream_ring_ms`).
- **jitter_rms_ms** rises under bursty scheduling (CPU load) or synthetic delay variation.
- **clock_offset_estimate_ms** tracks HUD “off” / sender offset over time.

### 3. Clock offset

- On stable LAN, `clock_offset_estimate_ms` should stay within a **few ms** of steady-state (platform-dependent); document per-release if publishing SLOs.

### 4. Adaptation and aggregation (future)

- When **v2** FEC/error fields and a controller exist: loss injection → verify **per-receiver** stats, then **median/p95** aggregates move in the documented direction (see aggregation spec).
- Re-run: loss injection → bounded-time response → **no oscillation** (hysteresis and minimum dwell).
- **Multi-RX:** three clients (clean / lossy / bursty) — controller inputs stable; no thrash when one outlier flaps.

### 5. Display–audio

- **RX:** CDG must not lead audio after drain-order + ring fixes; spot-check lyrics vs vocal.
- **TX:** With `--tx-preview-delay-ms auto`, preview should sit closer to **remote** experience than with `0`; with explicit **N**, verify seek lag ≈ **N** ms vs encoder timeline.

### 6. Retro / MCU

- Stats optional; `--rx-stats-ms 0` must not touch the stats socket path (no extra sockets). Default **2000 ms** is acceptable to disable on very constrained builds if product policy changes.

## Exit criteria (baseline)

- `make test` passes on **Windows amd64** CI/local.
- Manual smoke: TX + RX, stats enabled, no parse errors, TX counter increases.
