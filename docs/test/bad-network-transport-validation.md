# Bad-Network Transport Validation

## Purpose

This document defines the validation matrix for the bad-network transport
tranche in `docs/specs/bad-network-transport.md`.

It is intentionally stricter than the current protocol v3 proof matrix in
`docs/test/desktop-impairment-validation.md`.

Long-duration soaks, log bundles, and **quantified** burst-recovery thresholds:
`docs/test/long-impairment-soak-validation.md`. Roadmap index:
`docs/specs/remaining-tranches-roadmap.md`.

## Modes Under Test

- `quality`: retained higher-quality profile
- `resilience`: low-bitrate weak-link profile

## Required Measurements

For every case below, capture:

- time to first loading screen
- time to first visual anchor
- time to first audio
- steady-state average bitrate
- worst one-second burst bitrate
- RX startup state transitions
- RX repair and failure counters
- explicit active audio profile and codec id
- explicit active anchor mode and repair mode
- structured event timestamps for `loading_screen`, `first_anchor`,
  `first_audio`, and `running`

## Matrix

### 1. Healthy baseline

Conditions:

- no induced loss
- no induced reorder
- no throughput clamp

Pass criteria:

- loading screen visible in under `250 ms`
- first visual anchor in under `1000 ms`
- first audio in under `1500 ms`
- no startup wedge

### 2. Throughput pressure

Conditions:

- constrained relay bandwidth representative of weak Wi-Fi
- no added loss beyond clamp side effects

Recommended proof tooling:

- use a constrained relay that can enforce throughput ceilings rather than only
  synthetic drop patterns
- `python scripts/desktop_impairment.py --listen-group ... --emit-group ... --max-bytes-per-second 112500`
  now provides a repeatable local throughput clamp for first-pass proof work

Pass criteria:

- `resilience` profile stays playable
- TX does not let backfill or anchors monopolize the link
- `quality` profile either remains playable or fails observably without a silent
  wedge

### 3. Mixed burst loss plus reorder

Conditions:

- periodic reorder
- short bursts of loss
- repeated over a multi-minute run

Pass criteria:

- RX keeps advancing after repair opportunities are exhausted
- live media does not deadlock on a missing early packet
- logs clearly distinguish repaired, skipped, and unrecoverable startup packets

### 4. Late join mid-track with delayed visual anchor

Conditions:

- RX joins after the session is already running
- inject delay or loss on the first one or two anchor attempts

Pass criteria:

- loading screen appears promptly
- later anchor attempts can still succeed
- RX does not remain blank while live media continues

### 5. Late join mid-track with damaged first audio groups

Conditions:

- RX joins mid-track
- drop or damage some of the first audio groups

Pass criteria:

- audio startup recovers through redundancy, repair, or retry policy
- video-only startup success does not leave audio permanently stuck

### 6. Long soak under mild impairment

Conditions:

- moderate reorder
- occasional isolated loss
- run long enough to expose scheduler unfairness or drift

Pass criteria:

- no unbounded burst spikes
- startup-only traffic does not continue at startup intensity forever
- RX remains in `running` once steady state is reached unless a real recovery
  event occurs

## Reporting

Every validation run should summarize:

- active audio profile
- active video or anchor mode
- repair mode
- observed bitrate averages and peaks
- whether first visual or first audio missed target and why

For the first v4 rollout, the report should also include:

- active v4 `audio_codec_id` (`opus` = 1, narrowband family = 2–7; payload is **NB-IMA** for all narrowband ids today — see `docs/specs/v4-audio-codecs.md`)
- startup redundancy mode
- startup preroll target
- whether recovery used redundancy, parity, retry, or deadline skip

## Exit Criteria

The bad-network tranche is not considered proven until:

- both profiles have documented pass or fail behavior across the matrix
- resilience mode demonstrates materially lower bandwidth than quality mode
- late-join audio and video both have objective startup evidence
- the observed logs are specific enough to diagnose whether failures came from
  bandwidth pressure, repair exhaustion, anchor delay, or audio bootstrap loss
- the structured event logs are stable enough that repeated runs can be compared
  mechanically rather than only by eye
