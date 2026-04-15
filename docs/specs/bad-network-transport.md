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

### Session Metadata

Retain a session announcement family, but expand it to advertise:

- transport/profile version
- audio profile id
- video profile id
- bootstrap mode
- loading-screen availability
- repair profile

### Loading Screen State

Add an explicit lightweight loading-screen packet family or equivalent state
message. It should be:

- TX-generated
- self-contained
- safe to repeat aggressively
- independent of full asset bootstrap

This state exists only to show immediate feedback while the receiver waits for a
live anchor.

### Compact Video Anchor

Replace or supplement the current full framebuffer snapshot with a more
bad-network-friendly visual anchor:

- smaller than current periodic full snapshots where practical
- emitted with bounded pacing
- aligned to a known live timeline point
- sufficient to let RX render a meaningful first frame before deterministic
  backfill completes

### Live Video Delta Stream

The current `CDG_BATCH` model should be re-evaluated for poor links.

Required investigation:

- compact packetization rules
- optional low-entropy compression for repeated CD+G patterns
- bounded run-length or repeat encoding where repeated packet sequences make the
  wire smaller without making decode too expensive

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

The spec must explicitly carry enough profile metadata for a late-joining RX to
initialize decode immediately.

### Repair Layer

The current XOR-only FEC is insufficient by itself for bursty Wi-Fi.

The redesign must evaluate:

- stronger parity strategies
- bounded retransmit or NACK-assisted recovery for startup objects
- deliberate redundancy for first audio groups and first visual anchors
- per-family repair budgets so bootstrap traffic cannot starve live media

## Scheduler Rules

The transmitter scheduler must be explicitly fair.

Required rules:

- no family may emit an unbounded burst in one scheduler pass
- live audio deadlines outrank opportunistic backfill
- live visual anchors outrank deep asset replay
- repair traffic has a bounded share
- startup traffic is paced, not dumped

## Late-Join Audio Strategy

The new design must not assume that future audio frames plus the current fixed
preroll threshold are enough.

It must define:

- a late-join audio bootstrap path
- a first-audio recovery path when initial packets are missing
- adaptive or profile-aware preroll behavior
- explicit observability for "why audio has not started yet"

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
