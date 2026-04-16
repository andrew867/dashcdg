# CDG batch jitter — validation matrix

| ID | Scenario | Expected |
| --- | --- | --- |
| CJ-01 | `make test` | `test_core` exercises insert duplicate drop, drain APPLY + `note_applied`, late SKIP jump |
| CJ-02 | `apply_snapshot_seek` | Slots with `packet_start_index` below seek index cleared; cursor set |
| CJ-03 | Live RX (manual) | No regression vs prior reorder / late-skip counters in HUD |

## Host integration (desktop RX)

- All live CDG inserts go through `dashcdg_cdg_batch_jitter_insert`.
- Drain loop uses `dashcdg_cdg_batch_jitter_drain_step` + `note_applied` after
  `dashcdg_cdg_state_process_packet` for each sub-packet.
