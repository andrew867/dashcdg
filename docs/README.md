# dashcdg documentation

Start here for navigation. The **canonical** desktop Windows story (builds,
OpenGL vs GDI, retro, USB bundle) lives in **`specs/desktop-platform-support.md`**
and **`specs/windows-legacy-mingw-build.md`**.

**Why this fork exists** (short, opinionated): [`fork-manifesto.md`](fork-manifesto.md).

## Architecture

| Document | Contents |
| --- | --- |
| [`architecture/desktop-streaming.md`](architecture/desktop-streaming.md) | End‑to‑end TX/RX topology, jitter, snapshots, **OpenGL + Win32 GDI** receiver paths |
| [`architecture/transport-and-playout-modules.md`](architecture/transport-and-playout-modules.md) | Module map (`libdashcdg_*`, desktop objects, UDP) |
| [`architecture/threaded-streaming-runtime.md`](architecture/threaded-streaming-runtime.md) | Thread / queue ownership for the desktop runtime |
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
| [`specs/bad-network-audio-profiles.md`](specs/bad-network-audio-profiles.md) | Audio profiles (quality vs resilience / SBC‑like) |
| [`specs/cdg-batch-jitter-playout-boundary.md`](specs/cdg-batch-jitter-playout-boundary.md) | CDG batch jitter + snapshot deferral rules |
| [`specs/cpu-rgba-raster-contract.md`](specs/cpu-rgba-raster-contract.md) | `dashcdg_cdg_state_to_rgba8` contract (shared by GL and GDI) |
| [`specs/receiver-progress-invariants.md`](specs/receiver-progress-invariants.md) | RX progress / preroll invariants |
| [`specs/tx-cdg-source-model.md`](specs/tx-cdg-source-model.md) | TX CDG source / memory model |
| [`specs/remaining-tranches-roadmap.md`](specs/remaining-tranches-roadmap.md) | **Remaining work index** (soak, track stress, bad-network phases, CD+G regression, observability) |
| [`specs/bad-network-transport-next-phases.md`](specs/bad-network-transport-next-phases.md) | Bad-network transport **implementation phases** (companion) |
| [`specs/v4-video-repair-window-design.md`](specs/v4-video-repair-window-design.md) | On-the-fly video repair window math, budgets, and anchor-epoch rules (pre-implementation) |
| [`specs/operator-observability-and-sync-future-work.md`](specs/operator-observability-and-sync-future-work.md) | PTP / operator UI / metrics UI **future work** |
| [`specs/narrowband-low-bitrate-audio-quality.md`](specs/narrowband-low-bitrate-audio-quality.md) | NB quality hypotheses, acceptance, test plan (draft) |
| [`specs/tx-pause-screen.md`](specs/tx-pause-screen.md) | Pause screen packets |
| [`specs/embedded-rx-audio-profile.md`](specs/embedded-rx-audio-profile.md) | Embedded RX audio (planning) |
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
| Other `test/*.md` | Feature‑specific plans and reports |

## Hardware & ops

- [`embedded/`](embedded/) — embedded/FreeRTOS handoff set: Windows desktop reference, protocol v4 porting guide, ESP32 task plan, codec/rendering matrix
- [`hardware/`](hardware/) — ESP32 / BOM / bringup notes (desktop **v4** A/V + codec switching is the reference contract for firmware — see [`AGENTS.md`](../AGENTS.md), [`specs/embedded-rx-audio-profile.md`](specs/embedded-rx-audio-profile.md))  
- [`ops/quality-gates.md`](ops/quality-gates.md) — Release criteria  
- [`ops/audio-codec-direction-notes.md`](ops/audio-codec-direction-notes.md) — operator/runtime codec decisions, including the EVRC retirement note and weird-codec backlog
- [`ops/v4-video-repair-implementation-checklist.md`](ops/v4-video-repair-implementation-checklist.md) — TX/RX/proto/test execution slices for Tranche C.4
