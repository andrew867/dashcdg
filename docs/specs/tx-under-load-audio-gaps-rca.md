# RCA: TX audio late-fill and gaps under CPU load

## Symptom

- Under moderate/heavy TX CPU load, receivers report/emit audio gaps and late-fill behavior.
- TX logs show `audio_send_gap` and `audio_send_burst` events climbing.
- More visible with Wi-Fi/USB ethernet adapters under host scheduling pressure.

## Root causes

1. **Insufficient audio runway under load spikes**
   - Audio send catch-up ceiling and queue depth were tuned for normal load, not bursty encode/read stalls.
   - Result: short scheduler misses propagate into prolonged underrun windows at RX.

2. **Control/video work stealing TX timeslices**
   - V4 sender path can spend too much per pass on non-audio packets while audio is near due.
   - Result: audio release windows are missed (`audio_send_gap`) then overcompensated (`audio_send_burst`).

3. **Potential blocking `sendto` stalls**
   - Socket sends can block when host/network buffers are pressured.
   - Result: sender threads pause in kernel send path instead of keeping timing cadence.

## Fixes applied

- Increased TX audio queue capacity and prefill headroom:
  - `DASHCDG_TX_AUDIO_QUEUE_CAPACITY`: `128 -> 192`
  - high-water now keeps extra reserve.
- Increased catch-up burst limit:
  - `DASHCDG_TX_AUDIO_SEND_MAX_CATCHUP_PACKETS`: `32 -> 64`
- Increased audio-priority guard margin:
  - `DASHCDG_TX_AUDIO_DUE_SOON_MARGIN_MS`: `4 -> 12`
- Reduced per-pass video pressure:
  - `DASHCDG_V4_MAX_VIDEO_PER_PASS`: `2 -> 1`
- Increased socket send buffer:
  - `DASHCDG_TX_SOCKET_SNDBUF_BYTES`: `1 MiB -> 4 MiB`
- Added bounded send timeout on TX sockets:
  - `SO_SNDTIMEO = 12 ms` on media + stats/PTP sockets.

## Validation plan

1. Run TX + at least 2 RX for 30-60 min with normal load.
2. Repeat with synthetic TX load (browser/video encode/build in parallel).
3. Track:
   - `[tx] fault: audio_send_gap`
   - `[tx] fault: audio_send_burst`
   - RX-side underrun/zero-buffer recovery counters.
4. Pass criteria:
   - no sustained `audio_send_gap` growth
   - no audible recurring gaps over 10+ min windows
   - stable RX buffer/latency lines.

## Next tuning knobs (if needed)

- Raise queue to 256 frames.
- Raise send timeout to 20 ms for problematic adapters.
- Add adaptive video pacing based on queue depth.
