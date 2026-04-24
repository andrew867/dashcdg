# V4 video repair implementation checklist (sliced)

## Purpose

Turn `v4-video-repair-window-design.md` into reviewable implementation slices
before coding starts. This is the execution checklist for Tranche C.4 / Phase P3.5.

## Slice 0 — Contract freeze (no runtime changes)

- [ ] Confirm packet/wire strategy is additive and backward compatible.
- [ ] Freeze window constants for first pass (`k`, `W_ms`, cadence mode).
- [ ] Freeze anchor epoch semantics:
  - [ ] applied anchor defines a new repair epoch
  - [ ] pre-epoch windows are discarded
  - [ ] anchor establishes known-good clear+palette baseline semantics.
- [ ] Update references in:
  - [ ] `v4-live-video-playout.md`
  - [ ] `bad-network-transport-next-phases.md`
  - [ ] `remaining-tranches-roadmap.md`

## Slice 1 — Proto and data model scaffolding

- [ ] Define/extend payload descriptors for video repair symbols.
- [ ] Add parse/serialize coverage for repair packets.
- [ ] Add explicit group/window identifiers and member indexing.
- [ ] Keep old parsers safe (ignore unknown/additive fields as intended).

## Slice 2 — TX emission path

- [ ] Implement forward/reverse symbol scheduling (alternating cadence first).
- [ ] Add bounded symbol payload cap and guard rails.
- [ ] Ensure no tick monopolizes send budget (respect existing scheduler fairness).
- [ ] Add counters:
  - [ ] emitted_forward
  - [ ] emitted_reverse
  - [ ] emit_skipped_budget.

## Slice 3 — RX ingest and solve path (metrics-only first)

- [ ] Add bounded repair-window pool with static memory budget.
- [ ] Track member bitmaps and deadlines.
- [ ] Parse/store symbols and candidates; no apply yet.
- [ ] Counters:
  - [ ] windows_opened
  - [ ] windows_expired
  - [ ] windows_unsolved.

## Slice 4 — RX apply path (full recovery)

- [ ] Enable one-miss solve and recovered-delta injection into existing jitter path.
- [ ] Reject rewinds behind `next_packet_index`.
- [ ] Preserve existing skip/anchor fallback when unsolved.
- [ ] Counters:
  - [ ] solved_forward
  - [ ] solved_reverse
  - [ ] rejected_rewind
  - [ ] apply_failed.

## Slice 5 — Anchor/prelude determinism hardening

- [ ] Verify anchor epochs always restore known-good visual baseline.
- [ ] For compact anchors (if introduced), enforce semantic ordering:
  - [ ] clear baseline
  - [ ] palette/transparency baseline
  - [ ] block/tile paint data.
- [ ] Ensure repair windows cannot bridge across anchor epochs.

## Slice 6 — Validation and soak

- [ ] Unit tests:
  - [ ] one-miss solvable windows
  - [ ] multi-miss unsolved windows
  - [ ] deadline expiry behavior
  - [ ] epoch reset behavior on anchor apply.
- [ ] Integration tests:
  - [ ] `v4-transport-reliability-validation.md` case 6
  - [ ] `tx-cdg-source-late-join-regression-plan.md` LJ-7.
- [ ] Embedded runs:
  - [ ] ESP32 heap high-water check
  - [ ] no new black-frame/freeze regressions
  - [ ] palette-dependent intro correctness.

## Exit criteria for first merge tranche

- [ ] Single-loss repair success meets target in controlled profile.
- [ ] No regression in late join/track-switch stability.
- [ ] No persistent wrong-palette state after induced early-loss scenarios.
- [ ] Docs/tests are updated with actual counters and thresholds.

## Related

- `../specs/v4-video-repair-window-design.md`
- `../specs/v4-live-video-playout.md`
- `../specs/remaining-tranches-roadmap.md`
- `../specs/bad-network-transport-next-phases.md`
- `../test/v4-transport-reliability-validation.md`
- `../test/tx-cdg-source-late-join-regression-plan.md`
