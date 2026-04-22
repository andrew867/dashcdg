# V4 priority implementation plan

## Purpose

Turn the remaining v4 backlog into a concrete execution program with strict ordering,
phase gates, and clear “do this before that” boundaries.

This document is the implementation-order companion to:

- [../specs/remaining-tranches-roadmap.md](../specs/remaining-tranches-roadmap.md)
- [v4-group-playout-sync-rollout.md](v4-group-playout-sync-rollout.md)

## Priority order

1. **Tranche 0**: stabilize current v4 behavior and freeze validation gates
2. **Tranche A**: measurement and observability for cross-client sync
3. **Tranche B**: TX-as-controller group playout sync
4. **Tranche C**: impaired-network and rapid-switch hardening
5. **Tranche D**: efficiency, quality, and operator-facing follow-through
6. **Tranche E**: v5 and future platform expansion

## Tranche 0: current v4 stabilization

Goal:

- finish known startup, recovery, cadence, and legacy edge cases before any new sync-control work

### Phase 0.1: startup / recovery closeout

Scope:

- cold join / late join startup video and audio
- natural rollover and forced track-change startup correctness
- XP/P3 crash and recovery paths
- host-underrun / sender-idle / auto-recover behavior
- resilient-profile startup defaults and runway behavior

Exit criteria:

- same-box TX/RX startup is clean
- natural rollover does not wedge audio or video
- XP legacy RX survives repeated track changes and soak runs

### Phase 0.2: validation freeze

Scope:

- define smoke matrix for every desktop build
- define soak matrix for Win11 GL, Win11 GDI, XP legacy
- define pass/fail thresholds for:
  - startup success
  - late join
  - track change
  - underrun recovery
  - overnight soak

Exit criteria:

- a known-good v4 build is tagged as baseline
- every later tranche is required to pass the frozen matrix

## Tranche A: measurement and observability

Goal:

- instrument the system well enough to measure convergence and drift before adding group control

### Phase A.1: baseline convergence capture

Scope:

- capture startup offset, steady-state drift, and rollover behavior on:
  - Win11 WASAPI vs Win11 WASAPI
  - Win11 WASAPI vs XP WinMM/MME
  - Win11 MME vs Win11 WASAPI where useful as a control
- archive 10 min, 30 min, 1 h, and overnight results

Exit criteria:

- baseline spread and drift report exists for each receiver pair

### Phase A.2: receiver observability completion

Scope:

- complete RX stats and HUD fields for:
  - presented audio timestamp
  - target total latency
  - app buffer target
  - app buffer actual
  - host latency
  - drift trim
  - host-underrun recover counts
  - zero-buffer and silent-stall recover counts
  - startup anchor/bootstrap state

Exit criteria:

- operators can distinguish:
  - network starvation
  - host callback starvation
  - buffer-pressure hold
  - startup bootstrap delay

### Phase A.3: TX-as-controller measurement mode

Scope:

- TX ingests receiver timing reports
- TX computes a proposed group target
- TX logs fleet spread and proposed controller output
- RX behavior does not change yet

Exit criteria:

- controller math exists and is observable without risking playout

## Tranche B: group playout sync / IDMS

Goal:

- move from local-target buffering to real shared playout control

### Phase B.1: shared group-target protocol

Scope:

- define and send controller target in sender time
- keep compatibility fallback for receivers that do not follow it

Exit criteria:

- TX publishes a stable group target without destabilizing legacy clients

### Phase B.2: receiver servo against group error

Scope:

- RX servo minimizes presentation error to group target
- startup convergence and steady-state drift control are handled separately
- keep CLI/feature flag fallback to current local-only policy

Exit criteria:

- same-backend clients converge into the agreed tolerance band

### Phase B.3: heterogeneous backend hardening

Scope:

- finalize per-backend latency policy
- clamp trim authority by backend stability
- stabilize WASAPI/MME/XP differences

Exit criteria:

- Win11 WASAPI vs XP legacy stays inside acceptance band through long soak

## Tranche C: transport and pressure hardening

Goal:

- validate v4 behavior under impairment, pressure, and aggressive operator input

### Phase C.1: long impaired-network soak thresholds

Scope:

- define acceptable burst loss, reorder, and recovery bands
- archive multi-hour and overnight impaired-network results
- turn current subjective expectations into release gates

Exit criteria:

- threshold tables exist and are attached to the validation docs

### Phase C.2: rapid track switching under sustained TX pressure

Scope:

- repeated next/back/restart under CPU, disk, and network pressure
- verify no startup wedge, no audio starvation spiral, no black-video dead state

Exit criteria:

- rapid operator interaction is no longer a known destabilizer

### Phase C.3: bad-network transport and audio-profile hardening

Scope:

- startup redundancy tuning
- cadence under local machine pressure
- resilient-profile behavior under actual impairment
- FEC / repair verification against real loss

Exit criteria:

- resilient mode is demonstrably better than quality mode on impaired links

## Tranche D: quality and product follow-through

Goal:

- improve efficiency, perceived quality, and operator usability once core transport and sync are solid

### Phase D.1: TX CD+G slimdown / late-join source-model follow-through

Scope:

- reduce startup/control bandwidth where safe
- preserve deterministic late-join and recovery behavior

Exit criteria:

- lower bandwidth with no regression in late join or startup visuals

### Phase D.2: narrowband perceived-quality work

Scope:

- codec-loopback quality tests
- final speech-codec conditioning and level policy
- subjective acceptance pass for AMR, QCELP, SBC, and related paths

Exit criteria:

- low-bitrate codec behavior is documented and repeatably validated

### Phase D.3: v4 observability / PTP / operator UI

Scope:

- operator-facing sync and health displays
- better fleet status summaries
- optional PTP-aware diagnostics for lab use

Exit criteria:

- field diagnosis no longer depends on reading raw logs only

## Tranche E: future architecture

Goal:

- prepare the path to v5 and embedded/FPGA work without destabilizing v4

### Phase E.1: v5 simulcast / IGMP / ladder

Scope:

- define the next protocol family only after v4 is stable and measurable

Exit criteria:

- v5 planning no longer blocks v4 release work

### Phase E.2: hardware-facing sync abstraction

Scope:

- isolate sync/controller interfaces so SoC/FPGA work can reuse them

Exit criteria:

- desktop and future embedded receivers share the same timing contract

## Immediate next work

The next implementation tranche should be:

1. **Phase 0.1** completion
2. **Phase 0.2** validation freeze
3. **Phase A.1** baseline convergence capture
4. **Phase A.2** RX observability completion
5. **Phase A.3** TX-as-controller measurement mode

Do not start real group-target playout until those items are complete.
