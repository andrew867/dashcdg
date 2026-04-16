# Audio jitter / playout boundary — validation matrix

## Unit tests (`make test`)

Implemented in `tests/test_core.c` (or dedicated test file linked into `test-core`).

| ID | Scenario | Pass criteria |
| --- | --- | --- |
| AJ-01 | Fresh init | `initialized == 0`; insert sets `next_media_sequence` to first seq. |
| AJ-02 | In-order insert 3 frames | `find(seq)` returns each; `occupied_count == 3`. |
| AJ-03 | Duplicate seq | Second insert returns `0`; `pending_drops` increments. |
| AJ-04 | Late seq (below next) | Insert returns `0` when `initialized` and seq < next. |
| AJ-05 | Reorder (higher seq before drain) | Insert allowed; `reordered_packets` increments when `count_stats`. |
| AJ-06 | Drain APPLY | After insert seq N, `drain_step` with permissive input returns APPLY with frame N; after `after_apply`, next is N+1 and slot empty. |
| AJ-07 | Drain SKIP (forced late) | With empty slot at next seq but `have_sender_playback` and late clock, returns SKIP and advances `next_media_sequence`. |

## Integration (manual)

| ID | Steps | Expected |
| --- | --- | --- |
| I-AJ-01 | RX quality profile 20 min | No audio gate regression; HUD drop/reorder counts match order of magnitude vs pre-refactor baseline. |

## Invariants checked by tests

- `payload_length` clamp
- No slot leak after apply (occupied count decreases)
