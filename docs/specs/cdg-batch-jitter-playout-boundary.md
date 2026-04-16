# CDG batch jitter / playout boundary (normative)

## Scope

This document defines the **portable core** module that buffers **live CDG
subchannel packet batches** between the **transport/protocol layer** and the
**CDG state machine** (`dashcdg_cdg_state_process_packet`). It mirrors the
audio jitter contract in [`audio-jitter-playout-boundary.md`](audio-jitter-playout-boundary.md).

## Identity and ordering

- Each batch is addressed by **`packet_start_index`**: the absolute subchannel
  packet index of the **first** `dashcdg_subchannel_packet` in the batch (same
  semantics as `DASHCDG_PACKET_CDG_BATCH` / V4 video delta CDG mode).
- The playout cursor **`next_packet_index`** is advanced **only** when a batch
  is applied: it moves forward by **`packet_count`** (not always
  `DASHCDG_MAX_CDG_BATCH_PACKETS`).
- A batch is **eligible for drain** when `slot->packet_start_index ==
  jb->next_packet_index` (exact match). Batches are not spliced across the
  cursor; the sender aligns batch starts to the live timeline.

## Module

- Header: `core/include/dashcdg/cdg_batch_jitter.h`
- Implementation: `core/src/cdg_batch_jitter.c`

### Public operations

| Function | Role |
| --- | --- |
| `dashcdg_cdg_batch_jitter_init` / `clear` | Reset buffer |
| `dashcdg_cdg_batch_jitter_insert` | Store batch; optional reorder/drop stats |
| `dashcdg_cdg_batch_jitter_find` | Lookup by `packet_start_index` |
| `dashcdg_cdg_batch_jitter_oldest` | Minimum `packet_start_index` among occupied slots |
| `dashcdg_cdg_batch_jitter_occupied_count` | Pending batches |
| `dashcdg_cdg_batch_jitter_drain_step` | One-step drain: APPLY / SKIP / STOP |
| `dashcdg_cdg_batch_jitter_note_applied` | After host applies packets: advance cursor, free slot |
| `dashcdg_cdg_batch_jitter_apply_snapshot_seek` | Snapshot/anchor seek: purge stale slots, set cursor |

### Drain policy (late / gap)

When the next batch is missing and **late** conditions hold (host supplies
sender playback clock, grace window, and a gate bit equivalent to “bootstrap
complete” on the receiver), the buffer either **jumps** to the **oldest**
pending batch start or advances **`next_packet_index` by one nominal batch
stride** (`DASHCDG_MAX_CDG_BATCH_PACKETS`) when no older batch exists—matching
prior `app_rx.c` behavior.

## Host responsibilities

- **Apply path:** decode each `dashcdg_subchannel_packet` in order through
  `dashcdg_cdg_state_process_packet`, then call `note_applied`.
- **FEC recovery:** recovered payloads are inserted via the same `insert` API
  with `count_stats == 0`.
- **Snapshot / anchor:** call `apply_snapshot_seek` so pending batches before
  the new timeline position are dropped.

## Constants

- Slot count: **64** (same as audio jitter slot count).
- Payload cap: `DASHCDG_MAX_CDG_BATCH_PACKETS * DASHCDG_SUBCHANNEL_PACKET_BYTES`
  (from `protocol.h`).

## Related

- Architecture: [`../architecture/transport-and-playout-modules.md`](../architecture/transport-and-playout-modules.md)
- Audio analogue: [`audio-jitter-playout-boundary.md`](audio-jitter-playout-boundary.md)
