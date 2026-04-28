# Test plan: heap-backed variable-capacity jitter rings (ESP32)

## Objectives

1. Prove functional parity with current receiver behavior.
2. Quantify quality gains from adaptive capacity profiles.
3. Detect leaks, fragmentation cliffs, and long-soak drift.
4. Ensure no regressions in stats publishing, decode toggles, and session transitions.

## Test matrix

| Mode | Audio | Video | Expected profile |
| --- | --- | --- | --- |
| A | on | on | balanced |
| B | on | off | audio-priority |
| C | off | on | video-priority |
| D | off | off | minimal |

Run each mode with:

- clean network
- burst loss / reorder injection (if available)
- long soak

## Functional tests

1. **Cold join**
   - RX starts before TX.
   - Verify clock lock, then stable decode.
2. **Late join**
   - RX joins active stream.
   - Verify anchor apply and forward playout.
3. **Track transitions**
   - next/prev, rapid skip sequence, natural rollover.
4. **Pause/resume**
   - ensure no wedge on resume.
5. **Toggle transitions**
   - on/off each decode path while receiving traffic.
   - verify profile swaps and live alloc counters.
6. **Stats continuity**
   - `v4_rx_stats_sent` continues incrementing under heavy traffic.

## Memory tests

### Leak checks

- Record every 10 s:
  - `heap free`
  - `heap min`
  - internal largest block
  - DMA largest block
- 30 min and 2 h windows.
- Pass criteria:
  - no monotonic unbounded decline after warmup
  - largest block does not collapse below operational thresholds

### Fragmentation checks

- Force repeated profile switches:
  - 200 cycles: A -> C -> B -> D -> A
- During active traffic and idle traffic.
- Pass criteria:
  - no failed reprofiles
  - no progressive increase in alloc failure counters

## Quality/performance tests

1. **Video quality under audio off**
   - Compare fixed-ring baseline vs video-priority profile.
   - Metrics:
     - `jb_evict_rounds`
     - repair recovered/failed
     - visible banding/freeze incidence
2. **Audio continuity under video off**
   - Compare baseline vs audio-priority profile.
   - Metrics:
     - audio skips/underrun indicators
     - subjective continuity score
3. **Balanced mode**
   - Verify no regression from current both-on behavior.

## Stress tests

1. **Datagram surge**
   - sustain high packet rate (target >= prior 2M datagrams soak envelope).
2. **Loss burst**
   - periodic short bursts.
3. **Mixed multi-receiver**
   - desktop + ESP32 side-by-side from same TX.

## Required instrumentation

- Modal lines:
  - profile enum
  - runtime bytes (audio/video/blit/anchor)
  - ring capacity/occupied
  - alloc failure counters
- Logs:
  - profile switch start/end
  - switch latency
  - fallback reason on allocation failure

## Acceptance criteria

Ship-ready only if all hold:

1. No crash/WDT in 2 h soak.
2. No persistent stats blackout.
3. No net memory leak trend.
4. Video-priority mode reduces CDG eviction vs baseline.
5. Audio-priority mode improves continuity vs baseline.
6. Toggle transitions remain deterministic and recoverable.
