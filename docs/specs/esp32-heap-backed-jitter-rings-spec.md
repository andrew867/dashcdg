# Spec: ESP32 heap-backed variable-capacity jitter rings

## Status

- Draft: ready for implementation review
- Scope: `platform/espidf/projects/dashcdg_badge` receiver path
- Non-goal: no wire protocol changes

## Problem statement

Current ESP32 jitter buffers are fixed-size compile-time structs (`DASHCDG_AUDIO_JITTER_SLOT_COUNT`, `DASHCDG_CDG_BATCH_JITTER_SLOT_COUNT`). Mode toggles can free related heaps, but slot capacity cannot be reassigned dynamically between audio and video workloads.

Goal: replace fixed-size rings with heap-backed variable-capacity rings so runtime modes can allocate buffer capacity where it matters most:

- audio on + video off -> maximize audio ring depth
- video on + audio off -> maximize CDG ring depth
- both on -> balanced profile
- both off -> release everything possible

## Functional requirements

1. Keep the same v4 packet parsing and timing semantics.
2. Preserve existing drain contracts:
   - `APPLY`, `STOP`, `SKIP` behavior
   - late-gate and stall-loss logic
3. Keep decode toggles hot-switchable at runtime with no reboot.
4. Reconfigure capacity online with bounded pause (< 1 frame target, < 40 ms hard cap).
5. Maintain deterministic max memory budget using profile caps.
6. Report live allocation and effective capacities in modal stats.

## Architecture

### New core model

Introduce heap-backed ring modules for both media types:

- `audio_jitter_heap` (ESP32 first; desktop later optional)
- `cdg_jitter_heap` (ESP32 first; desktop later optional)

Each uses:

- slot metadata array (`occupied`, sequence/index, playback_ms, payload pointer, payload_len)
- payload pool allocator (fixed-size chunks or slab classes)
- free list + occupied map

### Data structure shape

- Ring capacity `N` is runtime-configured.
- Payload storage is separate from slot metadata.
- Slot metadata must be contiguous for predictable scan/drain.
- Payload buffers may come from:
  - single fixed chunk size (simple)
  - tiered chunk classes (better fit)

Recommended v1: fixed chunk size per ring (audio chunk <= 255B; CDG chunk <= max batch payload), with strict pool cap.

## Memory profiles

Define runtime profiles:

- `PROFILE_BALANCED`
- `PROFILE_AUDIO_PRIORITY`
- `PROFILE_VIDEO_PRIORITY`
- `PROFILE_MINIMAL` (both decode off)

Each profile sets:

- audio slot capacity
- video slot capacity
- payload pool bytes per ring
- headroom/eviction reserve policy

Example policy intent (numbers finalized during bring-up):

- balanced: 50/50 budget split
- audio-priority: 70/30 split favoring audio
- video-priority: 30/70 split favoring CDG
- minimal: free both rings and heavy transients

## Concurrency and ownership

- Single writer model remains: `badge_rx_task` owns insert/drain.
- No extra mutexes around ring internals beyond current `s_mtx` envelope.
- Reprofile/realloc operation must happen under controlled quiescent point:
  - stop new insert
  - snapshot/drop remaining slots per policy
  - rebuild ring structures
  - resume ingest/drain

## Migration and compatibility

1. Keep existing fixed-ring code behind compile flag during migration.
2. Add adapter layer so current call sites (`insert`, `drain_step`, `note_applied`, `occupied_count`) stay stable.
3. Gate rollout with `CONFIG_DASHCDG_BADGE_HEAP_JITTER_RINGS`.

No protocol changes, no sender changes required.

## Failure handling rules

- Allocation failure on profile switch:
  - keep previous profile alive
  - emit one-shot error counter/log
- Mid-stream OOM during insert:
  - bounded eviction (furthest-ahead first for video)
  - increment drop counters
  - never crash or block indefinitely
- Decoder off:
  - ring can be fully deallocated in minimal/audio/video-off profiles

## Telemetry requirements

Expose in modal and v4 stats extension (where possible):

- ring capacity (audio/video)
- occupied slots
- payload pool used/free
- alloc fail count
- reprofiles count
- profile active enum
- eviction count by reason

## Performance targets

- No regression in current soak stability.
- Stats bus must continue publishing under load.
- Video reliability under `video-priority` improves vs current fixed ring at same memory ceiling.
- Audio continuity under `audio-priority` improves vs current fixed ring at same memory ceiling.

## Out of scope

- Multi-task split of router/drain in this phase
- Protocol redesign
- Desktop conversion in first cut
