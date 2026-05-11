# TX Crash and Player Stoppage RCA (2026-05)

## Scope

Artifacts analyzed:

- `tx-metrics-soak27.jsonl`
- `desktop-tx-20260503-164705-p29572-t393435468.log`
- `desktop-tx-20260503-205437-p24832-t408287890.log`
- `desktop-tx-20260505-075009-p36976-t534018671.log`
- `desktop-rx-20260430-224541-p2244-t155755312.log`

## Executive Summary

TX did not show a clean in-process fatal message in the captured tails. Instead, the logs show prolonged overload conditions consistent with control-plane storm plus scheduler starvation:

- sustained repair-nack flood with frequent full-group masks (`0x01ff`)
- repeated TX force-rebroadcast triggers
- large and rising `thread_deadline_miss` counters (main/status/control)
- large audio send timing spikes (`send(max/avg)` into hundreds and then multi-second maxima in long runs)
- massive CDG starvation accumulation (`cdg_starved_bypass_taken` and very negative `cdg_worst_negative_lead_ms`)

This is a stoppage/stall class failure that can present to operators as "crash with no message" (window hung, watchdog/system kill, or process terminated externally while busy).

## Evidence Highlights

From `desktop-tx-20260505-075009-p36976-t534018671.log` and `desktop-tx-20260503-205437-p24832-t408287890.log`:

- frequent `v4 repair-nack full-group storm ... -> force rebroadcast`
- rapidly increasing `thread_deadline_miss` (`main`, `ctl`, `st`)
- repeated large `audio_timing send(max/avg)` excursions
- degraded peer health (`empty-buf`, `no-latency`, `clock-noisy`) during storms

From `tx-metrics-soak27.jsonl`:

- high `cdg_starved_bypass_taken` growth over time
- worst lead drift reaching extreme negative values (`cdg_worst_negative_lead_ms` in hundreds of seconds)

From `desktop-rx-20260430-224541-p2244-t155755312.log`:

- RX is generally alive and performing recovery loops, supporting the user report that RX is more stable than TX in these runs.

## Root Cause

Primary:

1. TX remained vulnerable to high-rate repair pressure during pathological loss/full-group NACK conditions. Under flood, TX spent excessive work budget on repeated repair handling and rebroadcast behavior.

Contributing:

1. Repair handling performed too much per-event resend work for repeated masks.
2. Force-rebroadcast cooldown was too short for sustained storm conditions.
3. Playlist failure handling did not evict inaccessible tracks, so repeated load failures could force repeated bad transitions and operator-visible stoppage behavior.
4. Empty playlist behavior did not aggressively recover from dynamic library churn (files deleted/offline then re-added).

## Corrective Actions Implemented

Code changes in `platform/desktop/src/app_tx.c` and `platform/desktop/src/app_rx.c`:

1. **Playlist resilience and self-heal**
   - failed track loads now evict the unavailable entry from playlist memory
   - TX automatically skips to the next available track
   - when playlist becomes empty and a scan directory exists, TX periodically reseeds from disk and auto-starts playback when new media appears

2. **Storm hardening on TX repair path**
   - per-event repair resend work is capped (`DASHCDG_TX_V4_NACK_MAX_REPAIRS_PER_EVENT`)
   - full-group rebroadcast cooldown increased (`DASHCDG_TX_V4_NACK_FULL_GROUP_REBROADCAST_COOLDOWN_MS`)
   - full-group NACK events avoid expensive per-member resend bursts

3. **UX upgrades**
   - richer animated pause/wait/connect/reconnect visuals with scrolling regions and moving accents on both TX and RX renderers

## Validation Plan

1. Run soak with deliberate fault injection:
   - delete current track file mid-run
   - unplug/remount media path
   - verify TX evicts missing track, advances, and reseeds when library repopulates
2. Induce packet loss / repair pressure and confirm:
   - reduced `send(max/avg)` spikes
   - slower growth of `thread_deadline_miss`
   - fewer rebroadcast events per minute
3. Confirm operator UX:
   - pause screen animation active
   - connecting vs reconnecting overlays render distinctly

