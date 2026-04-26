# RCA: TX recurring `audio_send_gap` + `audio_late_fill` on Win11

## Document control

| Field | Value |
| --- | --- |
| Symptom | TX logs repeatedly emit `fault: audio_send_gap` and `fault: audio_late_fill` during steady playback. |
| Scope | Desktop TX (`desktop-tx.exe`, Win11 observations), v4 transport, multi-receiver soak. |
| Primary evidence | `build/dist/dashcdg-windows-sneakernet/windows-x64/desktop-tx-20260426-012646-p12976.log` |
| Impact | Audible risk: timeline jitter, silence/late fill concealment, downstream RX instability under sustained host timing jitter. |
| Status | OPEN - RCA complete, fix design pending implementation. |

## Mission statement

Eliminate recurring TX-side audio timing misses without regressing video cadence, v4 FEC behavior, or pause/resume stability.

## Observed evidence (from current TX log)

- Repeated fault signatures during one run:
  - `audio_send_gap +1 max=221ms q=184`
  - `audio_late_fill +9 q=184`
  - subsequent recurring gaps (`max=165ms`, `max=106ms`, `max=110ms`) with late-fill increments.
- Queue depth around fault windows remains high (`q=183/184`), which indicates this is not simply producer starvation to zero.
- Pattern repeats over time instead of appearing only at startup/warmup.

## System context

- TX audio scheduler and media/video sender share `g_tx_state.mutex`.
- Audio sender loop computes next sleep interval and can catch up in bursts.
- Network send path is synchronous (`sendto`) with socket send timeout configured.
- Fault counters in TX distinguish:
  - `audio_queue_starve` (queue empty at due time),
  - `audio_send_gap` (send cadence miss),
  - `audio_silence_fill`,
  - `audio_late_fill`.

## Fault tree (NASA-style)

### Top Event

**TE-1: recurring TX timing misses create `audio_send_gap` + `audio_late_fill` bursts during steady-state playback.**

### Branch A - Scheduler/critical-section jitter

1. Audio due-time handling is delayed while mutex is held by non-audio work.
2. Delayed service causes elapsed gap > expected cadence.
3. Sender attempts catch-up and increments `audio_late_fill`.
4. Repeats under host timer jitter or periodic lock contention.

### Branch B - Blocking/slow kernel send completion

1. `sendto` (or socket path) intermittently blocks near timeout under host/network pressure.
2. Audio sender loop misses one or more release windows.
3. Gap counter increments; subsequent late-fill concealment increases.

### Branch C - Wake/sleep quantization and timer resolution

1. Computed sleep interval plus OS wake latency exceeds due margin.
2. Small periodic overruns accumulate into visible send-gap events.
3. Bursty catch-up behavior appears even when queue depth is healthy.

## Root-cause hypotheses (ranked)

1. **RC-1 (Most likely): mutex contention creates audio service latency spikes.**  
   Evidence: healthy queue depth at fault time argues against pure decode starvation.
2. **RC-2: intermittent send-path blocking contributes to cadence misses.**  
   Evidence: fault pattern aligns with intermittent gap bursts rather than constant drift.
3. **RC-3: sleep/wake jitter is under-accounted in due-time guard logic.**  
   Evidence: repeated small-to-medium gaps (100-220 ms) under otherwise stable run.

## Non-causes / weak candidates

- Not primarily a startup prefill issue (events recur long after startup).
- Not primarily queue-capacity collapse (queue remains near full at several fault points).
- Not obviously caused by FEC/NACK control path alone (fault type is audio cadence, not repair path).

## Risks if unresolved

- Audible glitches persist under Win11 host variability.
- RX compensation/recovery logic carries unnecessary load and can mask true transport quality.
- Peer health metrics become noisier when sender cadence itself is unstable.

## Required instrumentation before code fix (docs-first commitment)

Add/expand observability in TX before behavioral changes:

1. Per-loop timing budget:
   - lock wait duration,
   - lock hold duration in audio sender loop,
   - send syscall duration.
2. Gap causality tags:
   - `cause=lock-wait|send-block|wake-late|queue-empty|mixed`.
3. Percentile summaries every N seconds:
   - p50/p95/p99 loop latency and send latency.

## Exit criteria for RCA closure

RCA is considered closed when:

1. The dominant cause is proven with measurements (not inferred),
2. A fix is implemented and validated against this runbook,
3. Soak logs show no sustained recurring `audio_send_gap`/`audio_late_fill` bursts at baseline load.

