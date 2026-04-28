# Morning soak brief — TX / RX logs (2026-04-26)

## Document control

| Field | Value |
| --- | --- |
| Purpose | Summarize overnight / morning soak evidence from saved `desktop-tx` and `desktop-rx` sidecar logs, and judge whether recent mitigations likely fixed audio dropouts and odd audible behavior. |
| TX log (original) | `build/dist/dashcdg-windows-sneakernet/windows-x64/desktop-tx-20260426-031736-p24428-t169451859.log` |
| RX log (original) | `build/dist/dashcdg-windows-sneakernet/windows-x64/desktop-rx-20260426-031553-p14112-t169348843.log` |
| Archived copies | `docs/specs/soak-2026-04-26/` (same filenames as originals) |

## Build / time alignment note

- **TX** header reports `dev-master-ge6345eb3` (Apr 26 2026 03:15:40).
- **RX** header reports `dev-master-g6e14c490` (Apr 26 2026 02:25:57).

So this pair is **not guaranteed to be the same binary revision**; treat cross-log correlation as directional (same network session / same soak window) rather than strict A/B of a single commit.

## Quantitative findings

### TX (`desktop-tx-20260426-031736-...`)

Rough counts (grep over full sidecar):

| Signal | Approx count | Notes |
| --- | ---: | --- |
| `[tx] fault: audio_send_gap` | **86** | Still present; not “gone.” |
| `[tx] fault: audio_late_fill` | **425** | Often clustered with gaps / bursts. |
| `[tx] fault:` (all prefixes) | **596** | Includes non-audio faults; use line-level review for RCA. |

Representative **cause attribution** on `audio_send_gap` lines remains dominated by **wake-late**:

- Example: `cause(wl/qe/sb/lw)=1/0/0/0` with healthy queue depth `q=183..184`.

`audio_timing` lines still show **non-trivial mutex wait and wake-late spikes**, for example:

- `lock_wait(max/avg)=35ms/28ms` with `wake_late(max/avg)=27ms/1ms`
- Later: `lock_wait(max/avg)=122ms/38ms` with `wake_late(max/avg)=106ms/1ms`
- Near end of log: `lock_wait(max/avg)=331ms/31ms` with `wake_late(max/avg)=328ms/2ms`, followed by `audio_send_gap` with `max=396ms` and `audio_send_burst` + `audio_late_fill +17`.

**Conclusion for TX audio:** we likely **improved** average contention in some windows (deferred CDG FEC work, removal of cross-thread audio assist that regressed lock wait), but **we have not eliminated** the underlying class of misses. The dominant story in this log is still **scheduler / mutex interaction + wake-late**, not `sendto` blocking (send max often ~0–34 ms while gaps are hundreds of ms).

### RX (`desktop-rx-20260426-031553-...`)

Rough counts:

| Signal | Approx count | Notes |
| --- | ---: | --- |
| `fault: audio_arrival_gap` | **98** | Network / scheduling / TX cadence jitter visible at RX. |
| `fault: host_underrun` | **0** (no matches) | Compared to earlier soak snippets with underrun storms, this run looks **much calmer** on host output underrun. |
| `cdg_` fault strings | **0** (no matches) | CDG-specific `fault: cdg_*` spam not present under this grep; CDG issues may still exist under different fault names or only on ESP32. |

RX also shows normal **repair-nack** activity early in the file (expected under loss / repair testing).

## Interpretation — “did we fix the dropouts?”

**Partially, not fully closed.**

- **Evidence TX is not “fixed” yet:** dozens of `audio_send_gap` / hundreds of `audio_late_fill`, plus extreme `lock_wait` / `wake_late` spikes and a late-log burst (`max=396ms`, `late_fill +17`, `audio_send_burst`).
- **Evidence RX is healthier than the worst prior logs:** absence of `host_underrun` lines in this RX capture is a **positive** signal for listenability vs the earlier “underrun storm” pattern, but `audio_arrival_gap` still shows the wire timeline is not perfectly smooth.

Net: recent mitigations are **directionally helpful** (especially if you heard fewer “hard drops”), but **this log set does not support declaring the TX audio cadence issue resolved**.

## “Heavy track sounded like a slow compressor / limiter”

Plausible non-bug explanations (ranked):

1. **Mastering / dynamics on specific karaoke MP3s** — very common; can sound like pumping even on a perfect transport.
2. **Codec + gain staging** — Opus encode/decode with conservative headroom can interact with hot masters; one or two tracks can stand out.
3. **Transport recovery behavior** — after a gap, **catch-up bursts** (`audio_send_burst`, larger `late_fill` counts) can be perceived as “level riding” even if samples are not clipped.

**Practical check:** replay the same suspect track locally in a player; if the “compressor” character is there offline, it is almost certainly the asset.

## PACK parity line (FYI)

In the TX tail, `cdg-pack-rs` reports a high `failpct` for some windows (example excerpt in log: ~50% fail rate with large checked counts). That is consistent with **many PACK rows being parity-zero / not exercising RS checks** on some discs, not necessarily “bad audio.” Still worth tracking per-track deltas when diagnosing a specific publisher.

## Suggested next steps (engineering)

1. **Unify soak revision** — run TX + RX from the **same commit** and keep both sidecars with matching timestamps.
2. **TX: shorten mutex hold further** — the log still shows `lock_hold(max)=25ms` in some `audio_timing` windows; investigate what holds `g_tx_state.mutex` that long during v4 tick (anchor prep, snapshot send, NACK replay bursts).
3. **TX: audio-first preemption policy** — consider releasing audio due packets before non-audio work in the same locked section, or split state into audio-hot vs cold locks (bigger change).
4. **RX: correlate `audio_arrival_gap` with TX `audio_send_gap` timestamps** — same wall session would confirm end-to-end causality.

## Related specs

- `docs/specs/tx-audio-late-fill-send-gap-rca.md` — RCA / mitigation narrative.
- `docs/specs/tx-audio-late-fill-send-gap-fix-spec.md` — design targets.
