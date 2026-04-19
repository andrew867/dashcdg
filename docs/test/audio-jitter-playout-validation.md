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
| AJ-07 | Drain SKIP (forced late) | With empty slot at next seq, **skew** ≥ `DASHCDG_AUDIO_SKIP_EMPTY_MIN_SKEW_MS`, `have_sender_playback`, preroll gate — returns SKIP and advances `next_media_sequence`. |
| AJ-08 | No ghost skip (join skew) | After one frame, missing next with **small** sender-vs-slot skew (< min skew), buffer empty — returns **STOP**, sequence unchanged (`test_audio_jitter_drain_no_ghost_skip_small_clock_skew`). |
| AJ-09 | No cold-start empty skip before first decode | After startup or session reset, if the buffer is not decode-primed, empty-buffer and stall-loss skip paths return **STOP** even under large sender skew (`test_audio_jitter_empty_skip_blocked_until_primed_decode`). |
| AJ-10 | Sender playback honors announced preroll | Drain subtracts `announced_playout_delay_ms` from sender playback before late checks, so a frame is not treated as late merely because the sender is one preroll ahead (`test_audio_jitter_sender_playback_respects_announced_preroll`). |

## Integration (manual)

| ID | Steps | Expected |
| --- | --- | --- |
| I-AJ-01 | RX quality profile 20 min | No audio gate regression; HUD drop/reorder counts match order of magnitude vs pre-refactor baseline. |

## Invariants checked by tests

- `payload_length` clamp
- No slot leak after apply (occupied count decreases)
- Cold-start / post-reset drain cannot advance `next_media_sequence` before the first successful decode in the current jitter session
