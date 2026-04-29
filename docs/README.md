# dashcdg documentation

Welcome. This folder is the **map**: architecture, wire formats, Windows build
matrices, soak plans, and the occasional postmortem written at 2 a.m. when UDP
did something rude.

**New here?** Read [`fork-manifesto.md`](fork-manifesto.md) (why this exists),
then [`CONTRIBUTING.md`](CONTRIBUTING.md) (build, tests, how to land changes).
For the latest release snapshot, see [`releases/v0.2.0.md`](releases/v0.2.0.md).

The **canonical** desktop Windows story (OpenGL vs GDI, retro, USB sneakernet)
lives in **`specs/desktop-platform-support.md`** and
**`specs/windows-legacy-mingw-build.md`**.

## Normative vs historical

- **Tables below** point to **current** architecture and specs you should follow
  when changing code.
- Files named `*-rca.md` (root cause analysis), `*-brief.md`, one-off soak notes,
  or `*-implementation-plan.md` are often **snapshots in time**. They stay in the
  tree for archaeology, but may not reflect *today’s* code — check the primary
  sources (`app_rx.c`, `app_tx.c`, `AGENTS.md`) when in doubt.
- **Enterprise group sync:** canonical spec/tests/tranches are under `specs/enterprise-group-sync-spec.md`, `test/enterprise-group-sync-test-plan.md`, and `plans/enterprise-group-sync-tranches.md`. Older “enterprise sync” master-plan files live in [`archive/enterprise-sync-masterplan-2026-04/`](archive/enterprise-sync-masterplan-2026-04/README.md); stub paths redirect here.

## Architecture

| Document | Contents |
| --- | --- |
| [`architecture/desktop-streaming.md`](architecture/desktop-streaming.md) | End‑to‑end TX/RX topology, jitter, snapshots, **OpenGL + Win32 GDI** receiver paths |
| [`architecture/tx-audio-isolation.md`](architecture/tx-audio-isolation.md) | **TX** audio domain mutex, batched v4 RX stats ingest, wire sequence, deadline logs (2026) |
| [`architecture/transport-and-playout-modules.md`](architecture/transport-and-playout-modules.md) | Module map (`libdashcdg_*`, desktop objects, UDP) |
| [`architecture/threaded-streaming-runtime.md`](architecture/threaded-streaming-runtime.md) | Thread / queue ownership for the desktop runtime (target vs implemented) |
| [`architecture/bad-network-startup-path.md`](architecture/bad-network-startup-path.md) | v4 “badnet” startup behavior |
| [`architecture/baseline-seams.md`](architecture/baseline-seams.md) | Portable vs desktop seams |
| [`architecture/portable-core.md`](architecture/portable-core.md) | Core/proto boundaries |

## Specifications (desktop & wire)

| Document | Contents |
| --- | --- |
| [**`specs/desktop-platform-support.md`**](specs/desktop-platform-support.md) | **Master matrix:** Windows/Linux targets, exe names, `Makefile` targets, zips, sneakernet layout |
| [`specs/win32-gdi-view-backend.md`](specs/win32-gdi-view-backend.md) | Win32 GDI window: DIBSection, `--win-gdi`, **`desktop-gdi-rx.exe`**, `win32_gdi_view.c` |
| [`specs/windows-legacy-mingw-build.md`](specs/windows-legacy-mingw-build.md) | PE/subsystem audit, XP/P3 profile, **retro** bundle, sneakernet zip, Opus DLL notes |
| [`specs/transport-protocol.md`](specs/transport-protocol.md) | Protocol v3/v4 fields |
| [`specs/bad-network-transport.md`](specs/bad-network-transport.md) | v4 transport overview |
| [`specs/v5-multistream-adaptation-architecture.md`](specs/v5-multistream-adaptation-architecture.md) | **V5** — simulcast, IGMP ladder, parallel decode (planned) |
| [`specs/v5-broadcast-rack-protocol-spec.md`](specs/v5-broadcast-rack-protocol-spec.md) | **V5 umbrella** — discovery, timeline, PTP GM, FEC/RS, telemetry, rack ops (target architecture) |
| [`plans/v5-broadcast-rack-implementation-tranches.md`](plans/v5-broadcast-rack-implementation-tranches.md) | **V5** — P0–P7 tranches with **files touched** |
| [`test/v5-broadcast-rack-test-plan.md`](test/v5-broadcast-rack-test-plan.md) | **V5** — `V5-BR-*` test IDs |
| [`specs/bad-network-audio-profiles.md`](specs/bad-network-audio-profiles.md) | Audio profiles (quality vs resilience / SBC‑like) |
| [`specs/cdg-batch-jitter-playout-boundary.md`](specs/cdg-batch-jitter-playout-boundary.md) | CDG batch jitter + snapshot deferral rules |
| [`specs/cpu-rgba-raster-contract.md`](specs/cpu-rgba-raster-contract.md) | `dashcdg_cdg_state_to_rgba8` contract (shared by GL and GDI) |
| [`specs/receiver-progress-invariants.md`](specs/receiver-progress-invariants.md) | RX progress / preroll invariants |
| [`specs/v4-display-audio-sync.md`](specs/v4-display-audio-sync.md) | TX preview delay, RX drain order, **DAC vs graphics** time |
| [`specs/tx-cdg-source-model.md`](specs/tx-cdg-source-model.md) | TX CDG source / memory model |
| [`specs/remaining-tranches-roadmap.md`](specs/remaining-tranches-roadmap.md) | **Remaining work index** (soak, track stress, bad-network phases, CD+G regression, observability) |
| [`specs/enterprise-group-sync-spec.md`](specs/enterprise-group-sync-spec.md) | **Enterprise** multi-receiver sync — residual spread, smoothed TX control, RX leader scaling, DAC trim, gates (normative) |
| [`plans/enterprise-group-sync-tranches.md`](plans/enterprise-group-sync-tranches.md) | **Enterprise** sync implementation order (closeout + residual R1–R8) |
| [`specs/bad-network-transport-next-phases.md`](specs/bad-network-transport-next-phases.md) | Bad-network transport **implementation phases** (companion) |
| [`specs/v4-video-repair-window-design.md`](specs/v4-video-repair-window-design.md) | On-the-fly video repair window math, budgets, and anchor-epoch rules (pre-implementation) |
| [`specs/operator-observability-and-sync-future-work.md`](specs/operator-observability-and-sync-future-work.md) | PTP / operator UI / metrics UI **future work** |
| [`specs/narrowband-low-bitrate-audio-quality.md`](specs/narrowband-low-bitrate-audio-quality.md) | NB quality hypotheses, acceptance, test plan (draft) |
| [`specs/tx-pause-screen.md`](specs/tx-pause-screen.md) | Pause screen packets |
| [`specs/embedded-rx-audio-profile.md`](specs/embedded-rx-audio-profile.md) | Embedded RX audio (planning) |
| [`specs/v4-rx-stats-embedded-extension.md`](specs/v4-rx-stats-embedded-extension.md) | **Planned** v4 RX stats extension — embedded buffer / clock / PTP / ADC (wire TBD) |
| [`specs/transport-udp-boundary.md`](specs/transport-udp-boundary.md) | UDP helper boundary |
| [`specs/audio-jitter-playout-boundary.md`](specs/audio-jitter-playout-boundary.md) | Audio jitter boundary |

