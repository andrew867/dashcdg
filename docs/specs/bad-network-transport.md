# Bad-Network Transport Tranche

## Purpose

This document defines the next transport tranche that prioritizes weak Wi-Fi,
bursty multicast delivery, and late-join startup over wire compatibility with
the current desktop proof.

This tranche is intentionally separate from the current protocol v3 proof in
`docs/specs/transport-protocol.md`.

Companion documents:

- `docs/specs/bad-network-audio-profiles.md`
- `docs/architecture/bad-network-startup-path.md`
- `docs/test/bad-network-transport-validation.md`

## First-Tranche Lock

The first implementation tranche for this document is no longer codec- or
framing-agnostic. It is locked to:

- protocol `v4` as a clean wire break from the current desktop proof
- simultaneous TX plus RX rollout for bad-network mode
- `quality` audio profile backed by Opus
- `resilience` audio profile backed by an SBC-like framed low-bitrate mode
- a compact visual anchor that is smaller than the current full-state snapshot
  and is paced independently from deep backfill

## Design Goals

- keep live audio plus live CD+G playable on weak Wi-Fi links
- reduce both average bitrate and peak burst rate compared to the current proof
- show a useful screen immediately on late join, not a blank window
- start audio reliably on late join and after transient loss
- retain a higher-quality audio mode while adding a low-bitrate resilience mode
- permit a protocol redesign and version bump where required

## Current Pain Points

- TX continuously replays `ASSET_CHUNK` traffic even during steady-state play
- TX emits large `CDG_SNAPSHOT` bursts on top of live media
- TX can send many media datagrams in one scheduler pass, which is unfriendly to
  bad Wi-Fi and multicast-to-unicast AP behavior
- RX has a strong visual late-join path through `CDG_SNAPSHOT`, but audio late
  join still depends on future `AUDIO_FRAME` packets reaching the current start
  gate
- bounded XOR FEC protects only a single missing payload per group and is weak
  against clustered Wi-Fi loss

## Success Targets

The new transport must define and eventually prove these targets:

- first visible loading screen in under `250 ms` after RX joins a healthy local
  session
- first useful live visual anchor in under `1000 ms` after RX joins
- first audio in under `1500 ms` after RX joins on a healthy weak-Wi-Fi path
- no permanent audio-start wedge when video late join succeeds
- no scheduler tick that can monopolize the link with unbounded bootstrap or
  snapshot traffic
- a resilience profile whose steady-state link budget fits materially below the
  current proof's observed Ethernet usage

## Transport Model

The new transport should move from "always replay everything" toward a staged
model:

1. loading screen state
2. compact video anchor
3. live audio and live CD+G deltas
4. repair or redundancy traffic
5. opportunistic deeper backfill only when bandwidth permits

## Packet-Family Direction

The first v4 desktop rollout should use these packet families:

- `SESSION_INFO`
- `LOADING_SCREEN`
- `VIDEO_ANCHOR`
- `AUDIO_CHUNK`
- `VIDEO_DELTA`
- `REPAIR_WINDOW`
- `BACKFILL_CHUNK`
- `CLOCK_SYNC`

The exact enum values belong in the protocol definition, but the family split is
intentional: loading state, live startup, repair, and deep backfill should no
longer be overloaded into the same bursty behavior as the current proof.

### Session Metadata

Retain a session announcement family, but expand it to advertise:

- transport/profile version
- audio profile id
- video profile id
- bootstrap mode
- loading-screen availability
- repair profile

For the first v4 implementation, session metadata should also expose enough
state for immediate RX startup decisions:

- `transport_version = 4`
- `audio_codec_id` (see [v4-audio-codecs.md](v4-audio-codecs.md) — Opus vs fixed-point narrowband family)
- `audio_frame_ms`
- `audio_sample_rate`
- `audio_channels`
- `audio_bitrate_or_mode`
- `audio_join_redundancy`
- `video_anchor_mode`
- `video_delta_mode`
- `startup_backfill_mode`

### Loading Screen State

Add an explicit lightweight loading-screen packet family or equivalent state
message. It should be:

- TX-generated
- self-contained
- safe to repeat aggressively
- independent of full asset bootstrap

This state exists only to show immediate feedback while the receiver waits for a
live anchor.

First-tranche direction:

