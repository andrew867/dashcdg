# ESP32 badge FreeRTOS executive readiness checklist

## How to use

Each tranche pull request and the final release pull request must satisfy the relevant sections. Anything not satisfied requires a waiver entry in `esp32-badge-freertos-executive-refactor-spec.md` and `esp32-badge-freertos-hazard-analysis.md`.

## Design review

- [ ] Tranche scope matches `esp32-badge-freertos-refactor-implementation-tranches.md`.
- [ ] New tasks/timers/queues/notifications are documented in the inventory.
- [ ] Boot event facts and degraded reasons are listed.
- [ ] Owner task for each modified aggregate is identified.
- [ ] No mutex is held across slow I/O in the new code.

## Code review hard stops

- [ ] `portMAX_DELAY` only appears in waived locations.
- [ ] No `lv_*` call inside `s_mtx` for RX or HW.
- [ ] No `nvs_*` call inside hot-path or ESP event context.
- [ ] No `dac_continuous_write` while RX `s_mtx` held.
- [ ] No `heap_caps_*` walk inside hot mutex regions.
- [ ] ESP event handlers do not allocate, log heavily, or perform UI/NVS/RX mutation.
- [ ] ISR callbacks only use `*FromISR` APIs, atomics, or task notification.

## Test evidence required

- [ ] `BLD-01` clean build artifact attached.
- [ ] Tranche-specific test IDs executed and logs attached.
- [ ] Forbidden-pattern grep report attached.
- [ ] Counter/snapshot diffs between baseline and post-refactor attached.
- [ ] Soak runs attached when required by tranche.

## Artifact checklist

- [ ] Boot trace for nominal, degraded, and fault-injected runs.
- [ ] Hot-path timing capture.
- [ ] IPC pressure capture under stress.
- [ ] Health transitions across degraded entries and recoveries.

## Release candidate checks

- [ ] All tranches T1-T10 completed or waived.
- [ ] No unresolved hazard from `esp32-badge-freertos-hazard-analysis.md`.
- [ ] All test IDs in `esp32-badge-freertos-executive-test-plan.md` pass or have waivers.
- [ ] Master index status table reflects shipped state.
- [ ] WDT enforcement on with documented liveness ages.
- [ ] Production telemetry rate confirmed under Wi-Fi/UART load.
