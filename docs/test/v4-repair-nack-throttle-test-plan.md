# Test plan: v4 CDG repair-NACK throttle (desktop RX)

## Preconditions

- Two hosts or one TX + one RX on LAN, v4 transport, CDG FEC enabled (typical soak config).
- RX build includes per-group throttle + optional `DASHCDG_RX_LOG_REPAIR_NACK`.

## T1 — Baseline (no synthetic loss)

**Steps:** Play a 10+ minute track; capture RX `v4-stats` every 2 s.

**Pass:** `nack_tx` remains low (order of tens per session, not thousands per minute). RX log file growth dominated by `v4-stats` lines, not `repair-nack`.

## T2 — Mask evolution without new loss

**Steps:** Instrument or log internally (optional dev build): for a single `group_id`, assert that when `missing_mask` strictly decreases within 120 ms, **no** additional NACK is queued.

**Pass:** At most one NACK per group per cooldown window unless mask gains new bits.

## T3 — New loss inside cooldown

**Simulate:** If a harness can drop a **new** member after an initial NACK (mask gains a bit not present in prior `L`), RX must enqueue a new NACK even if `<120 ms` since last.

**Pass:** `(M & ~L) != 0` path fires; recovery or further NACK after cooldown continues.

## T4 — Queue backpressure

**Simulate:** Force `DASHCDG_RX_NACK_QUEUE_CAPACITY` exhaustion (stress tool, optional).

**Pass:** Throttle fields do **not** advance on drop; `nack_queue_dropped` increments (existing counter). Session eventually recovers without infinite log growth.

## T5 — Logging default

**Steps:** Run RX without `DASHCDG_RX_LOG_REPAIR_NACK`.

**Pass:** No `[rx] repair-nack:` lines in log; with env set, lines appear.

## T6 — Long soak regression

**Steps:** 45–60 min playlist with TX group-sync and two RX peers (match soak23 topology).

**Pass:** No video freeze; TX `cdg_worst_negative_lead_ms` stays within expected starve-bypass range (not runaway tens of seconds correlated with RX log size).