- TX publishes a tiny self-contained loading-state payload carrying a screen kind
  (`connecting`, `late-join`, `repairing`) plus a small animation phase counter
- RX may render that packet directly without requiring any external asset or CDG
  bootstrap state

### Compact Video Anchor

Replace or supplement the current full framebuffer snapshot with a more
bad-network-friendly visual anchor:

- smaller than current periodic full snapshots where practical
- emitted with bounded pacing
- aligned to a known live timeline point
- sufficient to let RX render a meaningful first frame before deterministic
  backfill completes

First-tranche direction:

- keep the current semantic source of truth as `dashcdg_cdg_state`
- replace the raw full-state burst with an RLE-oriented compact anchor payload
  derived from the current visible CDG canvas, palette, and offsets
- align each anchor to a specific live packet index so RX can render
  immediately, then continue from later `VIDEO_DELTA` packets

### Live Video Delta Stream

The current `CDG_BATCH` model should be re-evaluated for poor links.

Required investigation:

- compact packetization rules
- optional low-entropy compression for repeated CD+G patterns
- bounded run-length or repeat encoding where repeated packet sequences make the
  wire smaller without making decode too expensive

First-tranche direction:

- preserve packet-index-driven live video semantics
- allow `VIDEO_DELTA` to carry either raw CD+G packet groups or compact repeat
  runs when the same subchannel packet sequence repeats
- keep decode cost low enough that a later MCU RX can apply deltas without a
  heavyweight decompressor

The design must keep decode feasible for MCU-class receivers.

### Audio Profiles

The tranche must define at least two audio profiles:

- `quality`: retained higher-quality mode
- `resilience`: very low bitrate testing or survival mode

Candidate resilience-profile directions:

- low-rate Opus
- mu-law
- SBC-like low-bitrate framing
- other fixed and floating point friendly codecs

First-tranche direction:

- `quality` stays on Opus
- `resilience` is an SBC-like framed mode rather than another Opus variant

The spec must explicitly carry enough profile metadata for a late-joining RX to
initialize decode immediately.

### Repair Layer

The current XOR-only FEC is insufficient by itself for bursty Wi-Fi.

The redesign must evaluate:

- stronger parity strategies
- bounded retransmit or NACK-assisted recovery for startup objects
- deliberate redundancy for first audio groups and first visual anchors
- per-family repair budgets so bootstrap traffic cannot starve live media

First-tranche direction:

- keep bounded parity for steady-state media groups
- add explicit startup redundancy for the first audio groups and first anchor
  attempts
- reserve bounded retransmit or retry behavior for startup-class objects only
  rather than for the entire steady-state stream

## Scheduler Rules

The transmitter scheduler must be explicitly fair.

Required rules:

- no family may emit an unbounded burst in one scheduler pass
- live audio deadlines outrank opportunistic backfill
- live visual anchors outrank deep asset replay
- repair traffic has a bounded share
- startup traffic is paced, not dumped

First-tranche direction:

- one scheduler pass must never emit an unbounded anchor or backfill burst
- live audio deadlines should be serviced first
- at most one anchor-class object and one backfill-class object should be
  emitted per pass unless the scheduler is explicitly catching up from an idle
  gap
- short-window bitrate telemetry is mandatory so fairness failures are visible

## Late-Join Audio Strategy

The new design must not assume that future audio frames plus the current fixed
preroll threshold are enough.

It must define:

- a late-join audio bootstrap path
- a first-audio recovery path when initial packets are missing
- adaptive or profile-aware preroll behavior
- explicit observability for "why audio has not started yet"

First-tranche direction:

- `quality` may use a larger preroll and lighter startup redundancy
- `resilience` should use stronger join redundancy with a lower bitrate codec and
  a smaller target queue
- RX must expose startup states that distinguish `wait-config`,
  `wait-first-audio`, `wait-repair`, and `wait-preroll`

## Compatibility

This tranche is allowed to require a protocol version bump and simultaneous TX
plus RX updates. Backward compatibility with the current proof is not a goal if
it materially weakens the bad-network design.

## Proof Requirements

The implementation is not complete until it is backed by:

- a Wi-Fi-focused impairment matrix
- captured baseline versus resilience-profile link budget
- late-join startup measurements for both audio and video
- burst-loss and reorder validation
- observability that distinguishes startup, repair, and live playout states
