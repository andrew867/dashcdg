# V4 group playout synchronization and IDMS program

## Purpose

Define the next stability/reliability program for **v4 multi-receiver synchronized playout** so heterogeneous receivers such as:

- Windows 11 + WASAPI / PortAudio
- Windows XP + WinMM / legacy PortAudio host APIs
- future embedded or ESP32-aligned receivers

can converge on the **same audible playout instant** and maintain that alignment over long sessions.

This document is the normative design for the next sync tranche. It converts current v4 timing behavior into an explicit **IDMS-style** group playout system.

## Problem statement

Current v4 is materially better than earlier builds, but still shows the classic gap between:

- **local correctness**: each receiver can often keep its own video aligned to what it hears
- **cross-receiver parity**: two receivers with different host APIs and device latencies can remain audibly separated by tens to hundreds of milliseconds

Observed field symptoms:

- WinXP / MME or legacy PortAudio falls behind Win11 / WASAPI by about one drum hit or half an arena echo.
- Receivers may converge slowly or not at all after startup or CPU pressure.
- Software queue depth may look healthy while actual device presentation is late or stalled.
- Current queue servo acts per receiver, not against a common group presentation target.

## Standards basis

The next design follows the split found in RTP/RTCP and IDMS work:

- **RTP timing base**: timestamps, sequence numbers, sender/receiver timing reports, and delivery monitoring. RTP provides timing structure but does not itself guarantee synchronized playout or QoS.
  Source: RFC 3550, https://www.rfc-editor.org/rfc/rfc3550
- **Rapid sender-time mapping**: early sender-to-wallclock mapping reduces late-join sync uncertainty.
  Source: RFC 6051, https://www.rfc-editor.org/rfc/rfc6051
- **Inter-destination media sync (IDMS)**: receivers report arrival/presentation timing, a controller computes a common target playout point, and receivers synchronize to that target.
  Source: RFC 7272, https://datatracker.ietf.org/doc/html/rfc7272

`dashcdg` is not required to clone the full RTP/RTCP wire format. It must implement the same control-plane idea:

- measure real presentation
- distribute a common group target
- servo to the target with bounded rate correction

## Non-goals

- Sample-accurate genlock across all consumer Windows backends.
- Replacing the existing v4 media clock with a completely different transport.
- Mandatory backend unification across all hosts.
- Solving impaired Ethernet, codec repair, or CDG authoring issues inside this tranche except where they affect playout sync measurement.

## Success criteria

### Product goals

- Two desktop receivers on the same LAN must converge to a shared playout target without manual intervention.
- The system must survive track changes, late joins, codec switches, pause/unpause, and transient host CPU pressure.
- Cross-receiver audible offset must be measurably smaller than the current XP-vs-Win11 gap.

### Engineering targets

- Same-backend modern hosts: steady-state error <= 10 ms p95, <= 20 ms p99
- Heterogeneous Windows hosts including XP/WinMM class devices: steady-state error <= 20 ms p95, <= 40 ms p99
- Startup convergence after join or resume: <= 5 s to enter steady-state window
- No unbounded drift during a 1 h soak

## Current v4 timing model

Today v4 has:

- TX encoder-primary media timeline
- sender clock / PTP-style exchange
- announced playout delay
- receiver-local app ring target and queue servo
- DAC-primary or sender-primary graphics modes

This is necessary but not sufficient for tight inter-destination sync because:

- the receiver target is still derived mostly from local host latency and queue goals
- the system does not distribute a single group playout target computed from actual receiver presentation
- receiver feedback does not yet drive group sync policy

## Required architecture

### 1. Three timing layers

The implementation MUST keep these distinct:

1. **Media timeline**
   - the canonical sender media time carried by audio chunk `playback_ms` and v4 `clock_sync`
2. **Receiver presentation timeline**
   - the timestamp of what the receiver has actually presented or is about to present at the device
3. **Group target timeline**
   - the common playout point that all receivers are instructed to meet

No logic may substitute one for another implicitly.

### 2. Presentation-based sync control

The controller MUST use **measured presentation state**, not only queue depth.

Each receiver MUST be able to report:

- most recent audio presentation timestamp on the sender/media timeline
- current queue depth in ms
- estimated host output latency in ms
- current drift trim in ppm
- whether the audio path is healthy, stalled, recovering, or backpressure-held

### 3. Group target computation

The controller MUST compute a common target playout point for all receivers in a sync group.

The target SHOULD be based on:

- sender/media timeline
- the slowest or otherwise selected reference receiver
- configured floor/ceiling for total playout delay
- hysteresis to avoid oscillation

The controller MUST NOT chase the fastest receiver if doing so would cause underruns for slower hosts.

### 4. Receiver servo behavior

Receivers MUST servo toward the shared group target with:

- startup phase align
- steady-state fine drift correction
- bounded trim
- explicit holdoff during queue pressure or recovery

The servo MUST minimize **presentation error to the group target**, not merely local queue error.

## Protocol additions

### Phase 1: measurement only

No new control packet required yet. Extend RX stats payload or its logical producer so the following are populated accurately:

- presented audio timestamp
- host output latency estimate
- queue target / queue actual
- drift trim ppm
- silent-stall count
- auto-recover count

