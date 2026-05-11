# Spec: v4 CDG repair-NACK rate limiting (desktop RX)

## Goal

Prevent **repair-NACK storms** when `missing_member_mask` evolves on every FEC observation while parity/media packets trickle in. Preserve the ability to **NACK immediately** when **new** group members become missing compared to the last queued NACK.

## Normative behavior

For each `struct dashcdg_rx_fec_group` used for CDG (`stream_type == CDG`):

Let:

- `M` = current missing member bitmask (indices 0..`expected_group_size-1`).
- `L` = `cdg_repair_nack_last_missing_mask` from the last **successfully queued** NACK for this group.
- `T` = `cdg_repair_nack_last_local_ms` from that same event.
- `Δ` = `DASHCDG_RX_REPAIR_NACK_COOLDOWN_MS` (120 ms default).

**Queue rule:** If `M == 0`, never NACK.

Otherwise, if `T != 0` and `now - T < Δ`:

- If `(M & ~L) == 0`, **do not queue** a NACK (current missing set is a subset of what we already reported).
- Else **queue** a NACK (there exists at least one newly missing index not covered by `L`).

If `now - T >= Δ` or `T == 0`, **queue** a NACK when `M != 0`.

**State update:** After a successful enqueue (`dashcdg_rx_send_v4_repair_nack_locked` returns true), set `T = now` and `L = M`.

**Logging:** Per-NACK `RX_OUT` lines are **off** unless `DASHCDG_RX_LOG_REPAIR_NACK` is set to a non-empty string. Counters in periodic `v4-stats` remain authoritative.

## Non-goals

- Changing wire format of `v4_repair_nack`.
- Replacing the legacy `(group_id, mask)` dedupe in `dashcdg_rx_send_v4_repair_nack_now` (kept as a second line of defense).

## Compatibility

Receivers built with this spec send **fewer** duplicate NACKs; transmitters already tolerate sparse NACKs and full-group storm rebroadcast logic is unchanged.
