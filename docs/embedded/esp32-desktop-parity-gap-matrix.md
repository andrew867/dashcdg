# ESP32 vs Desktop Parity Gap Matrix

## Purpose

Track reliability and sync gaps between desktop RX/TX runtime behavior and badge RX behavior, with concrete acceptance criteria for each closure item.

## Priority gaps

| ID | Gap | Current badge state | Target parity | Acceptance criteria |
| --- | --- | --- | --- | --- |
| ESP-P1 | Clock sync control parse | `V4_CLOCK_SYNC.startup_state` control bits not fully consumed | Parse mode/target/spread and publish in receiver stats | Badge stats show non-zero mode/target/spread when TX emits syncctrl |
| ESP-P2 | Pause/unpause rearm | Resume transitions do not explicitly re-prime audio path | Re-prime DAC/jitter state on paused->running edge | Resume does not stay muted/stale after pause cycles |
| ESP-P3 | Session stale media guard | Prior-session packets may still be ingested around track transitions | Drop stale media based on session epoch envelope and sender time | No cross-track bleed under rapid next/back tests |
| ESP-P4 | Drift telemetry | `drift_trim_ppm` stays zero | Report bounded drift estimate from clock skew EMA | TX receives non-zero drift telemetry and trend is stable |
| ESP-P5 | Recovery telemetry parity | recovery counters are zero placeholders | Track and report local recovery triggers and source-idle style events | Stats include non-zero counters when recovery paths run |
| ESP-P6 | Repair control robustness | Repair NACK is synchronous in RX context | Keep send throttled and non-disruptive under loss bursts | No decode starvation under sustained repair bursts |
| ESP-P7 | Audio codec transition observability | Unsupported codec only increments a generic counter | Explicitly detect announced-vs-frame codec mismatch and resync decoder state | Codec transitions no longer wedge decode state; mismatch counters visible |

## Implementation order

1. Clock/session correctness (`ESP-P1`, `ESP-P2`, `ESP-P3`)
2. Audio continuity + codec transition handling (`ESP-P7`)
3. Telemetry parity (`ESP-P4`, `ESP-P5`)
4. Control-plane robustness (`ESP-P6`)

## Validation checklist

- Mixed receiver run (desktop + badge): verify sync control fields propagate and badge reports them.
- Rapid track change run: verify stale media drops and no epoch bleed.
- Pause/resume stress: verify audio re-prime and recovery counters.
- Impairment run with repair bursts: verify NACK throttle behavior and no prolonged decode starvation.
