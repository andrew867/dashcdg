# Bad-Network Startup Path

## Purpose

This document specifies the startup and late-join flow for the bad-network
transport tranche.

The main goal is to replace the current blank-or-slow bootstrap experience with
an immediate loading visual, a compact visual anchor, and a more deliberate
audio-start path.

## Startup States

RX should move through these visible states:

1. `discovering`
2. `loading-screen`
3. `anchoring-video`
4. `priming-audio`
5. `running`

These states should be observable in both the HUD and headless logs.

## Loading Screen

The transmitter should own the loading screen.

Requirements:

- self-contained visual state
- tiny enough to repeat aggressively on weak links
- independent of full asset replay
- pleasant operator-facing status instead of a blank render window

Acceptable examples:

- branded "connecting" card
- animated spinner or progress tile
- simple TX-generated CD+G scene

## Compact Video Anchor

The first meaningful picture should come from a compact anchor object rather
than waiting for full asset replay.

The anchor should:

- represent enough state to draw a useful current frame
- align to a specific live timeline point
- carry the next expected live delta sequence or packet index
- be bounded in size and pacing

Potential representations:

- compressed framebuffer plus palette state
- reduced tile-state snapshot
- keyframe-like packet stream that reconstructs only the active visible state

## Live Delta Handoff

After the anchor lands, RX should transition to live deltas immediately.

Rules:

- RX must not wait for full asset replay before using live deltas
- missing deep-history backfill must not block current playout
- later anchors must be allowed to replace a stale or damaged visual state

## Audio Startup

Audio startup must no longer depend on a rigid "future packets arrive cleanly"
assumption.

Required behavior:

- first audio groups receive join-focused redundancy or repair
- RX uses profile-aware preroll targets
- RX can explicitly recover if the first audio groups are damaged or missing
- audio wait state must describe the exact blocker

## Backfill Policy

Deep bootstrap replay becomes opportunistic.

Rules:

- low-priority backfill only uses leftover scheduler budget
- backfill yield is mandatory whenever live audio, live video, or join traffic
  needs the link
- a session can be considered live before deterministic full asset rebuild is
  complete

## Scheduler Priorities

Highest to lowest:

1. first audio groups and immediate repair for startup-critical packets
2. loading screen and compact video anchor
3. live audio
4. live video deltas
5. bounded repair traffic for steady state
6. opportunistic backfill

## Observability

Required counters or states:

- time to first loading screen
- time to first compact anchor
- time to first audio
- startup retries or recovery attempts
- current profile and repair mode
- backfill debt versus live deadline pressure

## Open Questions For Implementation

- whether the compact video anchor should be a new packet family or a profile of
  `CDG_SNAPSHOT`
- whether resilience-mode audio should favor smaller frames or redundant initial
  groups
- whether startup-critical packets need stronger reliability than steady-state
  media
