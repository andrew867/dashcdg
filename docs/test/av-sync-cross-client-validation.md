# Cross-client A/V sync validation

Manual test; no mocks. Goal: confirm **two receivers** stay on the **same lyric/raster phase** relative to each other and to **encoded `playback_ms`**, over at least **one full track** (~3–5+ minutes).

## Prerequisites

- One **TX** host with real **MP3+G** assets (ZIP with aligned pair, or pipeline’s expected layout).
- Two **RX** hosts on the same LAN: **heterogeneous stack** preferred (for example Win11 + default audio path, and WinXP legacy or second OS).
- Build with logging/HUD enabled so **playback_ms** or sender-derived time is visible on each RX if available.

## Procedure

1. Start TX first, then RX A, then RX B (late join is acceptable).
2. Play a track with **clear vocal onsets** (easy to see line transitions).
3. Note **t = 0** when both RX show stable video (past loading/connecting).
4. At **30 s**, **60 s**, **120 s**, **end of track**: compare **on-screen lyric line** (or identical graphic phase) on A vs B — must match.
5. Optional: on one RX, compare **HUD/network playback** (if displayed) to **decoded frame tag** in debug logs for a sample audio chunk — should stay in the same **~20 ms frame grid**, not drift apart over time.

## Pass criteria

- No **systematic** disagreement between clients (one word/line ahead of the other) at any checkpoint.
- No growing offset between **clock_sync-derived display time** and **heard** content that indicates a separate bug (single-client “DAC feels late” within one preroll is acceptable; **between** clients they must agree).

## Failure triage

- **A and B disagree:** Re-check RX is not overriding sender time with DAC for display; re-check TX `clock_sync.playback_ms` vs audio chunk tags.
- **Both agree but lyrics wrong vs recording:** Likely **ZIP/CDG authoring** (silence, offset); fix asset or mux, not protocol.

## References

- `docs/specs/av-sync-network-clients.md`
