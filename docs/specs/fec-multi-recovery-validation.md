## Multi-Symbol FEC Validation Matrix

This matrix verifies that desktop RX and badge RX recover the same number of missing CDG batches for the same repair-window metadata.

### Build/Runtime Preconditions

- TX uses v4 transport and `--tx-cdg-fec-level` in `1..5`.
- RX peers emit v4 stats generation 4.
- TX summary line includes `fec=Lx p75=...`.
- Repair symbol count `K` is encoded in repair-window reserved bits and consumed by RX.

### Soak Cases

1. Baseline no impairment
- Expect near-zero `cdg_fec_failed`.
- `cdg_fec_recovered` may be non-zero but low.

2. Random drop 10-20%
- Expect level 2-3 to recover majority of CDG continuity gaps.
- Badge and desktop recovered/failed trend should match within 10%.

3. Random drop 35-50%
- Expect adaptive climb to level 4-5.
- Recovery succeeds when per-group erasures <= advertised `K`.

4. Burst loss (5-20 packet bursts)
- Expect visible degradation if burst erasures exceed group recoverability.
- Verify adaptive increase and retransmit duplicates reduce failed groups over time.

5. Mixed reorder + drop
- Verify loss display remains bounded `0..100%`.
- Validate reorder does not create impossible loss percentages.

### Pass/Fail Criteria

- No RX crashes on XP/P3 legacy target.
- `cdg_fec_failed / (cdg_fec_recovered + cdg_fec_failed)` decreases as FEC level increases in same network profile.
- Badge and desktop report consistent recoverability trends.
- No impossible UI loss values above 100%.
