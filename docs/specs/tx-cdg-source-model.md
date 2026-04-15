# TX CDG Source And Memory Model

## Purpose

This document confirms the current desktop TX live-wire behavior and defines the
staged TX CD+G memory slimdown target without changing the existing transport
contract in place.

It exists because two facts are simultaneously true today:

- TX already streams live `AUDIO_FRAME` and live `CDG_BATCH` packets in parallel
- TX still pays too much memory for CD+G because it keeps both the raw asset and
  a second copied batch-oriented representation

The goal of this tranche is to make the current state explicit, then define the
refactor stages that reduce TX memory without regressing late join, snapshots,
pause/restart, or deterministic recovery.

## Confirmed Current Behavior

The current desktop TX does all of the following at the same time:

- loads the `.cdg` file into `g_tx_state.asset_bytes`
- repeatedly replays that asset as `ASSET_CHUNK`
- sends live `CDG_BATCH` packets on the playback timeline
- sends live `AUDIO_FRAME` packets on the same playback timeline when an `.mp3`
  exists
- emits `CDG_SNAPSHOT` anchors in parallel with both live media and asset replay

That means the wire is already a live mixed-media transport, not a
"send-whole-CDG-then-start-audio" design.

## Current TX Memory Model

Today the desktop TX keeps multiple CD+G representations resident for one track:

1. `asset_bytes`
   - full `.cdg` file contents
   - used for `ASSET_CHUNK` replay
2. `cdg_batches`
   - every timed `CDG_BATCH` prebuilt up front
   - duplicates the CD+G packet bytes into batch storage
3. preview/runtime helpers
   - `dashcdg_cdg_reader` and keyframe data when TX preview is enabled
   - `chunk_seen` coverage bitmap
   - one serialized `cdg_snapshot_state` buffer for live snapshot emission

The important inefficiency is `cdg_batches`: it exists for schedule convenience,
but it copies the same packet bytes that already exist in `asset_bytes`.

## Current Guarantees To Preserve

Any slimdown stage must preserve these externally visible behaviors:

- `AUDIO_FRAME` and `CDG_BATCH` continue advancing from the same playout
  timeline
- `ASSET_CHUNK` replay still lets RX rebuild the full deterministic asset
- `CDG_SNAPSHOT` still anchors late join and mid-session visual recovery
- late join must still work when RX arrives after the session has already begun
- forced rebroadcast, pause/resume, and track restart must still be supported

## Target Runtime Modes

This work splits the project into three explicit states instead of treating them
as the same milestone:

### 1. Current desktop proof

Current behavior remains valid proof of:

- live parallel audio plus CD+G transport
- late-join asset replay
- snapshot-based visual re-anchoring

But it is not yet the slimmed TX memory model.

### 2. Target modern desktop runtime

The modern target keeps the same user-visible transport semantics while
removing unnecessary TX-side CD+G duplication.

### 3. Future streaming-bootstrap target

Only after the random-access source layer is stable should the project consider
reducing or removing full preloaded asset dependence from the default steady
state.

## Refactor Stages

### Stage A: remove duplicated prebuilt CDG batches

Replace the current "one struct per batch with copied packet bytes" model with a
schedule representation that stores metadata only:

- `packet_start_index`
- `packet_count`
- `playback_ms`
- `media_sequence`
- FEC grouping metadata

When TX sends a `CDG_BATCH`, it should read the packet bytes directly from the
canonical CD+G source instead of from a second copied `packet_bytes` array.

Expected outcome:

- no second full copy of CD+G packet payloads
- existing `ASSET_CHUNK`, snapshot, and live batch semantics stay intact

### Stage B: introduce a random-access CDG source layer

Abstract the CD+G source behind one interface that can serve:

- `ASSET_CHUNK` replay by byte offset
- `CDG_BATCH` packet reads by packet index
- snapshot/keyframe generation reads
- optional TX preview seeks

Required source capabilities:

- random-access read by byte offset
- random-access read by packet index
- stable track length / packet-count metadata
- deterministic behavior across repeated reads

Allowed implementations:

- in-memory source
- file-backed source
- small sliding window backed by file reads

Expected outcome:

- `asset_bytes` stops being the only legal backing store
- TX can keep deterministic replay semantics without demanding one permanent
  full-file blob

### Stage C: evaluate true streaming bootstrap

Only after Stage B is stable should TX consider a more aggressive bootstrap path
where:

- steady-state live `CDG_BATCH` send does not depend on one full preloaded asset
- `ASSET_CHUNK` replay may be sourced from file or a controlled bootstrap window
- snapshots become the preferred fast-start video anchor while asset rebuild
  continues in the background

This stage must not ship by assumption alone; it changes late-join and recovery
behavior and therefore needs separate proof.

## Compatibility Rules

This slimdown tranche should preserve protocol v3 packet formats unless a later
transport redesign explicitly decides otherwise.

In particular:

- `ASSET_CHUNK` remains byte-addressed
- `CDG_BATCH` remains packet-index-addressed and timeline-driven
- `CDG_SNAPSHOT` remains a bounded framebuffer/palette state anchor

The change is primarily about TX storage and scheduling internals, not about
inventing a new packet family immediately.

## Validation Requirements

The slimdown work is only complete when all of the following are shown:

- live `AUDIO_FRAME` and `CDG_BATCH` still advance together during steady playout
- TX no longer scales memory with both `asset_bytes` and copied `cdg_batches`
  payload storage after Stage A
- late join still reaches first picture and then deterministic ready state
- pause/restart/forced rebroadcast still work
- snapshot generation still succeeds from the new source abstraction
- preview mode, if enabled, still behaves deterministically or is explicitly
  documented as using a separate fallback path

## Non-Goals

This document does not promise:

- legacy Windows GUI support
- a new wire protocol version
- bad-network transport redesign behavior

Those are separate tranches and must stay separate in docs and proof status.