## Tests & validation

| Document | Contents |
| --- | --- |
| [`test/win32-gdi-view-validation.md`](test/win32-gdi-view-validation.md) | **GDI RX** manual validation |
| [`test/cdg-batch-jitter-validation.md`](test/cdg-batch-jitter-validation.md) | CDG batch jitter tests |
| [`test/desktop-proof-plan.md`](test/desktop-proof-plan.md) | Desktop proof claims |
| [`test/desktop-impairment-validation.md`](test/desktop-impairment-validation.md) | Impaired network matrix |
| [`test/sample-media.md`](test/sample-media.md) | **`cdg/`** sample MP3+G library — purpose, release zip, redistribution note |
| [`test/portability-streaming-validation.md`](test/portability-streaming-validation.md) | Portability / streaming checklist |
| [`test/bad-network-transport-validation.md`](test/bad-network-transport-validation.md) | v4 transport tests |
| [`test/long-impairment-soak-validation.md`](test/long-impairment-soak-validation.md) | **Long** impaired soaks, log capture, burst threshold quantification |
| [`test/rapid-track-switch-pressure-validation.md`](test/rapid-track-switch-pressure-validation.md) | Rapid track changes under **sustained TX pressure** |
| [`test/tx-cdg-source-late-join-regression-plan.md`](test/tx-cdg-source-late-join-regression-plan.md) | **TX CD+G** source slimdown — late-join / switch regression |
| [`test/enterprise-group-sync-test-plan.md`](test/enterprise-group-sync-test-plan.md) | **Enterprise** group sync — environments, traceability (`EGS-*`), soak gates, release checklist |
| [`test/v5-broadcast-rack-test-plan.md`](test/v5-broadcast-rack-test-plan.md) | **V5 broadcast-rack** — `V5-BR-*` IDs (discovery, timeline, clock, FEC, telemetry, integration) |
| Other `test/*.md` | Feature‑specific plans and reports |

## Hardware, embedded & ops

- [`embedded/`](embedded/) — FreeRTOS / ESP32 handoff: Windows reference, v4 porting guide, task plans, codec matrix (`embedded/README.md` starts the tour).
- [`hardware/`](hardware/) — ESP32 boards, BOM, bringup (desktop v4 is still the **behavioral reference** for firmware).
- [`ops/quality-gates.md`](ops/quality-gates.md) — Release criteria
- [`ops/audio-codec-direction-notes.md`](ops/audio-codec-direction-notes.md) — operator/runtime codec decisions
- [`ops/v4-video-repair-implementation-checklist.md`](ops/v4-video-repair-implementation-checklist.md) — Tranche C.4-style execution checklist

## Meta

- [`CONTRIBUTING.md`](CONTRIBUTING.md) — how to build, test, and document
- [`../AGENTS.md`](../AGENTS.md) — agent / developer handoff for **desktop-rx** debugging
