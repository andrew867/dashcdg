# Implementation plan: transport, audio jitter, CPU RGBA raster

## Preconditions

- Specs: `docs/specs/transport-udp-boundary.md`, `docs/specs/audio-jitter-playout-boundary.md`, `docs/specs/cpu-rgba-raster-contract.md`
- Architecture: `docs/architecture/transport-and-playout-modules.md`
- Test plans: `docs/test/*-validation.md` for each area

## Milestone A — Documentation only (commit 1)

- Land all spec/test/architecture documents above
- Update `docs/architecture/portable-core.md` “Future extraction steps” with links and status

## Milestone B — Core jitter + raster + tests (commit 2)

- Add `core/src/audio_jitter.c`, `core/include/dashcdg/audio_jitter.h`
- Add `core/src/cdg_raster.c`, `core/include/dashcdg/cdg_raster.h`
- Extend `Makefile` `CORE_SOURCES` / `CORE_OBJECTS`
- Extend `tests/test_core.c` with AJ-* and RZ-* cases
- `make test` passes

## Milestone C — Desktop transport + RX wiring + GL (commit 3)

- Add `platform/desktop/src/transport_udp.c` + header; extend `DESKTOP_COMMON_OBJECTS`
- Refactor `app_rx.c` to embed `struct dashcdg_audio_jitter_buffer` and remove duplicate jitter logic
- Refactor `gl_renderer.c` to rasterize via `dashcdg_cdg_state_to_rgba8` and simplified shader path
- `make debug` builds `desktop-rx` / `desktop-tx` / `desktop-player`

## Exit criteria

- No TODO stubs in new modules
- `make test` green
- Docs and code agree on dimensions (`DASHCDG_CDG_RGBA_BYTES`)

## Deferred (explicitly out of this plan)

- CDG batch jitter extraction to core (same pattern as audio; future spec)
- Win32 GDI window backend (Phase 2 of `windows_retro_graphics_backend.plan.md`)
