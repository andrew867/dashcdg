# Remaining tranches plan (excluding ESP32 audio)

Purpose: execution order and completion criteria after Tranche B and Tranche D, with ESP32 audio work deferred.

## Scope excluded

- ESP32 codec/audio decode bring-up and ESP32 audio quality work.

## Execution order

1. Finish Tranche B (group playout sync/IDMS control rollout).
2. Finish Tranche D (D.1 TX CDG/source late-join, D.2 narrowband quality closure, D.4 operator metrics/UI).
3. Finish Tranche C hardening and soak closure.
4. Start Tranche E (v5/multistream architecture and controlled rollout plan).

## Tranche C completion targets

### C.1 long impaired-network soaks

- Run repeatable impairment matrix with archived TX/RX logs.
- Require no permanent silent wedges and no persistent snapshot-only video regressions.

### C.2 rapid track-switch pressure

- Validate repeated next/back/auto-advance under active load.
- Require deterministic recovery and no cumulative drift across playlist.

### C.3 transport + profile hardening

- Validate bad-network transport adaptation stability under mixed receivers.
- Require bounded adaptation response and no oscillation under outlier flaps.

### C.4 on-the-fly video repair windows

- Validate additive repair behavior and clean fallback when unresolved.
- Preserve anchor epoch determinism and startup visual correctness.

## Tranche E completion targets

### E.1 v5 architecture freeze

- Freeze packet model for multistream/ladder and compatibility policy with v4.
- Define migration/interop rules and fallback behavior.

### E.2 implementation pilot

- Implement minimal v5 pilot path behind feature flag.
- Verify no v4 regressions with feature disabled.

### E.3 rollout readiness

- Document operator policy for enabling v5 and rollback triggers.
- Produce soak and compatibility report.

## Required artifacts before each tranche close

- Updated spec deltas in `docs/specs/`.
- Updated validation checklist in `docs/test/`.
- Soak evidence bundle (logs + summary).
- Explicit pass/fail table by case id and build/commit.
- Source provenance matrix showing at least one `repo_local`, one `external_local_disk`, and one `network_mounted` run.