### Phase 2: group target packet

Add a new v4 control payload or extend an existing control message with:

- sync group id
- target presentation media timestamp, or absolute target sender time
- generation / sequence number
- controller-selected total playout delay
- optional convergence mode flags

The packet MUST be idempotent and safe to repeat.

### Phase 3: optional receiver recommendation fields

The controller MAY carry:

- tighten / loosen convergence mode
- recovery freeze window
- backend advisory policy

## Receiver algorithm requirements

### Startup and late join

On late join or resume:

- receiver MUST preroll enough to hit the shared group target
- receiver MUST not begin steady-state drift trimming until:
  - clock is valid
  - startup buffer is within convergence band
  - host output timestamp has begun advancing

### Steady state

Receiver MUST compute:

- `presentation_error_ms = local_presented_media_time - group_target_media_time`

Servo policy MUST:

- use small ppm corrections only
- include deadband and hysteresis
- clamp max correction
- avoid acting on stale host timestamps

### Recovery

If either of these is true:

- queue is empty while network media still arrives
- queue is non-empty but presentation timestamp stops advancing

then receiver MUST enter recovery and MUST temporarily suppress normal convergence trimming until the pipeline is healthy again.

## Backend policy

### Default policy

For convergence work, the default backend strategy MUST prefer:

- Win11/modern Windows: WASAPI shared by default
- XP/legacy: WinMM / legacy-supported path

### Win11 MME

Win11 can be tested with MME through PortAudio as a diagnostic or compatibility mode, but it MUST NOT be the default enterprise sync target. It is acceptable as:

- a comparison backend for parity testing
- a fallback if WASAPI is broken on a specific machine

It is not acceptable as the baseline path for convergence claims because its latency model is coarser and less stable than WASAPI.

### Kernel Streaming

Kernel Streaming MAY be evaluated as an advanced backend, but it is explicitly out of scope for the first IDMS tranche unless:

- it materially improves presentation timestamp stability
- it is supportable across the intended Windows fleet
- failure handling is well understood

The first implementation SHOULD fix synchronization using current supported backends before adding KS complexity.

## Phases and tranches

### Tranche A: instrumentation and truth

Goal:

- make presentation timing observable and trustworthy

Deliverables:

- RX stats populate presentation timestamp, host latency, queue target, queue actual, drift ppm
- HUD / logs show controller target vs local presentation error
- silent-stall and recovery counters are exported

Exit criteria:

- operators can explain each receiver’s phase error from logs alone

### Tranche B: group target control

Goal:

- move from local queue target to common group target

Deliverables:

- controller computes target
- receivers follow target
- startup convergence and steady-state drift are separate modes

Exit criteria:

- Win11 vs Win11 and Win11 vs XP both converge better than current baseline

### Tranche C: backend and host-policy hardening

Goal:

- reduce backend-specific timing variance and false recovery

Deliverables:

- backend policy doc finalized
- MME-on-Win11 diagnostic mode, if retained, documented as non-default
- host-timestamp stall detection and recovery proven

Exit criteria:

- no recurrent buffered-silent or zero-buffer wedges in soak matrix

### Tranche D: adaptive group control

Goal:

- use receiver reports to dynamically choose group playout delay and sync aggressiveness

Deliverables:

- controller hysteresis and bounds
- optional slow/fast convergence modes
- documented policy for dropping/tightening laggards

Exit criteria:

- multi-receiver convergence remains stable under moderate host pressure and mild network impairment

## Invariants

- TX media timeline remains encoder-primary.
- Audio chunk timestamps remain canonical media time.
- Graphics mode selection remains separate from audio group sync control.
- Recovery state must not wipe live video state for an audio-only stall.
- Queue depth alone must never be treated as proof of audible presentation.

## Risks

- XP/WinMM timestamp quality may limit best-case convergence versus WASAPI.
- Over-aggressive trim can create audible distortion or modulation.
- Controller target based on bad receiver reports can destabilize the whole group.
- Backend-specific host timestamp semantics may differ enough to require per-backend filtering.

## Open implementation questions

- Should TX always be the sync controller, or should one receiver be electable as sync leader?
- Should group target be carried as sender/media timestamp or sender-wallclock timestamp plus media offset?
- Should laggard receivers be allowed to intentionally sit behind the main group if they cannot meet the target without underrun?

## Related documents

- [av-sync-network-clients.md](av-sync-network-clients.md)
- [v4-display-audio-sync.md](v4-display-audio-sync.md)
- [v4-receiver-stats-aggregation-and-adaptation.md](v4-receiver-stats-aggregation-and-adaptation.md)
- [../test/av-sync-cross-client-validation.md](../test/av-sync-cross-client-validation.md)
- [../test/v4-group-playout-sync-validation.md](../test/v4-group-playout-sync-validation.md)
- [../ops/v4-group-playout-sync-rollout.md](../ops/v4-group-playout-sync-rollout.md)
- [`enterprise-group-sync-spec.md`](enterprise-group-sync-spec.md) — **enterprise** residual-phase requirements (detrended spread, smoothed controller, DAC trim, gates) and implementation pointers
