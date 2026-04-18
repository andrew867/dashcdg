# Rapid track switch under sustained TX pressure

## Purpose

Validate **next/previous track** (or equivalent control) while the transmitter remains under **encode + network send pressure**, ensuring no **deadlock**, **silent audio**, **stuck CDG**, or **unbounded queue growth**. Complements soak tests that hold one track.

## Preconditions

- MP3+G playlist with **≥ 3** tracks (or repeat same ZIP paths) so switches are meaningful.
- v4 transport enabled if testing bad-network profile semantics.
- Record: commit hash, TX/RX variants, OS.

## Pressure model

Choose at least **one** sustained-load condition:

| Mode | How |
| --- | --- |
| **CPU pressure** | Background CPU burn on TX host (external tool), leaving ≥1 core for realtime — document % |
| **Network pressure** | Impairment relay with `--max-bytes-per-second` and/or `--drop-every` (mild) |
| **Scheduler pressure** | Rapid switches alone may suffice if TX encode queue stays deep |

Document which mode(s) apply per run.

## Procedure

1. Start TX with playlist; start RX; wait until **running** (audio + render gates healthy).
2. **Warm-up** 60 s single track.
3. **Phase A — rapid switches**: invoke track change at **≤ 3 s** spacing for **≥ 30** switches (different directions optional). Prefer automated key injection if available; otherwise manual with timestamped log.
4. **Phase B — sustained hold**: after Phase A, play **one** track **≥ 15 min** under same pressure mode (catch slow leaks).
5. Optional **Phase C — codec flip**: if testing narrowband hot-swap (`c`), perform **≥ 10** codec cycles mid-stream per **[v4-codec-switching-contract.md](../specs/v4-codec-switching-contract.md)**.

## Observability (must log or HUD)

**TX**

- Playback position, queue depth indicators, encode starvation (`a=-1` patterns), FEC lines.
- Any reload of CDG batches / anchor generation after switch.

**RX**

- `gate=` line through preroll after each switch.
- `repair` / `fail` counters — should not diverge without bound across switches.
- CDG render gate: must recover **live** path after snapshot/bootstrap per prior behavior.

## Pass criteria

1. **No permanent wedge**: after each switch, audio reaches **running** within **2×** normal cold-start time for that profile (measure once in baseline).
2. **No duplicate exclusivity violation**: Opus/decoder state matches **[v4-codec-switching-contract.md](../specs/v4-codec-switching-contract.md)** if codec changed.
3. **TX** does not accumulate **unbounded** send failures without recovery.
4. **RX** `fail=` growth rate after Phase B ≤ **2×** same-duration baseline without rapid switches (same impairment).

## Failure triage

| Symptom | Likely area |
| --- | --- |
| Audio silent until next manual pause | RX start gate / clock vs session_start |
| CDG blank until seek | Snapshot / live packet ordering |
| TX `send_failures++` storm | UDP path or MTU |

## Related specs

- **[tx-cdg-source-model.md](../specs/tx-cdg-source-model.md)** — schedule + asset replay expectations.
- **[remaining-tranches-roadmap.md](../specs/remaining-tranches-roadmap.md)** — index.
