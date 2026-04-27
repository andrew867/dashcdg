# TX audio isolation (desktop) — 2026

## Purpose

Document how the **Windows desktop TX** send path keeps **v4 audio release** and
**wire pacing** from blocking on PTP, RX stats aggregation, and other **non-audio
control plane** work, while still sharing one coherent session view.

**Primary implementation:** `platform/desktop/src/app_tx.c`  
**Complementary (DAC clock on RX for lip-sync):** `platform/desktop/src/desktop_audio.c`

## Audio domain (`g_tx_ad`)

A dedicated struct **`dashcdg_tx_audio_domain`** and mutex **`g_tx_ad`** hold:

- The **audio ready queue** and pending / FEC / silence state used by the
  **audio send** thread
- A **mirrored** subset of session fields (pause, timing, pipeline generation,
  codec id, etc.) updated from the main TX path via
  **`dashcdg_tx_ad_sync_from_main_locked()`** while **`g_tx_state.mutex`** is held

The **audio send thread** locks **`g_tx_ad.mutex`**, not the giant TX state
mutex, so a slow PTP or stats path on another thread does not **directly** stall
`sendto` for media audio.

## v4 RX stats: ingest then batch apply

`V4_RX_STATS` from the PTP / stats port is **not** applied under
`g_tx_state.mutex` from the PTP thread. Packets are appended under
**`g_tx_v4_rx_ingest_mutex`**; the main TX loop **flushes** the batch and calls
**`dashcdg_tx_apply_one_v4_rx_stats_locked()`** so anchor adaptation and
reporter tables stay on the same thread that already owns the main lock.

**Overflow** (batch full) increments **`g_tx_v4_rx_ingest_drops`**; that counter
is surfaced in periodic **thread health** log lines (see “Observability” below).

## v4 rate window

v4 per-second byte/peak **window accounting** for HUD / adaptation uses
**`g_tx_v4_rate_mutex`** and a fuse step into **`g_tx_state`** on the main path
so hot sends do not take the full state lock for statistics alone.

## Wire sequence

All packet **header sequence** values on the wire go through
**`dashcdg_tx_next_wire_sequence()`** (atomic counter). Session start resets with
**`dashcdg_tx_reset_wire_sequence_to_one()`** so the audio thread and main thread
do not hand out **duplicate** sequence numbers.

**Send path:** **`dashcdg_tx_send_serialized_packet_held(pthread_mutex_t *rel, …)`**
releases **`rel`**, performs **`sendto`**, reacquires **`rel`**, and updates
counters. v4 media audio uses **`rel = &g_tx_ad.mutex`**.

## Threading and observability

- **Thread priorities (Win32):** audio send is boosted; PTP and control are
  intentionally below normal.
- **Deadline / budget** counters: main tick, audio send, PTP receive budget,
  control console loop, status loop, playlist directory scan, audio producer
  loop — increments when a loop body exceeds its budget (implementation-defined
  thresholds in `app_tx.c`).

A periodic line (same general cadence as the audio **timing** fault summary)
includes all **`thread_deadline_miss_*`** counters plus **`v4_rx_stats_drop`**, so
soaks can show *which* subsystems are slipping under load.

## When this doc is wrong

If `app_tx.c` refactors again, this file is **descriptive** — trust the code and
`git log` for the last contract change.
